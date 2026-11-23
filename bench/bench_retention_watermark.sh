#!/usr/bin/env bash
# ============================================================================
# Retention "watermark" demo — the flagship.
#
# Simulates a very common operational pattern: a backlog of 30 days is ingested
# in bulk, and THEN retention is applied (keep the last KEEP days).  This is
# where heap and appender diverge structurally:
#
#   * heap  : DELETE + VACUUM removes the rows but the FILE NEVER SHRINKS —
#             the table stays at its high-water mark unless you rewrite it
#             (VACUUM FULL / pg_repack, which lock or duplicate the table).
#   * appender: DROP PARTITION unlinks whole files instantly — footprint drops
#             to exactly KEEP days, no rewrite, no lock on live data.
#
# Variants:
#   heap_naive : heap, single table, DELETE + VACUUM   (what most teams do)
#   heap_part  : heap, daily partitions, DROP PARTITION (best practice = fair)
#   ao_part    : appender, daily partitions, DROP PARTITION
#
# Usage:
#   bash bench/bench_retention_watermark.sh
#   DAYS=30 KEEP=7 ROWS=100000 PORT=5433 bash bench/bench_retention_watermark.sh
#
# Requires: immotus_appender installed AND shared_preload_libraries='immotus_appender'.
# ============================================================================
set -euo pipefail

PGROOT=${PGROOT:-$HOME/pgsql_meson}
PORT=${PORT:-5433}
DB=${DB:-postgres}
DAYS=${DAYS:-30}
KEEP=${KEEP:-7}
ROWS=${ROWS:-100000}
BASE=${BASE:-2026-01-01}

PSQL="$PGROOT/bin/psql -p $PORT -d $DB -X -v ON_ERROR_STOP=1"
DROP_PARTITIONS=$((DAYS - KEEP))   # number of oldest partitions to drop

echo "==> setup: bulk ${DAYS} days x ${ROWS} rows, then keep last ${KEEP} days"

$PSQL -q <<'SQL'
DROP TABLE IF EXISTS heap_naive, heap_part, ao_part, bench_results;

CREATE TABLE heap_naive (ts timestamptz, device_id int, event_type int, payload text);
CREATE TABLE heap_part (ts timestamptz, device_id int, event_type int, payload text) PARTITION BY RANGE (ts);
CREATE TABLE ao_part   (ts timestamptz, device_id int, event_type int, payload text) PARTITION BY RANGE (ts) USING appender;

ALTER TABLE heap_naive SET (autovacuum_enabled = false);

CREATE TABLE bench_results (
    variant      text,
    retention_ms numeric,
    before_size  bigint,
    after_size   bigint,
    scan_ms      numeric DEFAULT NULL
);

CREATE OR REPLACE FUNCTION bench_size(oid) RETURNS bigint
LANGUAGE sql STABLE AS $$
  SELECT CASE WHEN relam = (SELECT oid FROM pg_am WHERE amname = 'appender')
              THEN appender_relation_size($1::regclass)
              ELSE pg_relation_size($1)
         END
    FROM pg_class WHERE oid = $1;
$$;
SQL

# Pre-create one partition per day for both partitioned variants.
$PSQL -q <<'SQL'
SELECT format('CREATE TABLE heap_part_p%s PARTITION OF heap_part FOR VALUES FROM (%L::date) TO (%L::date);',
              d, '2026-01-01'::date + d, '2026-01-01'::date + d + 1)
FROM generate_series(0, 29) d \gexec
SELECT format('CREATE TABLE ao_part_p%s PARTITION OF ao_part FOR VALUES FROM (%L::date) TO (%L::date);',
              d, '2026-01-01'::date + d, '2026-01-01'::date + d + 1)
FROM generate_series(0, 29) d \gexec
SQL

# SQL fragment that yields the live size of a table (works for partitioned too).
size_sql() {
    cat <<SQL
(SELECT CASE WHEN c.relkind = 'p' THEN
                 (SELECT coalesce(sum(bench_size(cc.oid)), 0)
                    FROM pg_class cc
                    JOIN pg_inherits ii ON ii.inhrelid = cc.oid
                   WHERE ii.inhparent = c.oid)
             ELSE bench_size(c.oid)
        END
   FROM pg_class c WHERE c.oid = '$1'::regclass)
SQL
}

# ---------------------------------------------------------------------------
# Bulk-ingest the full backlog into a table (30 simulated days).
# ---------------------------------------------------------------------------
bulk_load() {
    local table="$1"
    $PSQL -q -c "
INSERT INTO $table
SELECT ('$BASE'::date + d) + (g % 86400) * interval '1 second',
       g % 10000, g % 50, repeat('x', 64)
FROM generate_series(0, $((DAYS - 1))) d, generate_series(1, $ROWS) g;"
}

# ---------------------------------------------------------------------------
# Apply retention to one variant and record timing + footprint delta.
# The before-size and start timestamp are captured in the same psql session as
# the retention statements, so psql \gset variables stay in scope.
# ---------------------------------------------------------------------------
retain_variant() {
    local variant="$1" table="$2" mode="$3"
    echo "==> $variant (bulk ingest...)"
    bulk_load "$table"

    {
        printf 'SELECT clock_timestamp() AS t0, %s AS before_size \\gset\n' \
            "$(size_sql "$table")"
        case "$mode" in
            naive)
                cat <<SQL
DELETE FROM $table WHERE ts < '$BASE'::date + $DROP_PARTITIONS;
VACUUM $table;
SQL
                ;;
            part)
                for p in $(seq 0 $((DROP_PARTITIONS - 1))); do
                    echo "DROP TABLE ${table}_p${p};"
                done
                ;;
        esac
        cat <<SQL
INSERT INTO bench_results
SELECT '$variant',
       (extract(epoch FROM clock_timestamp() - :'t0') * 1000)::numeric,
       :before_size,
       $(size_sql "$table");

-- Warm-cache full sequential scan of the post-retention table.  A seq scan
-- must read EVERY page in the file, including empty/bloated ones, so this is
-- the "day-30 degradation" number: heap_naive reads 335MB of pages, appender 78MB.
SELECT clock_timestamp() AS t1 \gset
SELECT count(*) AS retained_rows FROM $table;
UPDATE bench_results
   SET scan_ms = (extract(epoch FROM clock_timestamp() - :'t1') * 1000)::numeric
 WHERE variant = '$variant';
SQL
    } | $PSQL
}

retain_variant heap_naive heap_naive naive
retain_variant heap_part  heap_part  part
retain_variant ao_part    ao_part    part

echo "==> results (bulk ${DAYS} days, keep ${KEEP})"
$PSQL -c "SELECT variant,
                 pg_size_pretty(before_size)  AS before_retention,
                 pg_size_pretty(after_size)   AS after_retention,
                 round(retention_ms)          AS retention_ms,
                 round(scan_ms)               AS full_scan_ms
            FROM bench_results
           ORDER BY after_size;"

echo "==> buffers a full scan must read (EXPLAIN ANALYZE BUFFERS)"
$PSQL -c "EXPLAIN (ANALYZE, BUFFERS, TIMING OFF) SELECT count(*) FROM heap_naive;" \
     -c "EXPLAIN (ANALYZE, BUFFERS, TIMING OFF) SELECT count(*) FROM ao_part;"
