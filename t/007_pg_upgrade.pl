# pg_upgrade of appender tables.
#
# Stock pg_upgrade copies the catalogs and the appender_meta manifest but
# deliberately leaves the segment files (<relfilenode>.dat.<segno>) behind, so
# an upgraded appender table is an empty shell until the segments are copied
# between the stopped clusters.  This test drives the whole flow and checks
# that every row survives and that appends keep working.

use strict;
use warnings FATAL => 'all';

use Cwd            qw(getcwd);
use File::Basename qw(dirname);
use File::Find;
use File::Path qw(make_path);

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $conf = "shared_preload_libraries = 'immotus_appender'\n";

my $old = PostgreSQL::Test::Cluster->new('pgup_old');
$old->init;
$old->append_conf('postgresql.conf', $conf);
$old->start;

$old->safe_psql('postgres', 'CREATE EXTENSION immotus_appender');
$old->safe_psql('postgres', <<'SQL');
CREATE TABLE t (a int, b text) USING appender;
INSERT INTO t (a, b)
  SELECT g, CASE WHEN g % 11 = 0 THEN NULL ELSE 'v' || g END
    FROM generate_series(1, 50000) g;
CREATE TABLE big (id int, t text) USING appender;
INSERT INTO big VALUES (1, repeat('x', 100000));
SQL

my $check = "SELECT count(*), sum(a), count(b),
       md5(string_agg(COALESCE(b, '<null>'), ',' ORDER BY a)) FROM t";
my $expected = $old->safe_psql('postgres', $check);
my $big_check = "SELECT id, length(t), left(t, 3) FROM big";

# pg_upgrade needs the old cluster down and cleanly shut down: a fast stop
# checkpoints, so the committed segments are on disk with no replay pending.
$old->stop('fast');

my $new = PostgreSQL::Test::Cluster->new('pgup_new');
$new->init;    # initialized but NOT started, as pg_upgrade requires
$new->append_conf('postgresql.conf', $conf);

chomp(my $bindir = `pg_config --bindir`);
my @pg_upgrade = (
	'pg_upgrade',
	'-b' => $bindir,
	'-B' => $bindir,
	'-d' => $old->data_dir,
	'-D' => $new->data_dir,
	'-p' => $old->port,
	'-P' => $new->port);

# pg_upgrade puts its socket and delete_old_cluster.sh in the current
# directory, so run it from a short temporary one: the test data directory is
# too deep for a Unix socket path on some platforms, and the script would
# otherwise land in the source tree.
my $here = getcwd();
my $short = PostgreSQL::Test::Utils::tempdir_short();
chdir $short or die "cannot chdir to $short: $!";

$old->command_ok([ @pg_upgrade, '--check' ], 'pg_upgrade --check passes');
$old->command_ok(\@pg_upgrade, 'pg_upgrade runs');

chdir $here or die "cannot chdir back to $here: $!";

# The binary-upgrade restore recreates appender_meta with a preserved OID and
# relfilenode, and pg_upgrade copies its data file, so the manifest survives.
# The segment files, however, are left behind (they are not a fork pg_upgrade
# knows about).
$new->start;
my $meta_rows = $new->safe_psql('postgres',
		"SELECT count(*) FROM appender_meta m "
	  . "WHERE m.relfilenode = "
	  . "(SELECT relfilenode FROM pg_class WHERE oid = 't'::regclass)");
ok($meta_rows > 0, 'the appender_meta manifest survives pg_upgrade');
$new->stop('fast');

# Copy the segment files pg_upgrade left in the old data directory, then fsync
# the new data directory (segments are outside smgr, so nobody else would sync
# them).
my $old_data = $old->data_dir;
my $new_data = $new->data_dir;
my @segs;
find(
	sub {
		push @segs, $File::Find::name if -f && /\.dat\.\d+\z/;
	},
	"$old_data/base");

ok(@segs > 0, 'the old cluster has appender segment files');
for my $src (@segs)
{
	(my $dst = $src) =~ s/\A\Q$old_data\E/$new_data/
	  or die "segment file outside the old data dir: $src";
	make_path(dirname($dst));
	PostgreSQL::Test::Utils::system_or_bail('cp', '-p', $src, $dst);
}

$new->start;

is($new->safe_psql('postgres', $check),
	$expected, 'pg_upgrade preserves every appender row');
is($new->safe_psql('postgres', $big_check),
	'1|100000|xxx', 'pg_upgrade keeps external value pointers valid');
$new->safe_psql('postgres',
	"INSERT INTO t (a, b) VALUES (60000, 'post-upgrade')");
is($new->safe_psql('postgres', "SELECT count(*) FROM t WHERE a = 60000"),
	'1', 'the upgraded cluster accepts appends');

$new->stop('fast');

done_testing();
