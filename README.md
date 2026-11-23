<div align="center">

# immotus_appender

**Append-only storage engine for PostgreSQL — no vacuum, no bloat, no dead
tuples.**

[![PostgreSQL 17, 18, 19](https://img.shields.io/badge/PostgreSQL-17%20%7C%2018%20%7C%2019-336791?logo=postgresql&logoColor=white)](https://www.postgresql.org/)
[![License](https://img.shields.io/badge/license-PostgreSQL%20OR%20Apache--2.0-blue)](./LICENSE)
[![CI](https://github.com/immotus-data/immotus_appender/actions/workflows/ci.yml/badge.svg)](https://github.com/immotus-data/immotus_appender/actions/workflows/ci.yml)

</div>

---

`immotus_appender` is a PostgreSQL extension that adds the **`appender` table
access method**.  Tables created with `USING appender` are write-once: rows are
appended and never updated or deleted, so the storage engine can drop
everything heap keeps for mutation.

Built for **audit logs, event streams, telemetry, metrics and immutable
history** — data you write once and read forever.

## Quick start

```bash
git clone https://github.com/immotus-data/immotus_appender.git
cd immotus_appender
make install PG_CONFIG=/path/to/pg_config
```

`immotus_appender` uses a custom WAL resource manager, so it must be loaded
at server startup, on the primary and on every standby. Add this to
`postgresql.conf` and restart:

```
shared_preload_libraries = 'immotus_appender'
```

```sql
CREATE EXTENSION immotus_appender;

CREATE TABLE audit_log (
    id          bigint GENERATED ALWAYS AS IDENTITY,
    event_time  timestamptz NOT NULL DEFAULT now(),
    user_id     uuid NOT NULL,
    action      text NOT NULL,
    payload     jsonb
) USING appender;

INSERT INTO audit_log (user_id, action, payload)
VALUES ('a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11', 'login', '{"ip": "10.0.0.1"}');

SELECT * FROM audit_log WHERE user_id = '...';
```

## Why appender?

The shape this fits is an event or audit log.  The write path runs all day,
every day: the same rows cost 29% less WAL and 18% less space than heap, so
the primary writes less and every standby ships less.  Once a day is closed it
is immutable by construction — the engine rejects `UPDATE` and `DELETE`, which
is the property an audit log actually needs — and it needs no vacuum for as
long as it is kept.  One partitioned table, one set of queries, two storage
engines: the open day stays on heap for indexed point lookups, and every
closed day becomes an `appender` partition that costs less space to keep.  Scans are slower on appender; the full story and the limits
are in [doc/event-log.md](doc/event-log.md).

| | heap | appender |
| --- | --- | --- |
| Row header | `xmin`, `xmax`, `ctid`, `infomask`, alignment padding | **none** |
| Updates / deletes | yes | **no, by construction** |
| `VACUUM`, freezing, anti-wraparound | required | **never needed** (one `VACUUM` after a switch from heap) |
| Bloat from churn | yes | **no** |
| WAL for a bulk insert | full-page images after a checkpoint | **no full-page images** |
| Indexes | any | none yet (sequential scans only) |

The result: smaller rows, less WAL, faster bulk loads, and a table that never
needs vacuuming.  See [Benchmarks](#benchmarks).

## How it works

```
INSERT/COPY ──► writer ──► block (~32 KB, CRC32C) ──► segment file
                                                      <relfilenode>.dat.N
                                                             │
SELECT ◄── scan/fetch ◄── block cache ◄──────────────────────┘
               ▲
       appender_meta: one row per segment, the commit point
```

- **Compact rows** (MemTuple): no per-row MVCC header, null bitmap, inline
  short varlenas.
- **Write-once blocks** (~32 KB) with a CRC32C, appended to **segment files
  outside the shared buffer manager**; a segment is sealed at
  `immotus_appender.segment_size` (1 GB) and never changes again.
- **Logical TIDs** `(segment, row)` — stable and independent of physical
  offsets.
- **Values, not a TOAST table**: large `varlena`s go to value blocks in the
  same segment, referenced by standard TOAST pointers and found through a
  per-segment directory.
- **The manifest is the commit point**: appends become visible when the
  `appender_meta` update commits.
- **The manifest is guarded**: the only trigger the extension defines is
  `appender_meta_guard_trigger`, a statement-level `BEFORE INSERT OR UPDATE OR
  DELETE OR TRUNCATE` trigger on `appender_meta` that rejects direct DML.
  Appender tables themselves carry no triggers.
- **Writers claim open segments** with non-blocking locks, fullest first, so
  open segments track peak concurrency, not table size.
- **Custom WAL resource manager**: streaming replication, PITR, base backups
  and `pg_rewind` all work.

Read [doc/architecture.md](doc/architecture.md) for the full design.

## Features

| Area | What you get |
| --- | --- |
| **Bulk ingest** | `INSERT`, `INSERT ... SELECT`, text and binary `COPY`; `COPY` hands rows over 1000 at a time |
| **Queries** | sequential and parallel scans, `EXPLAIN`, `ANALYZE` / `pg_stats` |
| **Transactions** | atomic appends, MVCC visibility, savepoints and subtransactions |
| **DDL** | `TRUNCATE`, `ALTER TABLE` (including rewrites and `SET ACCESS METHOD`), declarative partitioning |
| **Large values** | `EXTENDED` / `EXTERNAL` / `MAIN` storage, standard TOAST pointers, per-segment value directories, sliced reads |
| **Durability** | per-block CRC32C, per-commit `fdatasync`, crash recovery |
| **Replication & backup** | streaming replication, PITR, base backup, `pg_rewind`, `pg_dump`/`pg_restore`, `pg_upgrade` (segment files copied by hand; see [doc/pg_upgrade.md](doc/pg_upgrade.md)) |
| **Operations** | `appender_relation_size()`, `appender_gc()`, four GUCs (see [Configuration](#configuration)); `VACUUM` has nothing to reclaim and only refreshes statistics |

Storage, SQL surface and operational behaviour are described in
[doc/architecture.md](doc/architecture.md).

## Configuration

All settings live under the `immotus_appender` prefix, which is reserved: a
misspelled name is an error rather than a setting that silently does nothing.

| Parameter | Default | Range | Context | What it does |
| --- | --- | --- | --- | --- |
| `immotus_appender.segment_size` | 1 GB | 1 MB – 1 TB | superuser, `SET` | Size at which a segment is sealed. A smaller value burns the 65 534 segment numbers faster; segments already sealed keep their size. |
| `immotus_appender.prealloc_chunk` | 1 MB | 0 – 64 MB | `postgresql.conf`, `SIGHUP` | Segment tails are zero-filled ahead in chunks of this size, so commits inside a chunk only `fdatasync`. `0` disables preallocation. |
| `immotus_appender.valdir_cache_size` | 64 MB | 1 MB – 1 TB | any user, `SET` | Per-backend memory budget for cached value directories; least recently used ones are evicted past it. |
| `immotus_appender.debug_log` | off | on / off | `postgresql.conf`, `SIGHUP` | Emits a `DEBUG1` line for every replayed or skipped segment record. Meant for the tests, so it is hidden from `pg_settings` and `SHOW ALL`. |

## Compatibility and limitations

**Requires PostgreSQL 17, 18 or 19** and
`shared_preload_libraries = 'immotus_appender'`.

### What the model does not do

Rows are immutable: there is no `UPDATE`, no `DELETE` and no
`INSERT ... ON CONFLICT` path at all, and no dead tuples to vacuum.  Those
operations are what heap storage pays for with tuple versions, free space and
vacuum, so dropping them is the design rather than a gap.  Retention is done
by partitioning on time and dropping whole partitions, not by deleting rows.

### Known limitations

| Limitation | Effect and workaround |
| --- | --- |
| No indexes yet | Every query is a sequential scan. Partition by time so a query reads one partition, and select only the columns it needs. |
| One `fdatasync` per touched segment, per commit | Commit rate is below heap. Batch rows per transaction and keep write transactions short. |
| No logical replication | An appender table cannot be published, directly or through `FOR ALL TABLES` / `TABLES IN SCHEMA`. |
| `pg_relation_size()`, `pg_table_size()` and `pg_total_relation_size()` return 0 | They walk the smgr layout and do not see segment files, so `pg_database_size()` undercounts as well. Use `appender_relation_size(regclass)`. |
| `CREATE DATABASE` from a template with appender tables needs `STRATEGY = FILE_COPY` | The default strategy copies relations through the buffer manager and cannot carry segment files. |
| No `ALTER TABLE ... SET TABLESPACE`, `VACUUM FULL` or `CLUSTER` | Rewrite the table into a new one instead. |
| No two-phase commit | `PREPARE TRANSACTION` is rejected once the transaction has appended rows. |
| No backward scans, no `TABLESAMPLE`, permanent tables only | Rejected by the access method. |
| 65 534 segment numbers per storage, not reused until `TRUNCATE` or a rewrite | About 64 TB at the default 1 GB segment size. `immotus_appender.segment_size` is superuser-only for this reason. |
| No storage tiering yet | Sealed segments are read from the relation's tablespace directory; moving them to external storage is a planned direction, not supported today. |

Operational notes:

- `VACUUM` is never needed: there is nothing to reclaim or freeze, and the
  planner takes its row and page counts from the manifest at planning time.
  `ANALYZE` still matters for column statistics, exactly as it does for heap,
  and autovacuum's autoanalyze runs it on its own.  A table that was heap before
  `ALTER TABLE ... SET ACCESS METHOD appender` keeps the `relfrozenxid` of its
  heap life, which would hold back `pg_database.datfrozenxid`; run `VACUUM`
  once after the switch and the horizon moves to the present.  Tables created as
  appender have no horizon at all and need nothing.
- Major-version upgrades need one extra step; see
  [doc/pg_upgrade.md](doc/pg_upgrade.md).
- Orphan segment files left by a crash between `DROP` and its unlink are
  removed by `SELECT appender_gc()`.

## Benchmarks

Measured on Linux, `fsync = on`, `synchronous_commit = on`,
`shared_buffers = 1GB`, warm cache, rows with 4 columns and a 64-byte payload,
**median of 5 runs**.  Absolute throughput depends on the machine, so it is
shown relative to heap; bytes per row do not.

The full setup and methodology are in [doc/benchmarks.md](doc/benchmarks.md).
To reproduce on your own hardware, run `bench/run.sh`.

Storage, 1M rows:

| | heap | appender |
| --- | --- | --- |
| WAL, bytes/row, `INSERT ... SELECT` | 136.4 | **96.5** |
| WAL, bytes/row, text `COPY` | **91.2** | 96.5 |
| WAL, bytes/row, binary `COPY` | **91.1** | 96.5 |
| size on disk, bytes/row | 117.0 | **96.4** |

Speed, appender relative to heap (above 1 means appender is faster):

| workload | appender / heap |
| --- | --- |
| bulk load, `INSERT ... SELECT` 1M rows | **1.48** |
| bulk load, text `COPY` 1M rows | 0.99 |
| bulk load, binary `COPY` 1M rows | 0.98 |
| `count(*)` over 1M rows, 1 process | 0.55 |
| `count(*)` over 1M rows, 2 workers | 0.80 |
| aggregate over all 4 columns, 1M rows, 1 process | 0.85 |
| write 100 KB values (`EXTERNAL`) | **1.34** |
| read 100 KB values | 0.93 |

Bulk loads through `INSERT ... SELECT` are the best case: 1.5× heap with 29%
less WAL and 18% smaller rows.  `COPY` — text and binary — is at parity.
Scans read through the OS page cache instead of shared buffers and verify a
checksum for every block, so they trail heap the most on narrow queries such as
`count(*)`; the more columns a query deforms, the closer they get.

### Commit cost

Every commit that appended rows also `fdatasync`s the touched segments before the
manifest update, on top of the WAL flush heap needs. Same setup as above,
`synchronous_commit = on`, 15-second `pgbench` runs:

| clients | rows per commit | appender / heap |
| --- | --- | --- |
| 1 | 1 | 0.51 |
| 1 | 100 | 0.54 |
| 8 | 1 | 0.40 |
| 8 | 100 | 0.38 |

Heap commits faster in every case: appender trades commit rate for smaller
rows and less WAL. With more clients heap gains from group commit of the WAL
flush, while each appender commit still syncs its own segment.

- `synchronous_commit = off` does not remove the segment `fdatasync`: a commit
  record could reach disk without its data, and local crash recovery does not
  replay segment data.
- A standby repeats the sync for every commit it replays.
- Commit many rows at a time. Idle-in-transaction writers each hold a
  segment, so keep write transactions short.

## Testing

`immotus_appender` is an out-of-tree extension: it is built and run outside
the PostgreSQL source tree. There is no in-tree `make check`; the test suite
runs only against an **installed** PostgreSQL (binaries, headers and the Perl
test modules all come from the installation).

```bash
make install      PG_CONFIG=/path/to/pg_config   # install the extension
make installcheck PG_CONFIG=/path/to/pg_config
```

`make installcheck` runs, against a running server:

- the SQL regression tests (`sql/`; they need the `amcheck` contrib module);
- the isolation test of concurrent writers and readers (`specs/`);
- the TAP tests (`t/`): crash recovery, durability after a killed backend,
  block corruption, streaming replication and promotion, point-in-time
  recovery, incremental backup, `pg_rewind`, `pg_upgrade` and tablespaces.

The server must load the library (`shared_preload_libraries`). The TAP tests
are skipped unless PostgreSQL was built with `--enable-tap-tests`. Set
`PGPORT` if the test cluster is not on the default port.

## Documentation

| Document | What it covers |
| --- | --- |
| [doc/architecture.md](doc/architecture.md) | on-disk format, blocks, segments, manifest, WAL and durability |
| [doc/benchmarks.md](doc/benchmarks.md) | methodology, hardware record and how to reproduce the numbers |
| [doc/pg_upgrade.md](doc/pg_upgrade.md) | upgrading a cluster with `pg_upgrade` (one extra step) |

## Contributing

Issues and pull requests are welcome.  Before opening a PR:

- run `make installcheck` against a PostgreSQL build with `--enable-tap-tests`
  and the extension preloaded;
- keep the C `pgindent`-clean (`typedefs.list` is in the repo) and the Perl
  tests `perltidy`-clean;
- add a regression, isolation or TAP test for every behaviour change;
- note what a user would notice in [CHANGELOG.md](CHANGELOG.md);
- write commit messages in the
  [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/) style,
  `type: summary`, with the types the history already uses: `feat`, `fix`,
  `perf`, `docs`, `test`, `ci`, `style`, `refactor`, `chore`.

Contributions are accepted under both project licenses
(`PostgreSQL OR Apache-2.0`); see [LICENSE](LICENSE).

## License

immotus_appender is dual-licensed under the PostgreSQL License and the
Apache License 2.0. You may choose either license to govern your use of this
work.

1. [PostgreSQL License](./LICENSE-POSTGRESQL.txt)
2. [Apache License 2.0](./LICENSE-APACHE.txt)

`SPDX-License-Identifier: PostgreSQL OR Apache-2.0`

All contributions to immotus_appender are made under both licenses. Third-party
attributions (the Greenplum/VMware-derived MemTuple code) are listed in
[NOTICE](./NOTICE); those portions remain subject to the Apache License 2.0.
