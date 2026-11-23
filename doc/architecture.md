# Architecture

How `immotus_appender` stores and reads rows, and why it is fast on
append-only data.

```
                 SQL: INSERT / COPY / SELECT
                            │
                 ┌──────────▼───────────┐
                 │  appender table AM   │  appender.c
                 └──────┬───────┬───────┘
                        │       │
        writes ┌────────▼─┐   ┌─▼──────────────┐
               │  writer  │   │  scan / fetch  │
               │ (backend)│   │   (backend)    │
               └────┬─────┘   └───────┬────────┘
                    │                 │
        ┌───────────▼─────────────────▼──────────┐
        │   segment files <relfilenode>.dat.N    │  appender_datafile.c
        │   (outside the buffer manager)         │
        └───────────────────┬────────────────────┘
                            │ one row per segment,
                            │ committed end only
                    ┌───────▼────────┐
                    │  appender_meta │  manifest (heap table)
                    └────────────────┘
```

## 1. Rows

A row is a **MemTuple**: a compact record with

- no `xmin`/`xmax`/`ctid`/`infomask` and no per-row MVCC header;
- attributes in **alignment order** (8/4/2/1-byte classes), described by a
  `MemTupleBinding` computed once per tuple descriptor and cached;
- a **null bitmap** (one bit per null) instead of padding;
- inline **short varlena** values (≤ 126 bytes) with a 1-byte header.

Two bindings are built per descriptor: a 2-byte offset layout for tuples up to
64 KB and a 4-byte offset layout for larger ones.

The tuple descriptor is truncated to the number of attributes the block was
written with, so `ALTER TABLE ... ADD COLUMN` leaves old blocks readable.

## 2. Blocks and segments

Rows are packed into **blocks** of about 32 KB or 4096 rows, whichever comes
first.  A block is written once, checksummed, and never modified:

```
block: [ AppenderBlockHeader | payload ]
        version, kind, rowcount, first_rownum,
        datalen, rawlen, natts, codec, crc32c
```

Blocks are appended to **segment files** named
`<relfilenode>.dat.<segno>` in the relation's directory.  They are opened
through `fd.c` and are **never read through or written to shared buffers**:

- a bulk load cannot evict the buffer cache;
- a scan reads through the OS page cache (which is why scans of narrow
  columns trail heap the most);
- the checkpoint never has to write them.

A segment is **sealed** when it reaches
`immotus_appender.segment_size` (default 1 GB) or exhausts its row-number
space.  Sealed segments are immutable forever.

A sealed segment is also the unit a future cold tier would move off the
primary: it never changes, the manifest row already describes its contents
(`state`, `eof`, `rowcount`), and the rows inside are addressed by logical
TIDs that do not depend on where the file lives.  Segment storage was designed
with this in mind.  Today the files sit in the relation's tablespace directory
and are read from there directly; there is no offline or external location
yet, and moving a sealed segment away is not supported.

```
<relfilenode>.dat.0   [file header][data blocks][value blocks][valdir]...
<relfilenode>.dat.1   ...
```

The file header carries a magic, a format version and the segment number.
Segment files are zero-filled ahead in `prealloc_chunk` (1 MB) steps, so a
commit inside a chunk does not pay for a file-size change; the physical file
can be up to one chunk longer than its logical end.

## 3. Checksums

Every block header carries a **CRC32C** over the header and payload.  On ARMv8
and x86-64 the CRC runs on the hardware instruction (`crc32cd` / `crc32q`);
because the instruction has ~3-cycle latency but 1-cycle throughput, the
computation is split into three independent stripes and merged with
precomputed linear tables, roughly tripling the rate.

A block that fails validation raises `ERRCODE_DATA_CORRUPTED`; the reader
never trusts a length or a kind before checking it.

## 4. Large values

Large `varlena` values do not go to a side TOAST table.  When a value exceeds
`APPENDER_TOAST_THRESHOLD` (2 KB) and its storage allows it, it is written as
a **value block** in the same segment and the row stores a standard
`VARTAG_ONDISK` TOAST pointer, so the rest of PostgreSQL (including
`pg_dump`) sees an ordinary external value.

Each segment keeps a **value directory** (`VALDIR`) block chain: entries are
sorted by value id and chained backwards, so a lookup is a binary search, not
a scan.  Directories are cached per backend in an LRU bounded by
`valdir_cache_size` (64 MB).

Reads stream a slice of the value block and verify the whole-block CRC on the
fly, so fetching 1 KB of a 1 GB value never materializes the value.

## 5. The manifest, and what "committed" means

`appender_meta` is an ordinary heap table in the extension schema with one row
per segment:

```
(spcoid, relfilenode, segno) -> state, eof, rowcount, valdir_off, format_version
```

**A transaction's appends become visible when its manifest update commits.**
The data blocks are already in the file; the manifest row is the commit point.
That gives atomic appends and MVCC-correct visibility with no per-row state.

A transaction that has not committed yet keeps its own positions in backend
memory (per segment: `eof`, `rows`, and a mark per command id), so:

- a statement sees the rows it just wrote;
- a savepoint rollback restores positions and later writes overwrite the
  dead tail;
- other backends see nothing until commit.

A statement-level trigger rejects any direct DML on `appender_meta`.

## 6. Concurrency

Writers claim **open** segments with transaction-scoped object locks on
`(appender_meta, relfilenode, segno)`; a claim never waits.

