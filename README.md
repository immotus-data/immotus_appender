# immotus_appender

Append-only table access method for PostgreSQL.

`immotus_appender` is a PostgreSQL extension that provides the `appender`
table access method. Tables created with `USING appender` are write-once:
rows are only ever appended and never updated or deleted.

## Why

Append-only data (audit logs, event streams, telemetry) is stored more
compactly and written with less WAL when the storage knows it will never
change:

- rows carry no per-row transaction header, so they are smaller;
- blocks are written once and never modified, so WAL carries no full-page
  images;
- rows never need freezing, so large tables never trigger anti-wraparound
  vacuums;
- rows are immutable by construction, not by convention.

Sealed segments are immutable files, which is the basis for moving cold data
to cheaper storage in a later release.

## How it works

Rows are stored as compact tuples (no per-row transaction header) packed into
variable-size blocks, and blocks are appended to segment files
(`<relfilenode>.dat.<segno>`) that bypass the shared buffer manager. A segment
is sealed once it reaches `immotus_appender.segment_size` and is immutable from
then on. Every block carries a checksum. A row's TID is logical:
(segment, row number), independent of where the row is stored.

Large varlena values are stored in dedicated value blocks in the same segment
and referenced by standard TOAST pointers, so there is no separate TOAST table.
Each segment carries a value directory, so a value is found by lookup rather
than by scanning; the per-backend cache of directories is bounded by
`immotus_appender.valdir_cache_size`.

