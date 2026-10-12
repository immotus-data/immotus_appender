# Crash recovery of appender tables.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'immotus_appender'");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');
$node->safe_psql('postgres', 'CREATE TABLE t (a int, b text) USING appender');
$node->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'v' || g FROM generate_series(1, 1000) g");

# an open transaction writes blocks that must not survive the crash
my $open = $node->background_psql('postgres');
$open->query_safe('BEGIN');
$open->query_safe(
	"INSERT INTO t SELECT g, 'lost' FROM generate_series(1, 50000) g");
$node->safe_psql('postgres', "INSERT INTO t VALUES (424242, 'survivor')");

$node->stop('immediate');
$open->quit;
$node->start;

is($node->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'1001', 'committed rows survive, uncommitted rows are gone');
is( $node->safe_psql('postgres', "SELECT count(*) FROM t WHERE b = 'lost'"),
	'0', 'no row of the aborted transaction is visible');

# new rows overwrite the abandoned tail of the segment
$node->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'after' FROM generate_series(1, 100) g");
is($node->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'1101', 'inserts after recovery overwrite the abandoned tail');
is($node->safe_psql('postgres', 'SELECT sum(a) FROM t WHERE b = \'after\''),
	'5050', 'rows written after recovery read back intact');

# a crash between a DROP's commit and its unlink leaves nothing visible
$node->safe_psql('postgres', 'CREATE TABLE d (a int) USING appender');
$node->safe_psql('postgres', 'INSERT INTO d SELECT generate_series(1, 10)');
$node->safe_psql('postgres', 'DROP TABLE d');
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', 'SELECT appender_gc()'),
	'0', 'dropped table left no orphan segments');

done_testing();
