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
-- VACUUM has nothing to prune or freeze, it only refreshes the statistics
VACUUM st;
SELECT reltuples, relpages > 0 AS has_pages FROM pg_class WHERE relname = 'st';
SELECT count(*) FROM st;
VACUUM (FREEZE, VERBOSE) st;
VACUUM FULL st;
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

-- a DEFAULT partition takes the rows no other partition matches
CREATE TABLE pdd (k int, v text) PARTITION BY RANGE (k) USING appender;
CREATE TABLE pdd_lo PARTITION OF pdd FOR VALUES FROM (0) TO (100);
CREATE TABLE pdd_def PARTITION OF pdd DEFAULT;
INSERT INTO pdd SELECT g, 'v' || g FROM generate_series(1, 300) g;
SELECT tableoid::regclass AS part, count(*) FROM pdd GROUP BY 1 ORDER BY 1;
-- validating a new partition scans the default one
CREATE TABLE pdd_mid (k int, v text);
ALTER TABLE pdd ATTACH PARTITION pdd_mid FOR VALUES FROM (200) TO (400);
DROP TABLE pdd_mid, pdd;

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

-- claiming: a writer reuses the OPEN segment instead of starting a new one
SET immotus_appender.segment_size = '1MB';
CREATE TABLE cl (a int, b text) USING appender;
INSERT INTO cl SELECT g, repeat('x', 100) FROM generate_series(1, 30000) g;
SELECT count(*) FILTER (WHERE state = 1) > 1 AS sealed_many,
       count(*) FILTER (WHERE state = 0) AS open
  FROM segs('cl');
SELECT count(*) AS nsegs, max(segno) AS maxsegno FROM segs('cl') \gset
SELECT segno AS open_seg, rowcount AS open_rows FROM segs('cl') WHERE state = 0 \gset
INSERT INTO cl VALUES (0, 'next');
SELECT count(*) = :nsegs AS no_new_segment FROM segs('cl');
SELECT rowcount = :open_rows + 1 AS row_in_open FROM segs('cl') WHERE segno = :open_seg;

-- a segment of another format version is never appended to: the next free
-- number (max + 1) is used instead and the old segment stays OPEN
SELECT relfilenode AS cl_node FROM pg_class WHERE relname = 'cl' \gset
ALTER TABLE appender_meta DISABLE TRIGGER appender_meta_guard_trigger;
UPDATE appender_meta SET format_version = 99
 WHERE relfilenode = :cl_node AND segno = :open_seg;
INSERT INTO cl VALUES (0, 'newer');
SELECT segno = :maxsegno + 1 AS next_number, state FROM segs('cl') WHERE segno > :maxsegno;
SELECT state, rowcount = :open_rows + 1 AS untouched FROM segs('cl') WHERE segno = :open_seg;
UPDATE appender_meta SET format_version = 1
 WHERE relfilenode = :cl_node AND segno = :open_seg;
ALTER TABLE appender_meta ENABLE TRIGGER appender_meta_guard_trigger;
SELECT count(*), sum(a) FROM cl;
RESET immotus_appender.segment_size;
DROP TABLE cl;

-- TID reads never return data of a segment file that was removed in place
SET enable_seqscan = off;
BEGIN;
CREATE TABLE tc (a int, b text) USING appender;
EXPLAIN (costs off) SELECT a, b FROM tc WHERE ctid = '(65536,1)';
INSERT INTO tc VALUES (1, 'old') RETURNING ctid AS c1 \gset
SELECT a, b FROM tc WHERE ctid = :'c1';
TRUNCATE tc;
INSERT INTO tc VALUES (2, 'new') RETURNING ctid AS c2 \gset
SELECT :'c1' = :'c2' AS same_tid;
SELECT a, b FROM tc WHERE ctid = :'c2';
COMMIT;
SELECT a, b FROM tc WHERE ctid = :'c2';

BEGIN;
CREATE TABLE tc2 (a int, b text) USING appender;
SAVEPOINT sp;
INSERT INTO tc2 VALUES (1, 'rolled back') RETURNING ctid AS c1 \gset
SELECT b FROM tc2 WHERE ctid = :'c1';
ROLLBACK TO sp;
INSERT INTO tc2 VALUES (2, 'kept') RETURNING ctid AS c2 \gset
SELECT :'c1' = :'c2' AS same_tid;
SELECT b FROM tc2 WHERE ctid = :'c2';
COMMIT;
SELECT b FROM tc2 WHERE ctid = :'c2';
RESET enable_seqscan;
DROP TABLE tc, tc2;

-- a cached segment range must not outlive an in-place TRUNCATE
BEGIN;
CREATE TABLE trc (a int) USING appender;
INSERT INTO trc SELECT g FROM generate_series(1, 200) g;
SELECT ctid AS tr_old FROM trc WHERE a = 150 \gset
SELECT count(*) FROM trc WHERE ctid = :'tr_old';
TRUNCATE trc;
INSERT INTO trc VALUES (1), (2);
SELECT count(*) FROM trc WHERE ctid = :'tr_old';
SELECT count(*) FROM trc;
INSERT INTO trc VALUES (3) RETURNING ctid AS tr_new \gset
SELECT a FROM trc WHERE ctid = :'tr_new';
COMMIT;
DROP TABLE trc;

