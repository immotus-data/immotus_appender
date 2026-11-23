# Appender tables in a user tablespace.
#
# Segment files live next to the relation's storage, so a table created with
# TABLESPACE keeps them under pg_tblspc.  Both appender_gc() and the CREATE
# DATABASE template guard have to look there as well.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('tablespace');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'immotus_appender'");
$node->start;

my $tsdir = $node->basedir . '/ts';
mkdir $tsdir or die "could not create $tsdir: $!";
$node->safe_psql('postgres', "CREATE TABLESPACE ts LOCATION '$tsdir'");
$node->safe_psql('postgres', 'CREATE DATABASE src');
$node->safe_psql('src', 'CREATE EXTENSION immotus_appender');
$node->safe_psql('src',
	'CREATE TABLE t (a int, b text) USING appender TABLESPACE ts');
$node->safe_psql('src',
	"INSERT INTO t SELECT g, 'v' || g FROM generate_series(1, 500) g");
is($node->safe_psql('src', 'SELECT count(*) FROM t'),
	'500', 'rows are readable from a table created in a user tablespace');

# The WAL_LOG strategy copies relations block by block through the buffer
# manager, so the segment files would silently stay behind.
my ($ret, $stdout, $stderr) =
  $node->psql('postgres', 'CREATE DATABASE copy TEMPLATE src');
isnt($ret, 0, 'template copy of a database with segments is refused');
like(
	$stderr,
	qr/contains appender tables/,
	'the template guard looks in user tablespaces too');

# appender_gc() has to walk the same directories.
my $dboid = $node->safe_psql('src',
	"SELECT oid FROM pg_database WHERE datname = 'src'");
my ($segfile) = glob($node->data_dir . "/pg_tblspc/*/PG_*/$dboid/*.dat.*");
ok(defined $segfile, 'segment files live under pg_tblspc');
(my $segdir = $segfile) =~ s{/[^/]+$}{};
ok(open(my $fh, '>', "$segdir/999999.dat.0"), 'plant an orphan segment');
close($fh);
is($node->safe_psql('src', 'SELECT appender_gc()'),
	'1', 'appender_gc() removes an orphan from a user tablespace');
ok(!-e "$segdir/999999.dat.0", 'the orphan file is gone');
is($node->safe_psql('src', 'SELECT count(*) FROM t'),
	'500', 'the live segment file is kept');

$node->stop;

done_testing();
