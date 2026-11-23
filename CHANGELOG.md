# Changelog

What changed in each release of `immotus_appender`.  Versions follow semantic
versioning: a major release may change the on-disk layout or the WAL records,
a minor release only adds to them.

## Unreleased

## 1.0.0

First release.

- The `appender` table access method: `CREATE TABLE ... USING appender` gives
  tables that are written once and read forever.  Rows are immutable by
  construction, so a table carries no dead tuples, no bloat and nothing to
  vacuum.
- PostgreSQL 17, 18 and 19.
- Rows live in segment files outside the buffer manager and are addressed by
  logical TIDs: scans read the files directly, and parallel scans hand out
  blocks rather than whole segments.
- TOAST works the usual way — a standard external pointer, the usual
  compression — except that the out-of-line part of a value is written to a
  value block of the same segment instead of a side TOAST table.
- A custom WAL resource manager, so crash recovery, streaming replication and
  PITR work as they do for heap tables.
- `pg_upgrade` moves appender tables to a newer major version with one extra
  step: the segment files are copied by hand, because they are not smgr forks.
  The procedure is in [doc/pg_upgrade.md](doc/pg_upgrade.md).
- `VACUUM` has nothing to reclaim, and a table created with `USING appender`
  has no transaction horizons to advance.  A table converted with
  `ALTER TABLE ... SET ACCESS METHOD appender` keeps the `relfrozenxid` of its
  heap life, so `VACUUM` moves both horizons to the present instead of leaving
  them to hold back `pg_database.datfrozenxid`.
- `appender_relation_size(regclass)` reports the footprint of the segment
  files, and `appender_gc()` removes segment files left behind by a crash
  between a `DROP` and its unlink.
- Whatever the access method does not implement — indexes, logical
  replication, `VACUUM FULL`, `CLUSTER`, `ALTER TABLE ... SET TABLESPACE`,
  two-phase commit, backward scans, `TABLESAMPLE` — fails with a clear error
  instead of returning wrong results.  See the limits in
  [README](README.md#known-limitations).
