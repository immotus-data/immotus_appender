# Concurrent writers each append to their own segment and never lose rows;
# readers see exactly the rows committed before their snapshot, plus their
# own.

setup
{
    CREATE EXTENSION IF NOT EXISTS immotus_appender;
    CREATE TABLE ct (a int, who text) USING appender;
    INSERT INTO ct SELECT g, 'base' FROM generate_series(1, 10) g;
}

teardown
{
    DROP TABLE ct;
}

session s1
step s1_begin   { BEGIN; }
step s1_rr      { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM ct; }
step s1_insert  { INSERT INTO ct SELECT g, 's1' FROM generate_series(1, 100) g; }
step s1_count   { SELECT who, count(*) FROM ct GROUP BY who ORDER BY who; }
step s1_commit  { COMMIT; }
step s1_abort   { ROLLBACK; }

session s2
step s2_begin   { BEGIN; }
step s2_insert  { INSERT INTO ct SELECT g, 's2' FROM generate_series(1, 200) g; }
step s2_count   { SELECT who, count(*) FROM ct GROUP BY who ORDER BY who; }
step s2_commit  { COMMIT; }
step s2_truncate { TRUNCATE ct; }

session s3
step s3_count   { SELECT who, count(*) FROM ct GROUP BY who ORDER BY who; }
step s3_segs    { SELECT count(*) AS segments FROM appender_meta m JOIN pg_class c ON c.relfilenode = m.relfilenode WHERE c.relname = 'ct'; }

# two open writers, interleaved: both commit, no row is lost
permutation s1_begin s2_begin s1_insert s2_insert s1_count s2_count s3_count s1_commit s3_count s2_commit s3_count s3_segs

# one writer aborts: its rows vanish, the other's survive
permutation s1_begin s2_begin s1_insert s2_insert s1_abort s2_commit s3_count

# a repeatable-read reader does not see rows committed into the segment it
# later appends to
permutation s1_rr s2_insert s1_insert s1_count s1_commit s3_count

# TRUNCATE waits for an open writer
permutation s1_begin s1_insert s2_truncate s1_commit s3_count
