-- ============================================================================
-- immotus_appender — the event log showcase
--
-- What this script demonstrates, in order:
--
--   1. the write path — the same rows cost less WAL and less disk than heap;
--   2. the guarantee  — a closed day cannot be updated or deleted, by anyone;
--   3. retention      — a day is removed with DROP and leaves nothing behind.
--
-- It also shows the trade: a closed day is scanned sequentially, so the open
-- day stays on heap with indexes for point lookups.
--
-- Usage (superuser, the extension preloaded and created):
--   psql -p 5432 -d postgres -f bench/demo_event_log.sql
--
-- The script drops and recreates a table named `events`.
-- ============================================================================

\pset pager off
\timing on

\echo ''
\echo '### 0. One partitioned table.  Yesterday closed on appender, today'
\echo '###    open on heap — no indexes yet, so step 1 compares the write'
\echo '###    path cleanly.'
DROP TABLE IF EXISTS events CASCADE;
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

\echo ''
\echo '### 1. The write path: the same 100 000 rows, measured twice.'
\echo '###    Appender first — WAL bytes for this insert:'
SELECT pg_current_wal_lsn() AS wal_before \gset
INSERT INTO events
SELECT '2026-11-20'::timestamptz + ((g % 86400) || ' s')::interval,
       'svc' || (g % 5), g % 1000, repeat('m', 80)
  FROM generate_series(1, 100000) g;
SELECT pg_size_pretty(pg_wal_lsn_diff(pg_current_wal_lsn(), :'wal_before')) AS appender_wal;

\echo '###    Heap next — WAL bytes for the same rows:'
SELECT pg_current_wal_lsn() AS wal_before \gset
INSERT INTO events
SELECT '2026-11-21'::timestamptz + ((g % 86400) || ' s')::interval,
       'svc' || (g % 5), g % 1000, repeat('m', 80)
  FROM generate_series(1, 100000) g;
SELECT pg_size_pretty(pg_wal_lsn_diff(pg_current_wal_lsn(), :'wal_before')) AS heap_wal;

\echo ''
\echo '### 2. The open day now gets its indexes for point lookups — that is'
\echo '###    the heap side of the trade.'
CREATE INDEX ON events_20261121 (uid, ts);
CREATE INDEX ON events_20261121 (svc, ts);

\echo ''
\echo '### 3. What the same 100 000 rows take on disk:'
ANALYZE events_20261120;
ANALYZE events_20261121;
SELECT 'appender' AS engine,
       pg_size_pretty(appender_relation_size('events_20261120')) AS on_disk,
       pg_relation_size('events_20261120') AS pg_relation_size
UNION ALL
SELECT 'heap',
       pg_size_pretty(pg_relation_size('events_20261121')),
       pg_relation_size('events_20261121');

\echo ''
\echo '### 4. The guarantee: a closed day cannot be rewritten — not by the'
\echo '###    application, not by a DBA.'
\set ON_ERROR_STOP off
UPDATE events SET msg = 'tampered' WHERE ts < '2026-11-21';
\echo '###    and:'
DELETE FROM events WHERE ts < '2026-11-21';
\set ON_ERROR_STOP on

\echo ''
\echo '### 5. An incident: the point lookup on the open day uses its index.'
EXPLAIN (COSTS OFF)
SELECT * FROM events WHERE uid = 42 AND ts >= '2026-11-21';

\echo ''
\echo '### 6. The day is over.  Drop the indexes and switch the partition to'
\echo '###    appender in place — same table, same name, same rows.'
DROP INDEX events_20261121_uid_ts_idx;
DROP INDEX events_20261121_svc_ts_idx;
ALTER TABLE events_20261121 SET ACCESS METHOD appender;
SELECT amname AS access_method,
       (SELECT count(*) FROM events_20261121) AS rows_still_there
  FROM pg_class c JOIN pg_am a ON a.oid = c.relam
 WHERE c.relname = 'events_20261121';

\echo ''
\echo '### 7. Retention.  The oldest day is dropped, not deleted — no dead'
\echo '###    rows, no vacuum, and the files are gone at commit.'
DROP TABLE events_20261120;
SELECT appender_gc() AS orphans_after_drop;
SELECT count(*) AS rows_left FROM events;

\echo ''
\echo '### 8. Tomorrow opens as a fresh heap partition; one dashboard plan'
\echo '###    reads both storage engines.'
CREATE TABLE events_20261122 PARTITION OF events
    FOR VALUES FROM ('2026-11-22') TO ('2026-11-23');
CREATE INDEX ON events_20261122 (uid, ts);
INSERT INTO events
SELECT '2026-11-22'::timestamptz + ((g % 3600) || ' s')::interval,
       'svc' || (g % 5), g % 1000, repeat('m', 80)
  FROM generate_series(1, 5000) g;

EXPLAIN (COSTS OFF)
SELECT svc, count(*) FROM events GROUP BY svc;
SELECT svc, count(*) FROM events GROUP BY svc ORDER BY 2 DESC LIMIT 3;

\echo ''
\echo '### Done.  What to look at:'
\echo '###   step 1 — less WAL for the same rows on appender'
\echo '###   step 3 — pg_relation_size() reports 0, appender_relation_size() the files'
\echo '###   step 4 — UPDATE and DELETE are rejected by the engine'
\echo '###   step 5 — the open day is indexed, so point lookups stay fast'
\echo '###   step 7 — retention is a DROP; nothing is left behind'
\echo '###   step 8 — one query plan over heap and appender partitions'

DROP TABLE IF EXISTS events CASCADE;
