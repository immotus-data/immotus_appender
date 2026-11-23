# Benchmarks

Head-to-head numbers for the `appender` table access method against plain
`heap`, plus the two retention demos that show where the architecture differs.

All scripts need a server with `immotus_appender` in
`shared_preload_libraries`, and the extension `CREATE EXTENSION`d in the
target database.  The micro-benchmark (`run.sh`) additionally needs `pgbench`
and `bc` on the same host as the server, and a superuser connection (COPY
writes a server-side file under `/tmp`).

> Methodology is described in [../doc/benchmarks.md](../doc/benchmarks.md):
> release build, `fsync = on`, median of N runs.

## `run.sh` — micro-benchmark

Parameterized via environment variables:

| var | default | meaning |
|---|---|---|
| `RUNS` | 5 | repetitions per cell (use the median) |
| `DUR` | 15 | pgbench duration, seconds |
| `NROWS` | 1000000 | rows for load and scan benches |
| `NVAL` | 2000 | large values (~100 KB each) |
| `AMS` | `heap appender` | access methods to compare |

Measured:

- **load_insert / load_copy / load_copy_binary** — rows/s, WAL bytes/row,
  on-disk bytes/row (insert only).
- **scan_count / scan_allcols** — serial and parallel, ms.
- **commit_c{1,8}_r{1,100}** — pgbench commits/s at 1 and 8 clients, 1 or
  100 rows per transaction.
- **values_insert / values_read** — 100 KB EXTERNAL values, MB/s and ms.

It prints one `RESULT <bench> <am> <metric> <value>` line per measurement.

```sh
PGPORT=5432 PGDATABASE=postgres RUNS=3 NROWS=500000 DUR=10 bench/run.sh
```

## `bench_retention_watermark.sh`

Bulk-ingests `DAYS` days × `ROWS` rows into three variants, then applies
retention (keep the last `KEEP` days) and measures the resulting footprint
and a warm-cache full scan:

- `heap_naive` — one heap table, `DELETE` + `VACUUM`.  The file never
  shrinks: the full scan keeps reading the old high-water mark.
- `heap_part` — daily heap partitions, `DROP PARTITION`.
- `ao_part` — daily **appender** partitions, `DROP PARTITION`.

See [../doc/benchmarks.md](../doc/benchmarks.md) for the numbers.

```sh
DAYS=30 KEEP=7 ROWS=100000 PGROOT=/path/to/pg PORT=5432 bench/bench_retention_watermark.sh
```

## `bench_retention_lifetime.sh`

Simulates 30 days of daily ingestion plus a rolling 7-day window, measuring
per variant the total maintenance wall time and the final footprint.  The
naive heap variant pays `DELETE` + `VACUUM` every day; the partition variants
just `DROP PARTITION`.

```sh
DAYS=30 KEEP=7 ROWS=100000 PGROOT=/path/to/pg PORT=5432 bench/bench_retention_lifetime.sh
```

## `demo_walkthrough.sql` — interactive walkthrough

A commented psql script walking the append-only lifecycle (create, insert,
aborted insert, self-insert, scans, backward scan, and the operations that are
rejected).  It does not stop on errors.

```sh
psql -p 5432 -d postgres -f bench/demo_walkthrough.sql
```

## `demo_event_log.sql` — the event log showcase

One partitioned table: the open day is a heap partition with indexes, the
closed days are `appender`.  Runs the whole story in a few seconds: the write
path measured in WAL bytes against heap, the sizes, the rejected
`UPDATE`/`DELETE` that make history immutable, an
indexed point lookup on the open day, closing the day in place, retention by
`DROP`, and a dashboard query that reads both kinds of partition.  Expects a
superuser connection and drops a table named `events`.

The reasoning behind it and the limits are in [../doc/event-log.md](../doc/event-log.md).

```sh
psql -p 5432 -d postgres -f bench/demo_event_log.sql
```
