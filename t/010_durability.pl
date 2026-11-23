# Durability and corruption detection.
#
# With fsync on, a committed transaction has to survive an abrupt death of a
# backend; and a damaged segment file has to be reported as corruption
# instead of being served as data.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub segment_file
{
	my ($node, $table) = @_;
	my $dboid = $node->safe_psql('postgres',
		"SELECT oid FROM pg_database WHERE datname = 'postgres'");
	my $relfilenode = $node->safe_psql('postgres',
		"SELECT relfilenode FROM pg_class WHERE relname = '$table'");
	my ($file) = sort glob(
		sprintf("%s/base/%s/%s.dat.*", $node->data_dir, $dboid, $relfilenode)
	);

	return $file;
}

my $node = PostgreSQL::Test::Cluster->new('durability');
$node->init;
$node->append_conf('postgresql.conf',
		"shared_preload_libraries = 'immotus_appender'\n"
	  . "fsync = on\n"
	  . "synchronous_commit = on\n"
	  . "restart_after_crash = on\n");
$node->start;

is($node->safe_psql('postgres', 'SHOW fsync'), 'on', 'fsync is on');
$node->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');
$node->safe_psql('postgres', 'CREATE TABLE t (a int, b text) USING appender');
$node->safe_psql('postgres',
	"INSERT INTO t SELECT g, 'committed' FROM generate_series(1, 20000) g");

# --- a backend killed in the middle of a transaction -----------------------

my $bg = $node->background_psql('postgres');
$bg->query_safe("SET application_name = 'victim'");
$bg->query_safe('BEGIN');
$bg->query_safe(
	"INSERT INTO t SELECT g, 'lost' FROM generate_series(1, 50000) g");
my $pid = $node->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE application_name = 'victim'");
like($pid, qr/^\d+$/, 'found the writing backend');
kill 'KILL', $pid;
$bg->quit;

$node->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die 'the server did not come back after the killed backend';

is($node->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'20000', 'committed rows survive a backend killed with SIGKILL');
is($node->safe_psql('postgres', "SELECT count(*) FROM t WHERE b = 'lost'"),
	'0', 'the killed transaction left nothing behind');
$node->safe_psql('postgres', "INSERT INTO t VALUES (1, 'after')");
is($node->safe_psql('postgres', 'SELECT count(*) FROM t'),
	'20001', 'the table is writable after the crash');

# --- a damaged block -------------------------------------------------------

$node->safe_psql('postgres', 'CREATE TABLE c (a int, b text) USING appender');
$node->safe_psql('postgres',
	"INSERT INTO c SELECT g, repeat('x', 40) FROM generate_series(1, 5000) g"
);
is($node->safe_psql('postgres', 'SELECT sum(a) FROM c'),
	'12502500', 'the table reads back before the damage');

my $file = segment_file($node, 'c');
ok(defined $file, 'the table has a segment file');
open(my $fh, '+<', $file) or die "could not open $file: $!";
binmode($fh);
my $offset = 32 + 32 + 64;    # file header, block header, then the payload
seek($fh, $offset, 0) or die "could not seek in $file: $!";
read($fh, my $byte, 1) == 1 or die "could not read $file";
seek($fh, $offset, 0) or die "could not seek in $file: $!";
print $fh chr(ord($byte) ^ 0xFF) or die "could not write $file";
close($fh) or die "could not close $file: $!";

my ($ret, $stdout, $stderr) = $node->psql('postgres', 'SELECT sum(a) FROM c');
isnt($ret, 0, 'a damaged block makes the scan fail');
like(
	$stderr,
	qr/block checksum mismatch/,
	'the error reports the checksum rather than returning the data');
is($node->safe_psql('postgres', "SELECT appender_relation_size('c') > 0"),
	't', 'the manifest alone still describes the table');
is(($node->psql('postgres', 'DROP TABLE c'))[0],
	0, 'a damaged table can still be dropped');

$node->stop;

done_testing();
