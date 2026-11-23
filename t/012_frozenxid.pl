# xid horizons: converted tables must not peg the database
#
# ALTER TABLE ... SET ACCESS METHOD appender leaves the table with the
# relfrozenxid of its heap life, and vac_update_datfrozenxid() takes the
# minimum over every relation that has a valid horizon.  A stale value left in
# place would hold back pg_database.datfrozenxid until the cluster stops for
# wraparound, so VACUUM has to move it on.  A table created as appender has no
# horizon at all and keeps it that way.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('frozenxid');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'immotus_appender'\n");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');

my $horizon = sub {
	my ($table) = @_;
	return $node->safe_psql('postgres',
		"SELECT relfrozenxid || '/' || relminmxid || '/' || age(relfrozenxid) FROM pg_class WHERE relname = '$table'"
	);
};

# a heap table, then some transactions, so that its horizon is behind the
# current xid by the time it is converted
$node->safe_psql('postgres', 'CREATE TABLE converted (a int)');
$node->safe_psql('postgres',
	'INSERT INTO converted SELECT g FROM generate_series(1, 100) g');
$node->safe_psql('postgres', 'CREATE TABLE burner (a int)');
$node->safe_psql('postgres',
	'INSERT INTO burner SELECT generate_series(1, 50)');
$node->safe_psql('postgres', 'DROP TABLE burner');

$node->safe_psql('postgres',
	'ALTER TABLE converted SET ACCESS METHOD appender');

my @before = split /\//, $horizon->('converted');

ok($before[0] ne '0', 'the converted table inherits a valid relfrozenxid');
ok($before[1] ne '0', 'the converted table inherits a valid relminmxid');

$node->safe_psql('postgres', 'VACUUM converted');

my @after = split /\//, $horizon->('converted');

cmp_ok($after[0], '>', $before[0],
	'VACUUM moves the inherited horizon forward');
cmp_ok($after[2], '<', 10, 'and leaves it close to the current xid');
is($after[1], '1', 'the multixact horizon is refreshed as well');

$node->safe_psql('postgres', 'VACUUM converted');

my @second = split /\//, $horizon->('converted');

is($second[0], $after[0], 'a second VACUUM changes nothing');

is($node->safe_psql('postgres', 'SELECT count(*) FROM converted'),
	'100', 'the rows are still there');

# a table that was born as appender has no horizon to begin with
$node->safe_psql('postgres', 'CREATE TABLE native (a int) USING appender');
$node->safe_psql('postgres',
	'INSERT INTO native SELECT g FROM generate_series(1, 100) g');
$node->safe_psql('postgres', 'VACUUM native');
$node->safe_psql('postgres', 'VACUUM ANALYZE native');

is( $node->safe_psql(
		'postgres',
		"SELECT relfrozenxid || '/' || relminmxid FROM pg_class WHERE relname = 'native'"
	),
	'0/0',
	'a native appender table stays out of the horizon calculation');

$node->stop;

done_testing();
