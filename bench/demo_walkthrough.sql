-- ============================================================================
-- immotus_appender — walkthrough / simulation script
--
-- Step-by-step demonstration of the append-only table lifecycle:
--   CREATE -> INSERT -> (aborted insert) -> SELECT -> backward scan
--   -> DELETE(fail) -> UPDATE(fail) -> VACUUM(stats refresh)
--   -> VACUUM FULL(fail) -> TRUNCATE(works!) -> CHECKPOINT -> DROP
--
-- Usage:
--   psql -p 5433 -U postgres -d postgres -f bench/demo_walkthrough.sql
--
-- The script does NOT stop on errors (\set ON_ERROR_STOP off): several
-- operations are expected to fail for this table access method.
-- ============================================================================

\set ON_ERROR_STOP off
\pset pager off

\echo ''
\echo '### STEP 1: CREATE EXTENSION + CREATE TABLE ... USING appender'
CREATE EXTENSION IF NOT EXISTS immotus_appender;
DROP TABLE IF EXISTS demo;
-- -> appender_relation_set_new_filelocator() (creates an empty main fork)
CREATE TABLE demo (id int, payload text) USING appender;
SELECT pg_relation_filepath('demo') AS filepath,
       pg_size_pretty(pg_relation_size('demo')) AS size_after_create,
       relpages, reltuples
  FROM pg_class WHERE relname = 'demo';

\echo ''
\echo '### STEP 2: INSERT (VALUES + generate_series)'
-- VALUES -> appender_multi_insert(); generate_series -> the same path
INSERT INTO demo VALUES (1, 'alpha'), (2, 'beta'), (3, 'gamma');
INSERT INTO demo SELECT g, 'row' || g FROM generate_series(4, 1000) g;
SELECT count(*) AS rows_now FROM demo;
SELECT pg_relation_filepath('demo') AS filepath,
       pg_size_pretty(pg_relation_size('demo')) AS size_after_insert,
       relpages, reltuples
  FROM pg_class WHERE relname = 'demo';

\echo ''
\echo '### STEP 2b: aborted insert (ROLLBACK) — the tuple is gone for good'
BEGIN;
INSERT INTO demo VALUES (9999, 'ghost');
ROLLBACK;
SELECT count(*) AS rows_after_rollback, max(ctid) AS highest_ctid FROM demo;

\echo ''
\echo '### STEP 2c: INSERT ... SELECT from itself (Halloween-safe via curcid)'
INSERT INTO demo SELECT id, payload FROM demo;
SELECT count(*) AS rows_after_self_insert FROM demo;

\echo ''
\echo '### STEP 3: SELECT (seq scan) — the scan machinery at work'
SELECT ctid, id, payload FROM demo ORDER BY id LIMIT 3;
SELECT ctid AS first_ctid FROM demo ORDER BY id ASC LIMIT 1;
SELECT ctid AS last_ctid  FROM demo ORDER BY id DESC LIMIT 1;
EXPLAIN (COSTS OFF) SELECT * FROM demo WHERE id = 500;
SELECT ctid, id, payload FROM demo WHERE id = 500;

\echo ''
\echo '### STEP 3b: backward scan — bare SeqScan without Sort (really drives the AM backwards)'
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT ctid, id FROM demo;
FETCH FORWARD 3 FROM c;
FETCH BACKWARD 2 FROM c;
CLOSE c;
COMMIT;

\echo ''
\echo '### STEP 4: DELETE — EXPECT AN ERROR'
DELETE FROM demo WHERE id = 1;

\echo ''
\echo '### STEP 4b: UPDATE — EXPECT AN ERROR'
UPDATE demo SET payload = 'mutated' WHERE id = 2;

\echo ''
\echo '### STEP 5: VACUUM (lazy) — refreshes size estimate, nothing to prune'
\echo '-- append-only: dead tuples never sit on-page and MemTuples carry no xmin;'
\echo '-- VACUUM only refreshes relpages (size estimate from committed EOF):'
VACUUM VERBOSE demo;
SELECT relpages, reltuples, relfrozenxid FROM pg_class WHERE relname = 'demo';

\echo ''
\echo '### STEP 5b: VACUUM FULL — EXPECT AN ERROR (rewrite via copy_for_cluster)'
VACUUM FULL demo;

\echo ''
\echo '### STEP 5c: TRUNCATE — WORKS (new relfilenode via set_new_filelocator)'
SELECT pg_relation_filepath('demo') AS filepath_before_truncate;
TRUNCATE demo;
SELECT pg_relation_filepath('demo') AS filepath_after_truncate;
SELECT count(*) AS rows_after_truncate FROM demo;
-- the table is still usable:
INSERT INTO demo VALUES (7, 'post-truncate');
SELECT * FROM demo;

\echo ''
\echo '### STEP 6: CHECKPOINT'
CHECKPOINT;

\echo ''
\echo '### STEP 7: DROP TABLE'
DROP TABLE demo;
-- EOF
