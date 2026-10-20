# Major upgrades with `pg_upgrade`

`pg_upgrade` migrates a cluster to a new major version by copying the catalog
and the relation files it knows about (the main fork, 1 GB extents, `_fsm`,
`_vm`).  `immotus_appender` keeps its rows in its own segment files
(`<relfilenode>.dat.<segno>`) that `pg_upgrade` does not know about, so the
stock flow restores appender tables as empty shells: the catalog rows and the
`appender_meta` manifest are transferred, but the segments are left behind in
the old cluster.  Upgrading therefore needs one extra manual step.

The on-disk format of segments (file header, block header, manifest columns)
carries no PostgreSQL version-specific state, so nothing has to be rewritten
after the copy.

> **Note.** Always back up first and rehearse against a copy. The old cluster
> must be shut down cleanly (a fast stop checkpoints) before the copy, and both
> clusters must be stopped during the copy. `pg_upgrade` preserves database
> OIDs, table OIDs and relfilenodes, and appender names its files by
> relfilenode, so the old cluster's files are valid as-is in the new cluster
> and need no WAL replay.

## Procedure

Both clusters must have `immotus_appender` installed (the shared library in
`$libdir` of the new binaries) before you start.

```bash
# 0. OLD and NEW data directories, bindirs and ports.
OLD_DATA=/path/to/old/data      NEW_DATA=/path/to/new/data
OLD_BINDIR=/path/to/old/bin     NEW_BINDIR=/path/to/new/bin
OLD_PORT=5432                   NEW_PORT=5433

# 1. Stop the old cluster cleanly.
"$OLD_BINDIR/pg_ctl" -D "$OLD_DATA" -m fast -w stop

# 2. Run pg_upgrade as usual.  Appender tables come up as empty shells here:
#    their segments still live in the old cluster.
"$NEW_BINDIR/pg_upgrade" --check \
    -b "$OLD_BINDIR" -B "$NEW_BINDIR" \
    -d "$OLD_DATA"   -D "$NEW_DATA" \
    -p "$OLD_PORT"   -P "$NEW_PORT"

"$NEW_BINDIR/pg_upgrade" \
    -b "$OLD_BINDIR" -B "$NEW_BINDIR" \
    -d "$OLD_DATA"   -D "$NEW_DATA" \
    -p "$OLD_PORT"   -P "$NEW_PORT"

# 3. Both clusters are stopped now.  Copy the segment files pg_upgrade left
#    behind, then fsync them: the segment files are outside smgr, so nobody
#    else will sync them.
find "$OLD_DATA/base" -type f -name '*.dat.*' | while IFS= read -r f; do
    rel="${f#"$OLD_DATA"/}"
    mkdir -p "$NEW_DATA/$(dirname "$rel")"
    cp -p "$f" "$NEW_DATA/$rel"
done
"$NEW_BINDIR/initdb" --sync-only "$NEW_DATA"

# 4. Start the new cluster.
"$NEW_BINDIR/pg_ctl" -D "$NEW_DATA" -w start
```

## Limitations

- **Default tablespace only.**  Segment files live under `base/<dboid>` of the
  database's default tablespace; the copy above covers exactly that.  Appender
  tables in a user tablespace are not carried over — upgrade those databases
  with `pg_dump`/`pg_restore` or logical replication instead.
- The procedure is a physical copy: it assumes the old cluster was shut down
  cleanly.  A crashed (unclean) old cluster must be recovered first.
