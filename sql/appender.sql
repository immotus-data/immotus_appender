-- Regression tests for the appender table access method.

CREATE EXTENSION immotus_appender;

CREATE FUNCTION segs(regclass)
RETURNS TABLE (segno int4, state int2, rowcount int8)
LANGUAGE sql AS $$
    SELECT m.segno, m.state, m.rowcount
      FROM appender_meta m JOIN pg_class c ON c.relfilenode = m.relfilenode
     WHERE c.oid = $1
     ORDER BY m.segno
$$;

-- basic insert and scan
CREATE TABLE t (a int, b text) USING appender;
INSERT INTO t VALUES (1, 'one'), (2, 'two'), (3, 'three');
INSERT INTO t SELECT g, 'n' || g FROM generate_series(4, 100) g;
SELECT count(*), sum(a) FROM t;
SELECT ctid, a, b FROM t WHERE a IN (1, 100) ORDER BY a;
SELECT * FROM segs('t');
SELECT appender_relation_size('t') > 0 AS has_size;

-- rows are immutable
UPDATE t SET b = 'x' WHERE a = 1;
DELETE FROM t WHERE a = 1;

-- lookups by TID, own rows included
SELECT a FROM t WHERE ctid = '(65536,5)';
SELECT count(*) FROM (SELECT a FROM t FOR UPDATE) s;
BEGIN;
INSERT INTO t VALUES (101, 'own') RETURNING ctid AS own_ctid \gset
SELECT a, b FROM t WHERE ctid = :'own_ctid';
ROLLBACK;

-- INSERT ... SELECT from the same table sees only earlier rows
CREATE TABLE h (a int) USING appender;
INSERT INTO h SELECT generate_series(1, 10);
INSERT INTO h SELECT a + 100 FROM h;
SELECT count(*) FROM h;

-- a cursor sees the rows of commands before it
BEGIN;
DECLARE c CURSOR FOR SELECT count(*) FROM h;
INSERT INTO h VALUES (1);
FETCH c;
SELECT count(*) FROM h;
COMMIT;

-- subtransactions
CREATE TABLE s (a int) USING appender;
BEGIN;
INSERT INTO s VALUES (1);
SAVEPOINT sp1;
INSERT INTO s VALUES (2);
SAVEPOINT sp2;
INSERT INTO s VALUES (3);
RELEASE sp2;
ROLLBACK TO sp1;
INSERT INTO s VALUES (4);
COMMIT;
SELECT string_agg(a::text, ',' ORDER BY a) FROM s;

DO $$
BEGIN
    FOR i IN 1..5 LOOP
        BEGIN
            INSERT INTO s VALUES (10 + i);
            IF i % 2 = 0 THEN
                RAISE EXCEPTION 'roll back %', i;
            END IF;
        EXCEPTION WHEN raise_exception THEN
            NULL;
        END;
    END LOOP;
END $$;
SELECT string_agg(a::text, ',' ORDER BY a) FROM s;

-- TRUNCATE: transactional, in place, and rolled back
BEGIN;
INSERT INTO s VALUES (100);
TRUNCATE s;
INSERT INTO s VALUES (200);
COMMIT;
SELECT string_agg(a::text, ',') FROM s;

BEGIN;
CREATE TABLE s2 (a int) USING appender;
INSERT INTO s2 SELECT generate_series(1, 10);
TRUNCATE s2;
INSERT INTO s2 VALUES (42);
COMMIT;
SELECT string_agg(a::text, ',') FROM s2;

BEGIN;
INSERT INTO s VALUES (300);
SAVEPOINT x;
TRUNCATE s;
INSERT INTO s VALUES (999);
ROLLBACK TO x;
COMMIT;
SELECT string_agg(a::text, ',' ORDER BY a) FROM s;

-- schema changes
CREATE TABLE al (a int) USING appender;
INSERT INTO al VALUES (1);
ALTER TABLE al ADD COLUMN b text;
INSERT INTO al VALUES (2, 'two');
ALTER TABLE al ADD COLUMN c text DEFAULT 'def';
INSERT INTO al (a) VALUES (3);
ALTER TABLE al DROP COLUMN b;
INSERT INTO al VALUES (4, 'four');
SELECT * FROM al ORDER BY a;

ALTER TABLE al ALTER COLUMN a TYPE bigint;
SELECT pg_typeof(a), count(*), sum(a) FROM al GROUP BY 1;
ALTER TABLE al SET ACCESS METHOD heap;
ALTER TABLE al SET ACCESS METHOD appender;
SELECT count(*), sum(a) FROM al;

-- rows are deformed lazily, only up to the attributes a query needs
CREATE TABLE lz (a int, b text, c int, d text) USING appender;
INSERT INTO lz SELECT g, CASE WHEN g % 3 = 0 THEN NULL ELSE 'b' || g END,
       CASE WHEN g % 5 = 0 THEN NULL ELSE g * 10 END, repeat('d', g % 7)
  FROM generate_series(1, 1000) g;
