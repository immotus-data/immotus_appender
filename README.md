# immotus_appender

Append-only table access method for PostgreSQL.

`immotus_appender` is a PostgreSQL extension that provides the `appender`
table access method. Tables created with `USING appender` are write-once:
rows are only ever appended and never updated or deleted, so there are no
dead tuples, no vacuum, and no bloat.

## Why

PostgreSQL's built-in heap storage implements MVCC by keeping old tuple
versions and removing them with autovacuum. On write-heavy immutable
workloads (audit logs, event streams, IoT telemetry) autovacuum can fall
behind, leaving dead tuples that bloat tables and slow down scans.

Appender tables avoid this by design:

- no `UPDATE` / `DELETE`, so no dead tuples are ever produced;
- no vacuum, so no IO spikes and no tuning;
- append-only data files, so inserts always write to the end of the file.

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

Every block is WAL-logged by a custom resource manager, and touched segments
are fsynced at commit before the manifest update.

## Status

Version 0.1 (in development). Supported: `INSERT`, `COPY`, sequential and
parallel scans, savepoints, `TRUNCATE`, `ALTER TABLE` (including rewrites and
`SET ACCESS METHOD`), `ANALYZE`, partitioning.

Not supported: `UPDATE`, `DELETE`, `INSERT ... ON CONFLICT`, indexes, backward
scans, `TABLESAMPLE`, unlogged and temporary tables, two-phase commit,
`CLUSTER`/`VACUUM FULL`, moving a table to another tablespace.
`CREATE DATABASE` from a template that contains appender tables requires
`STRATEGY = FILE_COPY`. `pg_upgrade` does not carry segment files over; use a
logical dump or logical replication to upgrade.

Do not use in production.

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

`make installcheck` runs the SQL regression test (`sql/appender.sql`) and the
TAP test (`t/001_basic.pl`) against a running server. The TAP test is skipped
unless PostgreSQL was built with `--enable-tap-tests`. Set `PGPORT` if the
test cluster is not on the default port.

## License

Apache-2.0. See `LICENSE`.
