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

Appender tables are stored in append-only data files
(`<relfilenode>.dat.<filenum>`) that bypass the shared buffer manager. A data
file is a sequence of blocks: a 16-byte header followed by a batch of tuples,
packed back-to-back. A row is addressed by its block offset and its position
within the block, which is its TID.

A logical EOF per data file is kept in `immotus_meta.datafiles` and updated
once per transaction at commit. Row data is not WAL-logged; durability comes
from fsync'ing dirty data files at commit together with the EOF update, so
WAL volume is ~0 bytes per row.

## Status

Version 0.1. `INSERT`, `COPY` and `SELECT` (sequential scan) work, with
crash-safe durability. `UPDATE` and `DELETE` are not supported. Indexes,
TOAST and `ANALYZE` are not implemented yet.

Do not use in production.

## Performance

Measured with 1M rows, 4 columns, 64-byte payload, PostgreSQL 18:

| metric | heap | appender | change |
| --- | --- | --- | --- |
| rows/s | 552,618 | 1,797,611 | +225% |
| WAL bytes/row | 136.4 | 0.0 | -100% |
| bytes/row (size) | 117.03 | 96.05 | -17.9% |

## Quick start

```bash
git clone https://github.com/immotus-data/immotus_appender.git
cd immotus_appender
make install PG_CONFIG=/path/to/pg_config
```

`immotus_appender` uses a custom WAL resource manager, so it must be loaded
at server startup. Add this to `postgresql.conf` and restart:

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