1. Try open segments, **fullest first** (fewer active segments).
2. If none is available, create the next segment number.

So the number of open segments is bounded by the **peak number of concurrent
writers**, not by the table size, and a table with one writer has exactly one
open segment.

Positions are re-read after the lock is taken, because the previous holder may
have committed in between.

## 7. WAL and recovery

Segment changes are logged by a **custom resource manager**
(`RM_APPENDER_ID`): create, block write, sync and unlink.

- **Redo of data blocks is skipped**: the bytes are already in the files, and
  the manifest is the commit point.
- **Redo of create/unlink is replayed**, so a crash never leaves a segment
  the catalog and manifest do not know about.
- Streaming replication, PITR, `pg_rewind` and base backups work because the
  records are ordinary WAL.
- Incremental backup copies segment files in full; PostgreSQL tracks changed
  blocks only for its own relation files.

## 8. Durability and commit cost

A commit that appended rows:

1. `fdatasync`s every touched segment (they are outside smgr, so nobody else
   will sync them), then
2. commits the manifest update, which flushes WAL as usual.

So a commit costs **one extra sync per touched segment** compared with heap.
Single-row transactions therefore lose the most to heap, and batching rows per
commit narrows the gap; at 8 clients heap's WAL group commit widens it again.
The measured commit ratios are in [benchmarks.md](benchmarks.md).

`synchronous_commit = off` does **not** remove the segment sync: a commit
record could reach disk without its data, and local crash recovery does not
replay segment data.

## 9. File layout and sizing

| Item | Location | Notes |
| --- | --- | --- |
| segment files | `base/<dboid>/<relfilenode>.dat.<segno>` | outside smgr, see below |
| manifest | `appender_meta` (extension schema) | one row per segment |
| TID | logical `(segno, rownum)` | 16-bit segment, 24-bit row |

`pg_relation_size()`, `pg_table_size()` and `pg_total_relation_size()` are
smgr-based and report **0** for appender tables, and `pg_database_size()` does
not count the segment files either.  Use `appender_relation_size(regclass)`.

`pg_class.relpages` and `reltuples` are refreshed by `VACUUM` and by `ANALYZE`,
and both land on the same numbers: the page count is what the `relation_size`
callback reports, the row count comes from the manifest.  The planner does not
read them — it calls `relation_estimate_size()` on every plan.

`relfrozenxid` and `relminmxid` are not part of that: a table created as
appender has neither, because no row ever carries a transaction id.  A table
converted from heap has both, and core's `vac_update_datfrozenxid()` takes the
minimum over every relation with a valid horizon, so a value left in place
would hold back `pg_database.datfrozenxid` until the cluster stops for
wraparound.  `appender_vacuum_rel()` therefore advances an existing horizon to
`GetOldestNonRemovableTransactionId()` / `GetOldestMultiXactId()` — which is
safe precisely because there is nothing older in the relation — while leaving
an absent one absent.

## 10. Hooks and callbacks

An access method is not shown everything: statements that would copy or
publish a table never reach it, and the visibility of one session's appends
has to survive commit, abort and cache invalidation.  Six entry points are
registered in `_PG_init`; the module therefore has to be preloaded.

| Entry point | What it does |
| --- | --- |
| `ProcessUtility_hook` | Checks `CREATE PUBLICATION` and `ALTER PUBLICATION ... ADD/SET` before they run and refuses a publication that would cover an appender table, because a subscriber would decode the table as if it were heap. Checks `CREATE DATABASE` as well: a template holding appender tables can only be copied with `STRATEGY = FILE_COPY`. |
| `object_access_hook` | On `OAT_POST_CREATE` it catches appender relations that never reach `relation_set_new_filelocator()` — partitioned parents, and tables created after a publication already covers their schema. On `OAT_DROP` it schedules the segment files for removal at commit, deletes the manifest rows and drops the cached entries. |
| `RegisterXactCallback` | Flushes the buffered rows at pre-commit; refuses `PREPARE TRANSACTION` once the transaction has touched an appender table; closes file handles, caches and pending storage at commit or abort. |
| `RegisterSubXactCallback` | Keeps the rows a subtransaction appended when it commits with its parent, and drops them when it aborts. |
| `CacheRegisterRelcacheCallback` | Forgets the memtuple bindings, the manifest cache and the value directory cache when a relation changes, so a rewritten table is never read through a stale descriptor. |
| `RegisterCustomRmgr` | Makes redo recognise the records described in §7.  WAL that contains appender records cannot be replayed without the library, which is why it must be preloaded on every node. |

The two hook chains (`ProcessUtility` and `object_access`) call the previous
handler first or last as appropriate, so other extensions in the same server
keep working.

## 11. Limits

| Limit | Value | Why |
| --- | --- | --- |
| segments per storage (lifetime) | 65 534 | 16-bit segment field in the TID |
| rows per segment | 16 777 216 | 24-bit row field in the TID |
| storage per table at 1 GB segments | ≈ 64 TB | 65 534 × 1 GB |
| attributes per row | PostgreSQL's own limit | tuple descriptor |
| segment location | the relation's tablespace directory | sealed segments are read there; tiering to external storage is the planned direction, not supported yet |

Segment numbers are not reused until `TRUNCATE` or a rewrite, which is why
`immotus_appender.segment_size` is superuser-only: a tiny segment size burns
through the 65 534 numbers quickly.
