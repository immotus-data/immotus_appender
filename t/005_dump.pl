# pg_dump / pg_restore / pg_basebackup of appender tables.
#
# pg_dump emits SET default_table_access_method = appender before the table
# and COPYs the rows through the access method, so a dump must restore into a
# fresh cluster with identical rows.  pg_basebackup is file-level and does not
# know segment files at all: it must still end up with the committed rows.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $conf = "shared_preload_libraries = 'immotus_appender'\n";

my $src = PostgreSQL::Test::Cluster->new('dump_src');
$src->init(allows_streaming => 1);
$src->append_conf('postgresql.conf', $conf);
$src->start;

$src->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');
$src->safe_psql('postgres', <<'SQL');
CREATE TABLE t (a int, b text, c timestamptz DEFAULT now()) USING appender;
INSERT INTO t (a, b) SELECT g,
       CASE WHEN g % 7 = 0 THEN NULL ELSE 'v' || g END
  FROM generate_series(1, 20000) g;
ALTER TABLE t ADD COLUMN d int DEFAULT 9;
INSERT INTO t (a) VALUES (30000);
SQL
# more than one segment, across a savepoint boundary that did not commit
$src->safe_psql('postgres',
	"SET immotus_appender.segment_size = '1MB';
	 BEGIN;
	 INSERT INTO t (a, b) SELECT g, repeat('x', 100)
	   FROM generate_series(1, 20000) g;
	 SAVEPOINT s;
	 INSERT INTO t (a) VALUES (-1);
	 ROLLBACK TO s;
	 COMMIT");

my $check = "SELECT count(*), sum(a), count(b),
	     md5(string_agg(COALESCE(b, '<null>'), ',' ORDER BY a)),
	     min(c)::text, max(c)::text, sum(d) FROM t";
my $expected = $src->safe_psql('postgres', $check);

my $dumpdir = $src->backup_dir;
my $custom = "$dumpdir/t.dump";
my $plain  = "$dumpdir/t.sql";

# --- pg_dump custom format -> pg_restore ----------------------------------

$src->command_ok(
	[ 'pg_dump', '-Fc', '-d', 'postgres', '-f', $custom ],
	'pg_dump -Fc runs');

my $restored = PostgreSQL::Test::Cluster->new('dump_restored');
$restored->init;
$restored->append_conf('postgresql.conf', $conf);
$restored->start;
$restored->command_ok(
	[ 'pg_restore', '--no-owner', '-d', 'postgres', $custom ],
	'pg_restore runs');
is($restored->safe_psql('postgres', $check),
	$expected, 'pg_restore brings back identical rows');
is($restored->safe_psql('postgres',
		"SELECT count(*) FROM pg_class WHERE relname = 't' AND relam = "
		. "(SELECT oid FROM pg_am WHERE amname = 'appender')"),
	'1', 'the restored table still uses the appender access method');
$restored->safe_psql('postgres',
	"INSERT INTO t (a) VALUES (40000)");
is($restored->safe_psql('postgres', "SELECT count(*) FROM t WHERE a = 40000"),
	'1', 'the restored table accepts appends');
$restored->stop;

# --- pg_dump plain SQL -> psql ---------------------------------------------

$src->command_ok(
	[ 'pg_dump', '-Fp', '-d', 'postgres', '-f', $plain ],
	'pg_dump -Fp runs');

my $psqld = PostgreSQL::Test::Cluster->new('dump_psql');
$psqld->init;
$psqld->append_conf('postgresql.conf', $conf);
$psqld->start;
$psqld->command_ok(
	[ 'psql', '-X', '-q', '-d', 'postgres', '-f', $plain ],
	'psql replays the plain dump');
is($psqld->safe_psql('postgres', $check),
	$expected, 'a plain SQL dump restores identical rows');
$psqld->stop;

# --- pg_basebackup ---------------------------------------------------------

$src->safe_psql('postgres',
	"INSERT INTO t (a, b) SELECT g, 'post-backup' FROM generate_series(1, 100) g");
my $expected_bb = $src->safe_psql('postgres', $check);

# Cluster::backup runs pg_basebackup under the hood
$src->backup('bb');
my $bb = PostgreSQL::Test::Cluster->new('dump_bb');
$bb->init_from_backup($src, 'bb');
$bb->start;
is($bb->safe_psql('postgres', $check),
	$expected_bb, 'a base backup has the committed rows');
is($bb->safe_psql('postgres',
		"SELECT count(*) FROM t WHERE a = 30000 OR b IS NULL"),
	'2858', 'the base backup has the nulls and the added-column default');
$bb->safe_psql('postgres', "INSERT INTO t (a) VALUES (50000)");
is($bb->safe_psql('postgres', "SELECT count(*) FROM t WHERE a = 50000"),
	'1', 'the restored-from-backup cluster accepts appends');
$bb->stop;

done_testing();
