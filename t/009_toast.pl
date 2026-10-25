# External values in segment-local value blocks must survive crash recovery
# and replay correctly on a standby.  Value blocks travel through the same
# XLOG_APPENDER_BLOCK records as row blocks.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use Test::More;

my $conf = "shared_preload_libraries = 'immotus_appender'\n";

# --- crash recovery --------------------------------------------------------

my $node = PostgreSQL::Test::Cluster->new('toast_crash');
$node->init;
$node->append_conf('postgresql.conf', $conf);
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');
$node->safe_psql('postgres', 'CREATE TABLE t (id int, v text) USING appender');
$node->safe_psql('postgres',
	"INSERT INTO t SELECT g, repeat(md5(g::text), 1000) FROM generate_series(1, 100) g");

# leave an uncommitted external value in flight, then crash
my $open = $node->background_psql('postgres');
$open->query_safe('BEGIN');
$open->query_safe("INSERT INTO t VALUES (1001, repeat('y', 100000))");
$node->stop('immediate');
$open->quit;
$node->start;

is( $node->safe_psql('postgres',
		'SELECT count(*) FROM t WHERE left(v, 32) = md5(id::text)'),
	'100', 'committed external values survive a crash');
is( $node->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'100', 'aborted external values are not visible after a crash');
is( $node->safe_psql('postgres',
		"SELECT length(v) FROM t WHERE id = 1"),
	'32000', 'an external value detoasts after recovery');
$node->stop;

# --- streaming standby -----------------------------------------------------

my $primary = PostgreSQL::Test::Cluster->new('toast_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', $conf . "wal_keep_size = '320MB'\n");
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');
$primary->safe_psql('postgres', 'CREATE TABLE t (id int, v text) USING appender');
$primary->safe_psql('postgres',
	"INSERT INTO t SELECT g, repeat(md5(g::text), 500) FROM generate_series(1, 50) g");
$primary->backup('tb');

my $standby = PostgreSQL::Test::Cluster->new('toast_standby');
$standby->init_from_backup($primary, 'tb', has_streaming => 1);
$standby->start;
$primary->wait_for_catchup($standby);

is( $standby->safe_psql('postgres',
		"SELECT count(*) FROM t WHERE left(v, 32) = md5(id::text)"),
	'50', 'a standby replays external values');

$primary->safe_psql('postgres',
	"INSERT INTO t SELECT g, repeat(md5(g::text), 500) FROM generate_series(51, 100) g");
$primary->wait_for_catchup($standby);

is( $standby->safe_psql('postgres',
		"SELECT length(v) FROM t WHERE id = 100"),
	'16000', 'a standby detoasts a streamed external value');

# --- value addressing across a subtransaction rollback and TRUNCATE -----------

$primary->safe_psql('postgres', 'CREATE TABLE v (id int, t text) USING appender');
$primary->safe_psql('postgres',
	"INSERT INTO v VALUES (1, repeat('base', 50000))");
$primary->safe_psql('postgres', q{
	BEGIN;
	INSERT INTO v VALUES (2, repeat('dead', 50000));
	SAVEPOINT s;
	INSERT INTO v VALUES (3, repeat('junk', 50000));
	ROLLBACK TO s;
	INSERT INTO v VALUES (4, repeat('live', 50000));
	COMMIT;
});
is( $primary->safe_psql('postgres',
		"SELECT string_agg(id || ':' || left(t, 4), ',' ORDER BY id) FROM v"),
	'1:base,2:dead,4:live',
	'values survive a rolled-back subtransaction with fresh value ids');
$primary->safe_psql('postgres', 'TRUNCATE v');
$primary->safe_psql('postgres',
	"INSERT INTO v VALUES (5, repeat('after', 50000))");
is( $primary->safe_psql('postgres',
		"SELECT id || ':' || left(t, 5) FROM v"),
	'5:after',
	'values are correct after a TRUNCATE replaces the storage');

done_testing();
