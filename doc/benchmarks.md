# Benchmarks

How the numbers in the README are produced, and how to reproduce them on your
own hardware.

> Measured on Linux with `fsync = on`, median of 5 runs.  Reproduce the numbers
> with the commands in §2.

---

## 1. Methodology

- **Release build**, no `--enable-cassert` (asserts change the hot path).
- **`fsync = on`** and **`synchronous_commit = on`**, because the appender's
  defining cost is its per-commit `fdatasync`.
- `shared_buffers = 1GB`, everything else default unless stated.
- **Median of N runs** (N = 5 for the release tables); numbers are bimodal on
  a busy host because of page-cache writeback.
- Scans run with a **warm cache** (the table is read once before timing).

---

## 2. Build and run

```bash
# PostgreSQL 18 with headers; the extension must be preloaded.
make install PG_CONFIG=/path/to/pg_config

# postgresql.conf
#   shared_preload_libraries = 'immotus_appender'
#   fsync = on
#   synchronous_commit = on
#   shared_buffers = 1GB

createdb bench
psql -d bench -c 'CREATE EXTENSION immotus_appender'
```

Micro-benchmark (bulk load, scans, commit throughput, large values):

```bash
RUNS=5 NROWS=1000000 DUR=15 NVAL=2000 AMS='heap appender' bench/run.sh \
  | tee results.txt
```

Each line is `RESULT <bench> <am> <metric> <value>`; take the median per
`(bench, am, metric)`.

Retention demos (footprint and scan time after retention):

```bash
DAYS=30 KEEP=7 ROWS=100000 PGROOT=/path/to/pg PORT=5432 DB=bench \
  bench/bench_retention_watermark.sh

DAYS=30 KEEP=7 ROWS=100000 PGROOT=/path/to/pg PORT=5432 DB=bench \
  bench/bench_retention_lifetime.sh
```

---

## 3. Results

### 3.1 Storage, 1M rows, 4 columns, 64-byte payload

| | heap | appender |
| --- | --- | --- |
| WAL, bytes/row, `INSERT ... SELECT` | 136.4 | **96.5** |
| WAL, bytes/row, text `COPY` | **91.2** | 96.5 |
| WAL, bytes/row, binary `COPY` | **91.1** | 96.5 |
| size on disk, bytes/row | 117.0 | **96.4** |

The WAL and size figures depend on the row format, not on the machine.

### 3.2 Speed, appender relative to heap (above 1 = appender faster)

| workload | ratio |
| --- | --- |
| bulk load, `INSERT ... SELECT`, 1M rows | **1.48** |
| bulk load, text `COPY`, 1M rows | 0.99 |
| bulk load, binary `COPY`, 1M rows | 0.98 |
| `count(*)`, 1M rows, 1 process | 0.55 |
| `count(*)`, 1M rows, 2 workers | 0.80 |
| aggregate over all columns, 1M rows, 1 process | 0.85 |
| write 100 KB values (`EXTERNAL`) | **1.34** |
| read 100 KB values | 0.93 |

### 3.3 Commit throughput, 15-second `pgbench` runs, `synchronous_commit = on`

| clients | rows per commit | heap, commits/s | appender, commits/s | appender / heap |
| --- | --- | ---: | ---: | --- |
| 1 | 1 | 582.8 | 296.6 | 0.51 |
| 1 | 100 | 522.5 | 280.6 | 0.54 |
| 8 | 1 | 2 570.8 | 1 035.1 | 0.40 |
| 8 | 100 | 2 654.9 | 995.7 | **0.38** |

### 3.4 Retention, `bench_retention_watermark.sh`

| variant | before size | after size | retention, ms | full scan, ms |
| --- | ---: | ---: | ---: | ---: |
| `heap_naive` (`DELETE` + `VACUUM`) | 335 MB | 335 MB | 1 532 | **15** |
| `heap_part` (daily partitions) | 335 MB | 78 MB | **87** | 17 |
| `ao_part` (appender partitions) | 300 MB | **70 MB** | 135 | 14 |

The heap file never shrinks: `DELETE` + `VACUUM` leaves the high-water mark, so
the full scan still reads 2.3M dead rows (42 858 shared buffers versus 14 for
the appender partitions).  Appender partitions drop the footprint to
the retained days.

### 3.5 Retention, `bench_retention_lifetime.sh`

30 days of daily ingest with a rolling 7-day window, maintenance applied every
day:

| variant | total maintenance, ms | final size | retained rows |
| --- | ---: | ---: | ---: |
| `ao_part` | **1 731** | **70 MB** | 700 000 |
| `heap_part` | 2 012 | 78 MB | 700 000 |
| `heap_naive` | 2 985 | 89 MB | 700 000 |

---

## 4. Reading the numbers

- **Bulk `INSERT ... SELECT`** is the appender's best case: rows are batched
  per statement, blocks are written once, and the WAL carries no full-page
  images.
- **`COPY`** is at parity with heap.  Heap packs multi-row WAL records for
  COPY; the appender writes about 5 bytes/row more WAL there, and its rows
  are smaller.
- **Scans** read through the OS page cache and verify a CRC per block, so a
  narrow query such as `count(*)` trails heap the most; the more columns a
  query deforms, the closer it gets.
- **Commits** are where the per-commit `fdatasync` shows.  Heap gains from WAL
  group commit as clients grow; each appender commit still syncs its own
  segment.