-- foreign keys: the referenced table must be heap, since appender has no
-- unique indexes, but the child can be appender and its AFTER ROW triggers
-- fetch every new row by TID
CREATE TABLE fkp (id int PRIMARY KEY);
INSERT INTO fkp SELECT generate_series(1, 100) g;
CREATE TABLE fkc (a int REFERENCES fkp, b text) USING appender;
INSERT INTO fkc SELECT g % 100 + 1, 'ok' FROM generate_series(1, 1000) g;
SELECT count(*) FROM fkc;
INSERT INTO fkc VALUES (1000, 'bad');
INSERT INTO fkc VALUES (1, 'after');
SELECT count(*), count(DISTINCT a) FROM fkc;
DROP TABLE fkc, fkp;

-- COPY uses the batched insert path: NULLs, short and externalized values,
-- block and segment boundaries, then a different column count after DROP
CREATE TABLE cp (a int, b text, c bool) USING appender;
ALTER TABLE cp ALTER COLUMN b SET STORAGE EXTERNAL;
COPY (SELECT g,
             CASE WHEN g % 3 = 0 THEN NULL
                  WHEN g % 3 = 1 THEN 'v' || g
                  ELSE repeat(md5(g::text), 100) END,
             g % 2 = 0
      FROM generate_series(1, 5000) g)
  TO '/tmp/immotus_copy_batch.txt';
COPY cp FROM '/tmp/immotus_copy_batch.txt';
SELECT count(*), count(b), sum(a) FROM cp;
SELECT min(length(b)), max(length(b)) FROM cp;
SELECT a, b IS NULL AS is_null, length(b) FROM cp WHERE a IN (1, 2, 3) ORDER BY a;
SELECT count(*) FROM cp WHERE ctid = ANY (ARRAY(SELECT ctid FROM cp WHERE a <= 10));
COPY (SELECT g, 'x' || g FROM generate_series(1, 100) g)
  TO '/tmp/immotus_copy_batch2.txt';
ALTER TABLE cp DROP COLUMN c;
COPY cp FROM '/tmp/immotus_copy_batch2.txt';
SELECT count(*), count(b) FROM cp;
DROP TABLE cp;

-- COPY FREEZE has nothing to freeze here, but the contract around it holds
BEGIN;
CREATE TABLE cf (a int, b text) USING appender;
COPY cf FROM '/tmp/immotus_copy_batch2.txt' WITH (FREEZE);
COMMIT;
SELECT count(*) FROM cf;
COPY cf FROM '/tmp/immotus_copy_batch2.txt' WITH (FREEZE);
BEGIN;
TRUNCATE cf;
COPY cf FROM '/tmp/immotus_copy_batch2.txt' WITH (FREEZE);
COMMIT;
SELECT count(*) FROM cf;
DROP TABLE cf;

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

-- logical replication: appender tables cannot be published
CREATE PUBLICATION appender_pub FOR TABLE t;
CREATE TABLE ht (a int);
SET client_min_messages = error;
CREATE PUBLICATION appender_pub_heap FOR TABLE ht;
RESET client_min_messages;
ALTER PUBLICATION appender_pub_heap ADD TABLE ht, t;
ALTER PUBLICATION appender_pub_heap ADD TABLES IN SCHEMA public;
ALTER PUBLICATION appender_pub_heap SET TABLE ht;
DROP PUBLICATION appender_pub_heap;
DROP TABLE ht;

DROP TABLE t, h, s, s2, al, st, sg, big, pt;
SELECT appender_gc();

-- both manifest indexes are intact after inserts, TRUNCATE and DROP
CREATE EXTENSION amcheck;
SELECT bt_index_check('appender_meta_key'), bt_index_check('appender_meta_state');
DROP EXTENSION amcheck;

-- and an existing publication blocks new appender tables in its scope
CREATE SCHEMA rep;
SET client_min_messages = error;
CREATE PUBLICATION appender_pub_rep FOR TABLES IN SCHEMA rep;
RESET client_min_messages;
CREATE TABLE rep.t4 (a int) USING appender;
SET client_min_messages = error;
CREATE PUBLICATION appender_pub_all FOR ALL TABLES;
RESET client_min_messages;
CREATE TABLE t3 (a int) USING appender;
CREATE TABLE ht (a int);
ALTER TABLE ht SET ACCESS METHOD appender;
CREATE TABLE tpart (a int) PARTITION BY RANGE (a) USING appender;
DROP PUBLICATION appender_pub_all, appender_pub_rep;
CREATE TABLE t3 (a int) USING appender;
INSERT INTO t3 VALUES (1);
SELECT count(*) FROM t3;
DROP TABLE t3, ht;
DROP SCHEMA rep;

DROP FUNCTION segs(regclass);
DROP EXTENSION immotus_appender;
