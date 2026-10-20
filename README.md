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

The `appender_meta` table (in the extension's schema) is the manifest: one row
per segment with its committed end. A transaction's appends become visible
when its manifest update commits, so appends are atomic and MVCC-correct.
Concurrent writers append to different segments. Direct DML on the manifest is
blocked by a trigger.

Every change to segment files is WAL-logged by a custom resource manager, so
streaming replication, point-in-time recovery, incremental backups and
`pg_rewind` work. Touched segments are flushed with `fdatasync` at commit,
before the manifest update: a commit therefore costs one more sync than on
heap, which matters for single-row transactions and not for batched inserts.
Segment files are zero-filled ahead in `immotus_appender.prealloc_chunk`
(default 1 MB, 0 disables) chunks, so the `fdatasync` of a commit that stays
inside a chunk does not pay for the file-size change. The physical file can be
up to one chunk larger than its logical end.

## Status

Version 0.1 (in development). Supported: `INSERT`, `COPY`, sequential and
parallel scans, savepoints, `TRUNCATE`, `ALTER TABLE` (including rewrites and
`SET ACCESS METHOD`), `ANALYZE`, partitioning, streaming replication, PITR.

Not supported: `UPDATE`, `DELETE`, `INSERT ... ON CONFLICT`, indexes, backward
scans, `TABLESAMPLE`, unlogged and temporary tables, two-phase commit,
`CLUSTER`/`VACUUM FULL`, moving a table to another tablespace.
`CREATE DATABASE` from a template that contains appender tables requires
`STRATEGY = FILE_COPY`. `pg_upgrade` does not carry segment files over; use a
logical dump or logical replication to upgrade.

Segment files left behind by a crash between a `DROP` and its file removal
can be removed with `SELECT appender_gc()`. `appender_relation_size(regclass)`
reports the on-disk size of a table (`pg_relation_size` does not see segment
files).

Do not use in production.

## Performance

PostgreSQL 18.6 release build (`-O3`, no assertions), `shared_buffers = 1GB`,
Apple M-series laptop, medians of 3 runs. Rows of 4 columns with a 64-byte
payload.

| | heap | appender |
| --- | --- | --- |
| bulk load, rows/s (`INSERT ... SELECT` 1M rows) | 707,884 | **1,019,649** |
| WAL, bytes/row | 136.4 | **96.6** |
| size on disk, bytes/row | 117.0 | **96.1** |
| `count(*)` over 1M rows, single process, ms | **34.8** | 38.4 |
| `count(*)` over 1M rows, 2 workers, ms | **15.5** | 16.4 |
| 1 row per commit, 4 clients, commits/s | **60,053** | 31,250 |
| 100 rows per commit, 4 clients, rows/s | **432,900** | 234,000 |

Bulk loads are faster and smaller. Scans are close to heap: rows are deformed
only up to the columns a query needs, and block checksums are computed in
three interleaved streams; what remains is copying from the OS page cache.
Short transactions pay for the extra fsync at commit.

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

- the SQL regression test (`sql/appender.sql`);
- the isolation test of concurrent writers and readers (`specs/`);
- the TAP tests (`t/`): crash recovery, streaming replication and promotion,
  point-in-time recovery, incremental backup and `pg_rewind`.

The server must load the library (`shared_preload_libraries`). The TAP tests
are skipped unless PostgreSQL was built with `--enable-tap-tests`. Set
`PGPORT` if the test cluster is not on the default port.

## License

Apache-2.0. See `LICENSE`.
