#!/usr/bin/env bash
# ============================================================================
# "30 days" lifetime simulation — the flagship demo.
#
# Simulates a month of daily ingestion + a 7-day retention window, measuring
# for each variant the total maintenance time and the on-disk footprint that
# is left after 30 days of churn.
#
#   heap_naive : heap, single table, DELETE + VACUUM        (what most teams do)
#   heap_part  : heap, daily partitions, DROP PARTITION      (best practice)
#   ao_part    : appender, daily partitions, DROP PARTITION  (this project)
#
# Usage:
#   bash bench/bench_retention_lifetime.sh
#   DAYS=30 KEEP=7 ROWS=100000 PORT=5433 bash bench/bench_retention_lifetime.sh
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

echo "==> setup: 30 days x ${ROWS} rows, keep last ${KEEP} days"

$PSQL -q <<'SQL'
DROP TABLE IF EXISTS heap_naive, heap_part, ao_part, bench_results;

CREATE TABLE heap_naive (ts timestamptz, device_id int, event_type int, payload text);
CREATE TABLE heap_part (ts timestamptz, device_id int, event_type int, payload text) PARTITION BY RANGE (ts);
CREATE TABLE ao_part   (ts timestamptz, device_id int, event_type int, payload text) PARTITION BY RANGE (ts) USING appender;

-- deterministic demo: no autovacuum in the background, we do maintenance by hand.
-- (Only heap_naive churns dead tuples; the DROP-PARTITION variants never do,
-- and storage params can't be set on a partitioned parent anyway.)
ALTER TABLE heap_naive SET (autovacuum_enabled = false);

CREATE TABLE bench_results (
    variant     text,
    total_ms    numeric,
    final_size  bigint,
    retained    bigint
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
              d, '2026-01-01'::date + (d - 1), '2026-01-01'::date + d)
FROM generate_series(1, 30) d \gexec
SELECT format('CREATE TABLE ao_part_p%s PARTITION OF ao_part FOR VALUES FROM (%L::date) TO (%L::date);',
              d, '2026-01-01'::date + (d - 1), '2026-01-01'::date + d)
FROM generate_series(1, 30) d \gexec
SQL

# ---------------------------------------------------------------------------
# Run one variant through 30 simulated days.  VACUUM cannot run inside a
# transaction/DO block, so the day loop is generated as plain statements.
# ---------------------------------------------------------------------------
run_variant() {
    local variant="$1" table="$2" mode="$3"
    echo "==> $variant"

    {
        printf 'SELECT clock_timestamp() AS t0 \\gset\n'
        for ((d = 1; d <= DAYS; d++)); do
            # Day's ingestion: 100k events spread across the day's 86400 seconds.
            cat <<SQL
INSERT INTO $table
SELECT ('$BASE'::date + $((d - 1))) + (g % 86400) * interval '1 second',
       g % 10000, g % 50, repeat('x', 64)
FROM generate_series(1, $ROWS) g;
SQL
            case "$mode" in
                naive)
                    # Retention: delete everything older than KEEP days.
                    cat <<SQL
DELETE FROM $table WHERE ts < ('$BASE'::date + $((d - KEEP)));
VACUUM $table;
SQL
                    ;;
                part)
                    # Retention: drop the partition older than KEEP days.
                    if (( d > KEEP )); then
                        echo "DROP TABLE ${table}_p$((d - KEEP));"
                    fi
                    echo "VACUUM $table;"
                    ;;
            esac
        done

        # Final measurement: elapsed wall time + live footprint + retained rows.
        cat <<SQL
INSERT INTO bench_results
SELECT '$variant',
       (extract(epoch FROM clock_timestamp() - :'t0') * 1000)::numeric,
       (SELECT CASE WHEN c.relkind = 'p' THEN
                        (SELECT coalesce(sum(bench_size(cc.oid)), 0)
                           FROM pg_class cc
                           JOIN pg_inherits ii ON ii.inhrelid = cc.oid
                          WHERE ii.inhparent = c.oid)
                    ELSE bench_size(c.oid)
               END
          FROM pg_class c WHERE c.oid = '$table'::regclass),
       (SELECT count(*) FROM $table);
SQL
    } | $PSQL
}

run_variant heap_naive heap_naive naive
run_variant heap_part  heap_part  part
run_variant ao_part    ao_part    part

echo "==> results (30 days, keep ${KEEP})"
$PSQL -c "SELECT variant,
                 round(total_ms)            AS total_ms,
                 pg_size_pretty(final_size) AS final_size,
                 retained                   AS retained_rows
            FROM bench_results
           ORDER BY final_size;"
