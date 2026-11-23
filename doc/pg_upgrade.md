# Upgrading a cluster with `pg_upgrade`

`pg_upgrade` copies the catalog and the relation files it knows about (the
main fork, 1 GB extents, `_fsm`, `_vm`).  `immotus_appender` keeps its rows in
segment files (`<relfilenode>.dat.<segno>`) that `pg_upgrade` does not know
about, so a stock upgrade restores appender tables as **empty shells**: the
catalog rows and the `appender_meta` manifest arrive, but the segments stay
behind in the old cluster.

The fix is one extra step: copy the segment files and fsync them.  The on-disk
format (segment header, block header, manifest columns) carries no PostgreSQL
version-specific state, so nothing is rewritten after the copy.

> **Read this first.** Back up.  Rehearse on a copy.  Stop the old cluster
> cleanly (a fast shutdown checkpoints) and keep both clusters stopped while
> copying.  `pg_upgrade` preserves database OIDs, table OIDs and relfilenodes,
> and appender names its files by relfilenode, so the old files are valid
> as-is in the new cluster — no WAL replay, no rewrite.

## The common case: one node, default tablespace

This is the scenario for almost everyone: one primary, appender tables in the
database's default tablespace, no standbys to rebuild.

```bash
# 0. Parameters.  BINDIR is the old binaries, BINDIR_NEW the new ones.
OLD_DATA=/var/lib/postgresql/18/data      NEW_DATA=/var/lib/postgresql/19/data
OLD_BIN=/usr/lib/postgresql/18/bin        NEW_BIN=/usr/lib/postgresql/19/bin
OLD_PORT=5432                             NEW_PORT=5433

# 1. Stop the old cluster cleanly.
"$OLD_BIN/pg_ctl" -D "$OLD_DATA" -m fast -w stop

# 2. Run pg_upgrade.  Appender tables come up empty here; that is expected.
"$NEW_BIN/pg_upgrade" --check -b "$OLD_BIN" -B "$NEW_BIN" \
    -d "$OLD_DATA" -D "$NEW_DATA" -p "$OLD_PORT" -P "$NEW_PORT"
"$NEW_BIN/pg_upgrade"         -b "$OLD_BIN" -B "$NEW_BIN" \
    -d "$OLD_DATA" -D "$NEW_DATA" -p "$OLD_PORT" -P "$NEW_PORT"

# 3. Both clusters are stopped now.  Copy every segment file across and sync
#    them: they are outside smgr, so nobody else will.
find "$OLD_DATA/base" -type f -name '*.dat.*' | while IFS= read -r f; do
    rel=${f#"$OLD_DATA"/}
    mkdir -p "$NEW_DATA/$(dirname "$rel")"
    cp -p "$f" "$NEW_DATA/$rel"
done
"$NEW_BIN/initdb" --sync-only "$NEW_DATA"

# 4. Start the new cluster and verify.
"$NEW_BIN/pg_ctl" -D "$NEW_DATA" -w start
```

### Why each step

1. **Clean shutdown.**  The copy is a physical copy; an unclean old cluster
   must be recovered before you start, or the manifest and segment files may
   disagree.
2. **`pg_upgrade`.**  It moves the catalog, the `appender_meta` manifest and
   every regular relation.  Appender segments are skipped because they are not
   forks of the relation.
3. **Copy + `initdb --sync-only`.**  `pg_upgrade` leaves both clusters
   stopped; copying is a plain `cp -p` of every `*.dat.*` file, preserving the
   directory layout (`base/<dboid>/...`).  `initdb --sync-only` fsyncs the new
   data directory in bulk — cheaper and safer than syncing file by file.
4. **Start.**  Nothing to rebuild, nothing to `VACUUM`.

### Verify the upgrade

```sql
-- Row counts must match the old cluster.
SELECT count(*) FROM audit_log;

-- The manifest and the segment files must agree.
SELECT appender_relation_size('audit_log');   -- non-zero
SELECT * FROM appender_gc();                  -- 0 orphans

-- A scan must return real rows, not an empty shell.
SELECT max(event_time), count(*) FROM audit_log;
```

## Other scenarios

### With standbys

Recreate the standbys with `pg_basebackup` from the new primary after step 4;
do not try to carry the old standby's segment files across.  Verify that
`shared_preload_libraries` includes `immotus_appender` on every node before
starting the standby.

### Appender tables in a non-default tablespace

The copy above covers exactly `base/<dboid>` of the default tablespace.  A
table in a user tablespace keeps its segments under
`pg_tblspc/<spcoid>/PG_<version>_<catversion>/<dboid>/`, and `pg_upgrade` does
not move them.  For those databases, use the logical path instead:

```bash
"$OLD_BIN/pg_dump" -Fc -d mydb -f mydb.dump
"$NEW_BIN/pg_restore" -d mydb mydb.dump
```

This rewrites the data, so it is slower, but it is the supported route.

### Only a few tables

`pg_dump`/`pg_restore` of the affected tables is often simpler than a
physical upgrade, especially when only a handful of tables use the appender
AM.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| appender table is empty after upgrade | segments not copied | redo step 3, then restart |
| `appender_relation_size()` is 0 | same | same |
| `appender_gc()` returns > 0 | orphan files from a crash between `DROP` and unlink | expected; the call removes them |
| `could not open file ".../base/.../*.dat.0"` | segment copied without its directory, or wrong database OID | check the `base/<dboid>` path and file ownership/permissions |
| `ERROR: immotus_appender must be loaded via shared_preload_libraries` | new cluster started without the library | add it to `postgresql.conf` and restart |

## Limitations

- **Default tablespace only** for the physical copy; use `pg_dump` for tables
  in user tablespaces.
- The procedure assumes the old cluster was shut down **cleanly**.  An unclean
  cluster must be recovered (start and stop it) before the copy.
- Both clusters must have `immotus_appender` installed in the same
  `$libdir` before `pg_upgrade` runs.
- `pg_upgrade --link` is not supported for appender tables: segment files must
  be copied, not hard-linked, because they live outside smgr.

