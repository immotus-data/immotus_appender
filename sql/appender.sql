-- Regression tests for the appender table access method.

CREATE EXTENSION immotus_appender;

CREATE TABLE t (a int, b text) USING appender;

INSERT INTO t VALUES (1, 'one'), (2, 'two'), (3, 'three');
INSERT INTO t SELECT g, 'n' || g::text FROM generate_series(4, 100) g;

-- sequential scan returns all rows in insertion order
SELECT count(*) AS rows FROM t;
SELECT a, b FROM t WHERE a IN (1, 100) ORDER BY a;
SELECT min(a) AS min, max(a) AS max, sum(a) AS sum FROM t;

-- UPDATE and DELETE are rejected
UPDATE t SET b = 'x' WHERE a = 1;
DELETE FROM t WHERE a = 1;

-- EOF metadata is recorded
SELECT relid::regclass AS rel, filenum, eof > 0 AS eof_recorded
  FROM appender_meta
 WHERE relid = 't'::regclass;

-- direct DML on the metadata table is blocked
DELETE FROM appender_meta;

-- indexes are not supported yet
CREATE INDEX t_a_idx ON t (a);

DROP TABLE t;
DROP EXTENSION immotus_appender;
