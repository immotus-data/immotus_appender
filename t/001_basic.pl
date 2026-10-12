# Basic tests for the appender table access method.

use strict;
use warnings;

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

is( $node->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'1000', 'insert and sequential scan');

# committed rows survive an immediate crash
$node->safe_psql('postgres', 'INSERT INTO t VALUES (424242, \'survivor\')');
$node->stop('immediate');
$node->start;

is( $node->safe_psql('postgres', 'SELECT count(*) FROM t WHERE a = 424242'),
	'1', 'committed row survives crash');
is( $node->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'1001', 'row count unchanged after crash');

done_testing();
