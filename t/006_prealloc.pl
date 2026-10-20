# Segment preallocation and fdatasync.
#
# A segment file is zero-filled ahead in chunks, so commits inside a chunk
# need only fdatasync.  This checks the physical layout and that data written
# across many chunk boundaries survives a crash, both with preallocation on
# and with it disabled.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub logical_eof
{
	my ($node, $table) = @_;
	return $node->safe_psql('postgres',
		"SELECT sum(eof) FROM appender_meta m, pg_class c
		 WHERE c.relfilenode = m.relfilenode AND c.relname = '$table'");
}

sub physical_size
{
	my ($node, $table) = @_;
	my $dboid = $node->safe_psql('postgres',
		"SELECT oid FROM pg_database WHERE datname = 'postgres'");
	my $relfilenode = $node->safe_psql('postgres',
		"SELECT relfilenode FROM pg_class WHERE relname = '$table'");
	my $total = 0;
	for my $f (glob($node->data_dir . "/base/$dboid/$relfilenode.dat.*"))
	{
		$total += -s $f;
	}
	return $total;
}

# --- preallocation on -------------------------------------------------------

my $pre = PostgreSQL::Test::Cluster->new('prealloc');
$pre->init;
$pre->append_conf('postgresql.conf',
	"fsync = on\n"
	. "shared_preload_libraries = 'immotus_appender'\n"
	. "immotus_appender.prealloc_chunk = '64kB'");
$pre->start;
is($pre->safe_psql('postgres', 'SHOW immotus_appender.prealloc_chunk'),
	'64kB', 'prealloc_chunk is applied');
$pre->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');
$pre->safe_psql('postgres', 'CREATE TABLE t (a int, b text) USING appender');
$pre->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'v' || g FROM generate_series(1, 1000) g");

cmp_ok(0 + physical_size($pre, 't'), '>', 0 + logical_eof($pre, 't'),
	'preallocation leaves a zero tail past the logical eof');

$pre->safe_psql('postgres',
	"INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 5000) g");
$pre->stop('immediate');
$pre->start;
is($pre->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'6000', 'rows across preallocation chunks survive a crash');
is($pre->safe_psql('postgres',
		"SELECT count(*) FROM t WHERE b LIKE 'x%'"),
	'5000', 'every preallocated row reads back intact');

# toggling prealloc_chunk mid-transaction must not corrupt data: the cached
# zero-fill horizon has to be invalidated while preallocation is off
$pre->safe_psql('postgres', 'CREATE TABLE t2 (a int, b text) USING appender');
my $bg = $pre->background_psql('postgres');
$bg->query_safe('BEGIN');
$bg->query_safe(
	"INSERT INTO t2 SELECT g, repeat('a', 100) FROM generate_series(1, 2000) g");
$pre->safe_psql('postgres',
	'ALTER SYSTEM SET immotus_appender.prealloc_chunk = 0');
$pre->safe_psql('postgres', 'SELECT pg_reload_conf()');
$bg->query_safe('SELECT pg_sleep(0.1)');
$bg->query_safe(
	"INSERT INTO t2 SELECT g, repeat('b', 100) FROM generate_series(1, 3000) g");
$pre->safe_psql('postgres',
	"ALTER SYSTEM SET immotus_appender.prealloc_chunk = '64kB'");
$pre->safe_psql('postgres', 'SELECT pg_reload_conf()');
$bg->query_safe('SELECT pg_sleep(0.1)');
$bg->query_safe(
	"INSERT INTO t2 SELECT g, repeat('c', 100) FROM generate_series(1, 100) g");
$bg->query_safe('COMMIT');
$bg->quit;
is($pre->safe_psql('postgres', 'SELECT count(*) FROM t2'),
	'5100', 'a prealloc_chunk toggle mid-transaction does not corrupt data');
$pre->safe_psql('postgres',
	'ALTER SYSTEM RESET immotus_appender.prealloc_chunk');
$pre->safe_psql('postgres', 'SELECT pg_reload_conf()');
$pre->stop;

# --- preallocation off ------------------------------------------------------

my $plain = PostgreSQL::Test::Cluster->new('plain');
$plain->init;
$plain->append_conf('postgresql.conf',
	"shared_preload_libraries = 'immotus_appender'\n"
	. "immotus_appender.prealloc_chunk = 0");
$plain->start;
$plain->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');
$plain->safe_psql('postgres', 'CREATE TABLE t (a int, b text) USING appender');
$plain->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'v' || g FROM generate_series(1, 1000) g");

cmp_ok(0 + physical_size($plain, 't'), '==', 0 + logical_eof($plain, 't'),
	'without preallocation the file ends exactly at the logical eof');
is($plain->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'1000', 'rows read back with preallocation disabled');
$plain->stop;

# --- preallocation during standby replay ------------------------------------

my $prim = PostgreSQL::Test::Cluster->new('repl_prim');
$prim->init(allows_streaming => 1);
$prim->append_conf('postgresql.conf',
	"fsync = on\n"
	. "shared_preload_libraries = 'immotus_appender'\n"
	. "immotus_appender.prealloc_chunk = '64kB'");
$prim->start;
$prim->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');
$prim->safe_psql('postgres', 'CREATE TABLE t (a int, b text) USING appender');
$prim->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'base' FROM generate_series(1, 1000) g");
$prim->backup('b');

my $repl = PostgreSQL::Test::Cluster->new('repl');
$repl->init_from_backup($prim, 'b', has_streaming => 1);
$repl->start;

$prim->safe_psql('postgres',
	"INSERT INTO t SELECT g, repeat('x', 200) FROM generate_series(1, 5000) g");
$prim->wait_for_catchup($repl);
is($repl->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'6000', 'standby has every replayed row');
cmp_ok(0 + physical_size($repl, 't'), '>', 0 + logical_eof($repl, 't'),
	'standby preallocates a zero tail during replay');
$repl->stop;
$prim->stop;

done_testing();