The `appender_meta` table (in the extension's schema) is the manifest: one row
per segment with its committed end. A transaction's appends become visible
when its manifest update commits, so appends are atomic and MVCC-correct.
Concurrent writers append to different segments: a writer takes an open
segment no other transaction holds, the fullest first, or else starts a new
one, so the number of open segments stays at the peak number of concurrent
writers. Direct DML on the manifest is blocked by a trigger.

Every change to segment files is WAL-logged by a custom resource manager, so
streaming replication, point-in-time recovery, incremental backups and
`pg_rewind` work. Incremental backups copy segment files in full, because
PostgreSQL tracks changed blocks only for its own relation files.

Touched segments are flushed with `fdatasync` at commit, before the manifest
update: a commit therefore costs one more sync than on heap, which hurts
single-row transactions most and less as commits get larger (see Commit cost).
Segment files are zero-filled ahead in `immotus_appender.prealloc_chunk`
(default 1 MB, 0 disables) chunks, so the `fdatasync` of a commit that stays
inside a chunk does not pay for the file-size change. The physical file can be
up to one chunk larger than its logical end.

## Status

Version 1.0 (in development). Supported: `INSERT`, `COPY`, sequential and
parallel scans, savepoints, `TRUNCATE`, `ALTER TABLE` (including rewrites and
`SET ACCESS METHOD`), `ANALYZE`, partitioning, large varlena storage
(`EXTENDED`/`EXTERNAL`/`MAIN` semantics), streaming replication, PITR.

Not supported: `UPDATE`, `DELETE`, `INSERT ... ON CONFLICT`, indexes, backward
scans, `TABLESAMPLE`, unlogged and temporary tables, two-phase commit,
`CLUSTER`/`VACUUM FULL`, moving a table to another tablespace, logical
replication and logical decoding. A publication accepts an appender table, but
its rows are never sent: do not use logical replication or CDC tools with
appender tables.
`CREATE DATABASE` from a template that contains appender tables requires
`STRATEGY = FILE_COPY`. Major-version upgrades use `pg_upgrade` plus a copy of
the segment files; see `doc/pg_upgrade.md`.

Segment files left behind by a crash between a `DROP` and its file removal
can be removed with `SELECT appender_gc()`. `appender_relation_size(regclass)`
reports the on-disk size of a table: `pg_relation_size`, `pg_table_size` and
`pg_total_relation_size` are smgr-based and report 0 for appender tables.
Because segment files live under `base/<dboid>`, `pg_database_size()` and
`pg_tablespace_size()` do count them.

Do not use in production.

## Performance

Measured against heap on Linux (ext4) with PostgreSQL 18 from the PGDG
packages, `fsync = on`, `shared_buffers = 1GB` and default `immotus_appender`
settings; reads run with a warm cache. Rows have 4 columns with a 64-byte
payload. Each figure is the median of 5 runs. Absolute throughput depends on
hardware, so it is shown relative to heap; bytes per row do not. To reproduce
on your own hardware, run `bench/run.sh`.

Storage, 1M rows:

| | heap | appender |
| --- | --- | --- |
| WAL, bytes/row, `INSERT ... SELECT` | 136.4 | **96.5** |
| WAL, bytes/row, `COPY` | **91.2** | 96.5 |
| size on disk, bytes/row | 117.0 | **96.4** |

Speed, appender relative to heap (above 1 means appender is faster):

| workload | appender / heap |
| --- | --- |
| bulk load, `INSERT ... SELECT` 1M rows | **1.43** |
| bulk load, `COPY` 1M rows | 0.81 |
| `count(*)` over 1M rows, 1 process | 0.76 |
| `count(*)` over 1M rows, 2 workers | 0.67 |
| aggregate over all 4 columns, 1M rows, 1 process | 0.93 |
| write 100 KB values (`EXTERNAL`) | 0.93 |
| read 100 KB values | 0.77 |

Bulk loads through `INSERT ... SELECT` are faster than heap and produce less
WAL. `COPY` is slower than heap: it does not batch rows yet, while heap writes
multi-row WAL records. Scans read through the OS page cache instead of
shared buffers and verify a checksum for every block, so they trail heap the
most on narrow queries such as `count(*)`; the more columns a query
deforms, the closer they get.

Row-level `AFTER` triggers and foreign keys on an appender table re-read every
new row by TID, which is much more expensive than on heap: loading 200,000
rows into a table with a foreign key ran at about a quarter of heap's speed.

### Commit cost

Every commit that appended rows `fdatasync`s the touched segments before the
manifest update, on top of the WAL flush heap needs. Same setup as above,
`synchronous_commit = on`, 15-second `pgbench` runs:

| clients | rows per commit | appender / heap |
| --- | --- | --- |
| 1 | 1 | 0.56 |
| 1 | 100 | 0.66 |
| 8 | 1 | 0.41 |
| 8 | 100 | 0.49 |

Heap commits faster in every case: appender trades commit rate for smaller
rows and less WAL. With more clients heap gains from group commit of the WAL
flush, while each appender commit still syncs its own segment.

- `synchronous_commit = off` does not remove the segment `fdatasync`: a commit
  record could reach disk without its data, and local crash recovery does not
  replay segment data.
- A standby repeats the sync for every commit it replays.
- Commit many rows at a time. Idle-in-transaction writers each hold a
  segment, so keep write transactions short.
- There is no `DELETE`. Retain data with time partitions and drop or detach
  old ones.
- A table storage can use at most 65,534 segment numbers over its lifetime:
  sealed segments keep their number until `TRUNCATE` or a rewrite. That is
  about 64 TB at the default 1 GB segment size, but a tiny
  `immotus_appender.segment_size` reaches it quickly, which is why only
  superusers can change it.

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

## Requirements

- PostgreSQL 18 (tested against 18.6)
- development headers (`postgresql-server-dev` on Debian/Ubuntu)

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
- the TAP tests (`t/`): crash recovery, streaming replication and promotion,
  point-in-time recovery, incremental backup and `pg_rewind`.

The server must load the library (`shared_preload_libraries`). The TAP tests
are skipped unless PostgreSQL was built with `--enable-tap-tests`. Set
`PGPORT` if the test cluster is not on the default port.

## License

Apache-2.0. See `LICENSE`.
