# Concurrent writers and readers under repeated crashes.
#
# Every writer transaction appends rows to an appender table and records the
# batch in a heap ledger in the same transaction, sometimes also appending a
# batch that is rolled back to a savepoint.  At any snapshot, and after every
# crash, the appender table must hold exactly the rows the ledger accounts
# for.  Readers check that invariant while the load runs.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('stress');
$node->init;
$node->append_conf('postgresql.conf',
		"shared_preload_libraries = 'immotus_appender'\n"
	  . "immotus_appender.segment_size = '1MB'\n"
	  . "max_connections = 50\n");
$node->start;

$node->safe_psql(
	'postgres', q{
	CREATE EXTENSION immotus_appender;
	CREATE TABLE t (client int, v int, pad text) USING appender;
	CREATE TABLE ledger (client int, n int);
});

my $writer = $node->basedir . '/writer.sql';
append_to_file(
	$writer, q{
\set n random(1, 40)
\set junk random(1, 20)
BEGIN;
INSERT INTO t SELECT :client_id, g, repeat('p', 50) FROM generate_series(1, :n) g;
SAVEPOINT s;
INSERT INTO t SELECT :client_id, -g, 'rolled back' FROM generate_series(1, :junk) g;
ROLLBACK TO s;
INSERT INTO ledger VALUES (:client_id, :n);
COMMIT;
});

my $reader = $node->basedir . '/reader.sql';
append_to_file(
	$reader, q{
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) AS rows, coalesce(sum(v), 0) AS total FROM t \gset
SELECT coalesce(sum(n), 0) AS want_rows, coalesce(sum(n * (n + 1) / 2), 0) AS want_total FROM ledger \gset
\if :rows != :want_rows
SELECT 1 / 0 AS rows_mismatch;
\endif
\if :total != :want_total
SELECT 1 / 0 AS total_mismatch;
\endif
COMMIT;
});

my $check = q{
	SELECT (SELECT count(*) FROM t) = (SELECT coalesce(sum(n), 0) FROM ledger)
	   AND (SELECT coalesce(sum(v), 0) FROM t)
	       = (SELECT coalesce(sum(n * (n + 1) / 2), 0) FROM ledger)
	   AND (SELECT count(*) FROM t WHERE v < 0) = 0
};

sub pgbench_cmd
{
	my ($seconds) = @_;
	return [
		'pgbench', '--no-vacuum',
		'--client' => 8,
		'--jobs' => 4,
		'--time' => $seconds,
		'--file' => "$writer\@9",
		'--file' => "$reader\@1",
		'--host' => $node->host,
		'--port' => $node->port,
		'postgres'
	];
}

# load interrupted by immediate shutdowns
for my $round (1 .. 4)
{
	my ($out, $err) = ('', '');
	my $h = IPC::Run::start(pgbench_cmd(30), '>', \$out, '2>', \$err);
	sleep 3;
	$node->stop('immediate');
	$h->finish;
	$node->start;
	is($node->safe_psql('postgres', $check),
		't', "round $round: rows match the ledger after a crash");
}

# uninterrupted load: a reader that sees a mismatch aborts its client, which
# makes pgbench exit non-zero
my ($out, $err) = ('', '');
my $clean = IPC::Run::run(pgbench_cmd(10), '>', \$out, '2>', \$err);
ok($clean, 'readers saw consistent snapshots under load')
  or diag($err);
my ($done) = $out =~ /number of transactions actually processed: (\d+)/;
cmp_ok($done // 0, '>', 100, 'the load actually ran');
is($node->safe_psql('postgres', $check),
	't', 'rows match the ledger at the end');
note(   'transactions in the last run: '
	  . ($done // 0)
	  . ', rows in total: '
	  . $node->safe_psql('postgres', 'SELECT count(*) FROM t'));

ok($node->safe_psql('postgres', "SELECT count(*) FROM appender_meta") > 1,
	'the load spread over several segments');

done_testing();
