# Point-in-time recovery, incremental backups and pg_rewind with appender
# tables.  These tools do not know segment files: they must still end up
# with exactly the committed rows.

use strict;
use warnings FATAL => 'all';

use File::Copy;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $conf = "shared_preload_libraries = 'immotus_appender'\n";

# --- point-in-time recovery ------------------------------------------------

my $node = PostgreSQL::Test::Cluster->new('pitr');
$node->init(has_archiving => 1, allows_streaming => 1);
$node->append_conf('postgresql.conf', $conf . "summarize_wal = on\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');
$node->safe_psql('postgres', 'CREATE TABLE t (a int, b text) USING appender');
$node->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'first' FROM generate_series(1, 1000) g");
$node->backup('full');

$node->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'second' FROM generate_series(1, 2000) g");
$node->safe_psql('postgres', "SELECT pg_create_restore_point('target')");
$node->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'third' FROM generate_series(1, 3000) g");
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->backup('incr', backup_options => [ '--incremental',
	$node->backup_dir . '/full/backup_manifest' ]);
$node->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'fourth' FROM generate_series(1, 4000) g");
my $last_wal = $node->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_lsn())');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->poll_query_until('postgres',
	"SELECT '$last_wal' <= last_archived_wal FROM pg_stat_archiver")
  or die 'WAL archiving did not catch up';

my $restored = PostgreSQL::Test::Cluster->new('restored');
$restored->init_from_backup($node, 'full', has_restoring => 1);
$restored->append_conf('postgresql.conf',
	"recovery_target_name = 'target'\nrecovery_target_action = 'promote'\n");
$restored->start;
$restored->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die 'recovery did not finish';
is( $restored->safe_psql('postgres',
		"SELECT string_agg(b || '=' || n, ',' ORDER BY b) FROM (SELECT b, count(*) n FROM t GROUP BY b) s"),
	'first=1000,second=2000',
	'recovery to a restore point replays appends up to the target only');
$restored->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'new' FROM generate_series(1, 10) g");
is($restored->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'3010', 'the recovered cluster accepts inserts');
$restored->stop;

# --- incremental backup ----------------------------------------------------

my $combined = PostgreSQL::Test::Cluster->new('combined');
$combined->init_from_backup($node, 'incr', combine_with_prior => ['full']);
$combined->start;
is( $combined->safe_psql('postgres',
		"SELECT string_agg(b || '=' || n, ',' ORDER BY b) FROM (SELECT b, count(*) n FROM t GROUP BY b) s"),
	'first=1000,second=2000,third=3000',
	'a combined incremental backup has every appended row');
$combined->stop;

# --- pg_rewind -------------------------------------------------------------

my $primary = PostgreSQL::Test::Cluster->new('rw_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	$conf . "wal_log_hints = on\nwal_keep_size = '320MB'\n");
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');
$primary->safe_psql('postgres', 'CREATE TABLE t (a int, b text) USING appender');
$primary->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'shared' FROM generate_series(1, 1000) g");
$primary->safe_psql('postgres', 'CHECKPOINT');
$primary->backup('rw');

my $standby = PostgreSQL::Test::Cluster->new('rw_standby');
$standby->init_from_backup($primary, 'rw', has_streaming => 1);
$standby->start;
$primary->wait_for_catchup($standby);

$standby->promote;
$standby->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'new timeline' FROM generate_series(1, 500) g");
$standby->safe_psql('postgres', 'CHECKPOINT');

# the old primary diverges and is then rewound onto the new one
$primary->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'diverged' FROM generate_series(1, 700) g");
$primary->stop;

# pg_rewind copies the source's configuration; keep the target's own
my $conf_file = $primary->data_dir . '/postgresql.conf';
my $conf_keep = $primary->basedir . '/postgresql.conf.keep';
copy($conf_file, $conf_keep) or die "could not copy $conf_file: $!";

command_ok(
	[
		'pg_rewind', '--debug',
		'--source-server' => $standby->connstr('postgres'),
		'--target-pgdata' => $primary->data_dir,
		'--no-sync',
	],
	'pg_rewind runs');

move($conf_keep, $conf_file) or die "could not restore $conf_file: $!";
$primary->enable_streaming($standby);
$primary->start;
$standby->wait_for_catchup($primary);
is( $primary->safe_psql('postgres',
		"SELECT string_agg(b || '=' || n, ',' ORDER BY b) FROM (SELECT b, count(*) n FROM t GROUP BY b) s"),
	'new timeline=500,shared=1000',
	'a rewound node has the new timeline rows and none of its own');

done_testing();