ALTER TABLE lz ADD COLUMN e int DEFAULT 42;
SELECT count(*), sum(c), count(b), sum(length(d)), sum(e) FROM lz;
SELECT a, b, d FROM lz WHERE c IS NULL ORDER BY a LIMIT 3;
SELECT count(*) FROM lz x JOIN lz y ON x.c = y.a * 10 WHERE y.b IS NOT NULL;
DROP TABLE lz;

-- statistics
CREATE TABLE st (a int, b text) USING appender;
INSERT INTO st SELECT g, md5(g::text) FROM generate_series(1, 20000) g;
VACUUM st;
SELECT reltuples FROM pg_class WHERE relname = 'st';
ANALYZE st;
SELECT reltuples FROM pg_class WHERE relname = 'st';
SELECT attname, n_distinct FROM pg_stats WHERE tablename = 'st' ORDER BY attname;

-- segments are sealed when full
SET immotus_appender.segment_size = '1MB';
CREATE TABLE sg (a int, b text) USING appender;
INSERT INTO sg SELECT g, repeat('x', 100) FROM generate_series(1, 30000) g;
RESET immotus_appender.segment_size;
SELECT count(*) AS segments, sum(state) AS sealed, sum(rowcount) AS rows FROM segs('sg');
SELECT count(*), sum(a) FROM sg;

-- parallel scans hand out segments, the leader's own rows included
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
EXPLAIN (COSTS OFF) SELECT count(*) FROM sg;
SELECT count(*), sum(a) FROM sg;
BEGIN;
INSERT INTO sg VALUES (1000000, 'own');
SELECT count(*), sum(a) FROM sg;
ROLLBACK;
-- a single segment is shared block by block, each row read exactly once
CREATE TABLE one (a int) USING appender;
INSERT INTO one SELECT generate_series(1, 50000);
EXPLAIN (COSTS OFF) SELECT count(*), count(DISTINCT ctid), sum(a) FROM one;
SELECT count(*), count(DISTINCT ctid), sum(a) FROM one;
BEGIN;
INSERT INTO one SELECT generate_series(50001, 60000);
SELECT count(*), count(DISTINCT ctid), sum(a) FROM one;
ROLLBACK;
DROP TABLE one;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;

-- out-of-line values are copied in
CREATE TABLE src (t text);
ALTER TABLE src ALTER COLUMN t SET STORAGE EXTERNAL;
INSERT INTO src SELECT repeat(md5(g::text), 2000) FROM generate_series(1, 3) g;
CREATE TABLE big (t text) USING appender;
INSERT INTO big SELECT t FROM src;
DROP TABLE src;
SELECT count(*), sum(length(t)), bool_and(t = repeat(substr(t, 1, 32), 2000)) FROM big;

-- partitions
CREATE TABLE pt (k int, v text) PARTITION BY RANGE (k) USING appender;
DO $$
BEGIN
    FOR i IN 0..79 LOOP
        EXECUTE format('CREATE TABLE pt_%s PARTITION OF pt FOR VALUES FROM (%s) TO (%s)',
                       i, i * 100, (i + 1) * 100);
    END LOOP;
END $$;
INSERT INTO pt SELECT g % 8000, 'v' FROM generate_series(1, 40000) g;
SELECT count(*) FROM pt;

-- dropping a table removes its segments
CREATE TABLE d (a int) USING appender;
INSERT INTO d VALUES (1);
SELECT relfilenode AS d_node FROM pg_class WHERE relname = 'd' \gset
BEGIN;
DROP TABLE d;
ROLLBACK;
SELECT count(*) FROM appender_meta WHERE relfilenode = :d_node;
DROP TABLE d;
SELECT count(*) FROM appender_meta WHERE relfilenode = :d_node;

-- the manifest is guarded
DELETE FROM appender_meta;
TRUNCATE appender_meta;

-- not supported
CREATE INDEX ON t (a);
CREATE UNLOGGED TABLE u (a int) USING appender;
CREATE TEMP TABLE tmp (a int) USING appender;
BEGIN;
DECLARE sc SCROLL CURSOR FOR SELECT a FROM t;
FETCH 2 FROM sc;
FETCH PRIOR FROM sc;
ROLLBACK;
BEGIN;
INSERT INTO t VALUES (0, 'p');
PREPARE TRANSACTION 'appender';
SELECT current_database() AS db \gset
CREATE DATABASE appender_copy TEMPLATE :"db";

DROP TABLE t, h, s, s2, al, st, sg, big, pt;
SELECT appender_gc();
DROP FUNCTION segs(regclass);
DROP EXTENSION immotus_appender;
