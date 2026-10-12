# Streaming replication and promotion of appender tables.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"shared_preload_libraries = 'immotus_appender'");
$primary->start;

$primary->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');
$primary->safe_psql('postgres', 'CREATE TABLE t (a int, b text) USING appender');
$primary->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'base' FROM generate_series(1, 1000) g");

$primary->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'base', has_streaming => 1);
$standby->start;

# appends after the base backup, across several segments
$primary->safe_psql('postgres',
	"SET immotus_appender.segment_size = '1MB';
	 INSERT INTO t SELECT g, repeat('x', 100) FROM generate_series(1, 30000) g");
$primary->safe_psql('postgres',
	"BEGIN;
	 INSERT INTO t VALUES (-1, 'kept');
	 SAVEPOINT s;
	 INSERT INTO t SELECT g, 'rolled back' FROM generate_series(1, 5000) g;
	 ROLLBACK TO s;
	 COMMIT");

# storage that the standby must create and remove
$primary->safe_psql('postgres', 'CREATE TABLE tr (a int) USING appender');
$primary->safe_psql('postgres', 'INSERT INTO tr SELECT generate_series(1, 100)');
$primary->safe_psql('postgres', 'TRUNCATE tr');
$primary->safe_psql('postgres', 'INSERT INTO tr VALUES (7)');
$primary->safe_psql('postgres', 'CREATE TABLE dr (a int) USING appender');
$primary->safe_psql('postgres', 'INSERT INTO dr SELECT generate_series(1, 100)');
my $dr_node = $primary->safe_psql('postgres',
	"SELECT relfilenode FROM pg_class WHERE relname = 'dr'");
$primary->safe_psql('postgres', 'DROP TABLE dr');

$primary->wait_for_catchup($standby);

my $check = "SELECT count(*) || ' ' || sum(a) || ' ' || md5(string_agg(b, ',' ORDER BY ctid)) FROM t";
my $expected = $primary->safe_psql('postgres', $check);
is($standby->safe_psql('postgres', $check),
	$expected, 'standby has the same rows as the primary');
is($standby->safe_psql('postgres', "SELECT count(*) FROM t WHERE b = 'rolled back'"),
	'0', 'rows of a rolled back savepoint are not replicated as visible');
is($standby->safe_psql('postgres', 'SELECT string_agg(a::text, \',\') FROM tr'),
	'7', 'TRUNCATE is replicated');

my @left = glob($standby->data_dir . "/base/*/$dr_node.dat.*");
is(scalar @left, 0, 'segments of a dropped table are removed on the standby');

# the promoted standby takes new appends
$standby->promote;
$standby->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'promoted' FROM generate_series(1, 10) g");
is($standby->safe_psql('postgres', "SELECT count(*) FROM t WHERE b = 'promoted'"),
	'10', 'promoted standby accepts inserts');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'31011', 'promoted standby keeps all replicated rows');

done_testing();
