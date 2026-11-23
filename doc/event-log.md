# An event log: cheaper to write, impossible to rewrite

`immotus_appender` is for data that is written once and read forever, and the
cleanest way to see why is an event or audit log.  Events arrive all day,
every day; they are read in two ways -- point lookups while they are recent,
scans and aggregates once they have aged -- and they are kept for months or
years.  The write path runs constantly; the read path is occasional.  That
is the workload the access method is built for.

Heap charges this workload in three places that an appender table does not:

- **WAL volume.**  A heap bulk load rewrites blocks and logs full-page images
after every checkpoint; an appender block is written once and needs none.
Measured per row, `INSERT ... SELECT` writes 136.4 bytes of WAL on heap and
96.5 on appender -- 29% less for the same rows.  Less WAL means less I/O on
the primary and less traffic to every standby.
- **Row size.**  A heap row carries `xmin`, `xmax`, `ctid` and `infomask` even
though nobody will ever update it.  An appender row carries none: 96.4 bytes
against 117.0, 18% smaller.
- **Maintenance.**  Heap rows age, and their transaction horizons have to be
frozen for as long as the table exists.  An appender table has no dead tuples
and, when it was created as appender, no horizons at all: autovacuum has no
reason to visit it, and it never becomes an anti-wraparound candidate.

The trade is read speed: an appender scan verifies a checksum per block and
reads through the OS page cache instead of shared buffers, so `count(*)` runs
at 0.55x heap and a four-column aggregate at 0.85x.  That is why the hot window stays on heap and only the closed days move
to appender.

## One table, two storage engines

| | the open day | a closed day |
| --- | --- | --- |
| storage | heap | appender |
| indexes | on `(uid, ts)` and `(svc, ts)` | none |
| queries | point lookups, "the last events for this service" | scans over a time range, aggregates |
| updates | possible, if an event has to be annotated | rejected by the engine |
| maintenance | vacuum and freeze, as usual | none, and no freeze visits |

The application does not know about any of this: it inserts into the parent
and queries the parent.  The planner picks the partition, and the partition
brings its own storage engine.

```sql
CREATE TABLE events (
    ts  timestamptz NOT NULL,
    svc text,
    uid bigint,
    msg text
) PARTITION BY RANGE (ts);

CREATE TABLE events_20261120 PARTITION OF events
    FOR VALUES FROM ('2026-11-20') TO ('2026-11-21') USING appender;

CREATE TABLE events_20261121 PARTITION OF events
    FOR VALUES FROM ('2026-11-21') TO ('2026-11-22');
CREATE INDEX ON events_20261121 (uid, ts);
CREATE INDEX ON events_20261121 (svc, ts);
```

## The write path, measured

All numbers come from [benchmarks](benchmarks.md), and
`bench/demo_event_log.sql` reproduces the WAL and size figures live:

| | heap | appender |
| --- | ---: | ---: |
| WAL for `INSERT ... SELECT` | 136.4 bytes/row | **96.5 bytes/row** |
| size on disk | 117.0 bytes/row | **96.4 bytes/row** |
| bulk load speed | 1.00 | **1.48** |

Text and binary `COPY` are at parity with heap -- the win is in
`INSERT ... SELECT`, the load pattern of ETL jobs.

## Immutability is a guarantee, not a convention

A closed day cannot be changed by the application, by a batch job, or by a
DBA with a fat-fingered `UPDATE`.  The storage engine rejects the statement:

```
ERROR:  UPDATE is not supported for appender tables
ERROR:  DELETE is not supported for appender tables
```

That is the property an audit log exists to have: history stays history, and
the guarantee comes from the engine, not from discipline.  If a correction is
ever needed, append the newer version of the row and let readers pick the
latest one -- the usual event-sourcing pattern.

## Closing the day

Three ways, all supported; pick whichever fits the maintenance window.

**In place, when the day is over.**  The partition stays attached and keeps
its name, so nothing has to be re-attached and no query has to change.  The
indexes have to go first, because the access method has none:

```sql
DROP INDEX events_20261121_uid_ts_idx, events_20261121_svc_ts_idx;
ALTER TABLE events_20261121 SET ACCESS METHOD appender;
```

This is a rewrite, so it locks that partition (and only that partition) for
the duration.  Two hundred thousand rows took about 250 ms in the demo.

**By copying into a new partition.**  More steps, but nothing is rewritten in
place and the old table can be kept for a while:

```sql
ALTER TABLE events DETACH PARTITION events_20261121;
CREATE TABLE events_20261121_cold PARTITION OF events
    FOR VALUES FROM ('2026-11-21') TO ('2026-11-22') USING appender;
INSERT INTO events_20261121_cold SELECT * FROM events_20261121;
DROP TABLE events_20261121;
```

**Never on heap.**  If the application only ever inserts, write the day
straight into an appender partition and keep a heap partition only for the
hours where corrections are still possible.  This is the cheapest option and
the one to prefer when the workload allows it.

## Retention

```sql
DROP TABLE events_20260120;
```

That is the whole job.

- **No maintenance.**  An appender partition has no dead tuples and, when it
was created as appender, no `relfrozenxid`: autovacuum never visits it, and
it is never frozen.  A heap partition gets no new writes either, but it
still ages toward wraparound and is frozen from time to time.  (A partition
switched from heap keeps its old `relfrozenxid`; one `VACUUM` right after
the switch moves it to the present.)
- **A smaller footprint.**  The same day takes 18% less space than on heap,
because a row carries no `xmin`/`xmax`/`ctid`/`infomask`.
- **Nothing left for a scan to read.**  Neither variant leaves dead rows
behind after a clean `DROP`; the segment files are unlinked at commit, so
`SELECT appender_gc()` finds nothing to collect.

## What it costs

The measured trade-offs:

- **Scans are slower.**  `count(*)` runs at 0.55x heap and a four-column
aggregate at 0.85x, because every block is checksummed and read through the
OS page cache instead of shared buffers.  A closed day is scanned
sequentially, so queries over it should be aggregates or time-bounded scans,
not point lookups.
- **Commit latency is higher.**  A commit that appended rows also `fdatasync`s
the touched segments, so appender commit throughput is 0.38-0.54x heap.
Batch rows per transaction and keep write transactions short.
- **No indexes yet.**  Every query over an appender partition is a sequential
scan; partition by time and restrict `ts`.
- **`pg_relation_size()` reports 0**, because the segment files are outside
smgr.  Use `appender_relation_size(regclass)`; `pg_database_size()` does not
count the segments either.
- **`pg_upgrade` needs one extra step** -- the segment files are copied by
hand; see [pg_upgrade](pg_upgrade.md).
- **`ANALYZE` still matters** for column statistics, exactly as on heap.

## Trying it

`bench/demo_event_log.sql` runs the whole story in a few seconds: the write
path measured in WAL bytes against heap, the
sizes, the rejected `UPDATE`/`DELETE`, an indexed point lookup on the open day,
closing the day in place, retention by `DROP`, and a dashboard query over both
kinds of partition.

```sh
psql -p 5432 -d postgres -f bench/demo_event_log.sql
```
