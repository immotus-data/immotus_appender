# appender_gc(): orphan segment files
#
# A segment file whose relation is gone is collected, counted once and really
# removed; a file that belongs to a live relation is left alone; and a dropped
# table leaves nothing behind for the collector to find.

use strict;
use warnings FATAL => 'all';

use File::Copy;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('gc');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'immotus_appender'\n");
$node->start;

my $dboid = $node->safe_psql('postgres',
	"SELECT oid FROM pg_database WHERE datname = 'postgres'");
my $dir = $node->data_dir . "/base/$dboid";

$node->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');
$node->safe_psql('postgres', 'CREATE TABLE t (a int) USING appender');
$node->safe_psql('postgres',
	'INSERT INTO t SELECT g FROM generate_series(1, 1000) g');

is($node->safe_psql('postgres', 'SELECT appender_gc()'),
	'0', 'a live segment is not collected');

# A file of a relation that no longer exists is what the collector is for.  The
# relfilenode has to be one pg_class does not know, or the file looks like a
# legitimate extra segment of a live table.
my $seg =
  $node->safe_psql('postgres',
	"SELECT relfilenode FROM pg_class WHERE relname = 't'")
  . '.dat.1';
ok(-f "$dir/$seg", 'the segment file is there');

copy("$dir/$seg", "$dir/999999.dat.1") or die "could not copy $seg: $!";
is($node->safe_psql('postgres', 'SELECT appender_gc()'),
	'1', 'the orphan is collected and counted once');
ok(!-f "$dir/999999.dat.1", 'the orphan file is gone');
is($node->safe_psql('postgres', 'SELECT appender_gc()'),
	'0', 'nothing left to collect');

# DROP unlinks the segment files at commit, so a clean drop leaves no orphan.
$node->safe_psql('postgres', 'DROP TABLE t');
ok(!-f "$dir/$seg", 'the dropped table took its segments with it');
is($node->safe_psql('postgres', 'SELECT appender_gc()'),
	'0', 'the drop left no orphan');

$node->stop;

done_testing();
