#!/usr/bin/env bash
# Table access method benchmarks: bulk load, scans, commit throughput, large
# values.  Needs a server reached through the usual PG* variables as a
# superuser (COPY uses a server file in /tmp), and psql, pgbench and bc on the
# same host as the server; appender needs immotus_appender preloaded.
# Prints one "RESULT <bench> <am> <metric> <value>" line per run.
#
#   RUNS=5 DUR=15 NROWS=1000000 NVAL=2000 AMS="heap appender" bench/run.sh
set -euo pipefail

RUNS=${RUNS:-5}
DUR=${DUR:-15}
NROWS=${NROWS:-1000000}
NVAL=${NVAL:-2000}
AMS=${AMS:-heap appender}

q() { psql -X -q -At -v ON_ERROR_STOP=1 "$@"; }
r() { echo "RESULT $*"; }

COLS="ts timestamptz, device_id int, event_type int, payload text"
ROW="now(), g % 10000, g % 50, repeat('x', 64)"

size_of() {	# $1 = am, $2 = table
	if [[ $1 == appender ]]; then
		q -c "SELECT appender_relation_size('$2')"
	else
		q -c "SELECT pg_relation_size('$2')"
	fi
}

if [[ " $AMS " == *" appender "* ]]; then
	q -c "CREATE EXTENSION IF NOT EXISTS immotus_appender"
fi
q -c "SELECT 'settings: ' || string_agg(name || '=' || setting, ' ' ORDER BY name)
        FROM pg_settings WHERE name IN ('fsync','synchronous_commit','wal_sync_method',
          'shared_buffers','max_wal_size','server_version','wal_level',
          'immotus_appender.prealloc_chunk','immotus_appender.segment_size')"

# --- bulk load: INSERT ... SELECT and COPY --------------------------------
q -c "COPY (SELECT $ROW FROM generate_series(1, $NROWS) g) TO '/tmp/bench_rows.copy'"
for run in $(seq "$RUNS"); do
	for am in $AMS; do
		for how in insert copy; do
			q -c "DROP TABLE IF EXISTS b" -c "CREATE TABLE b ($COLS) USING $am" -c "CHECKPOINT"
			if [[ $how == insert ]]; then
				stmt="INSERT INTO b SELECT $ROW FROM generate_series(1, $NROWS) g"
			else
				stmt="COPY b FROM '/tmp/bench_rows.copy'"
			fi
			out=$(q <<-SQL
				SELECT pg_current_wal_lsn() AS l0, clock_timestamp() AS t0 \gset
				$stmt;
				SELECT round(extract(epoch FROM clock_timestamp() - :'t0'::timestamptz)::numeric, 4)
				       || ' ' || pg_wal_lsn_diff(pg_current_wal_lsn(), :'l0'::pg_lsn);
			SQL
			)
			read -r sec wal <<<"$out"
			r load_$how $am rows_per_s "$(echo "$NROWS / $sec" | bc)"
			r load_$how $am wal_bytes_per_row "$(echo "scale=1; $wal / $NROWS" | bc)"
			[[ $how == insert ]] && r load_insert $am size_bytes_per_row \
				"$(echo "scale=1; $(size_of $am b) / $NROWS" | bc)"
		done
	done
done
q -c "DROP TABLE IF EXISTS b"

# --- sequential scans (warm cache) ----------------------------------------
for am in $AMS; do
	q -c "DROP TABLE IF EXISTS s_$am" -c "CREATE TABLE s_$am ($COLS) USING $am" \
	  -c "INSERT INTO s_$am SELECT $ROW FROM generate_series(1, $NROWS) g" \
	  -c "VACUUM ANALYZE s_$am"
done
scan() {	# $1 = label, $2 = settings, $3 = query with %t for the table
	for am in $AMS; do
		sql=${3//%t/s_$am}
		times=$(q <<-SQL
			$2
			$sql; $sql;
			\timing on
			$(for _ in $(seq "$RUNS"); do echo "$sql;"; done)
		SQL
		)
		grep -o 'Time: [0-9.]*' <<<"$times" | awk '{print $2}' |
			while read -r ms; do r "scan_$1" $am ms "$ms"; done
	done
}
SERIAL="SET max_parallel_workers_per_gather = 0;"
PARALLEL="SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0;
          SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;"
scan count_serial "$SERIAL" "SELECT count(*) FROM %t"
scan count_parallel "$PARALLEL" "SELECT count(*) FROM %t"
scan allcols_serial "$SERIAL" \
	"SELECT max(ts), sum(device_id), sum(event_type), sum(length(payload)) FROM %t"
for am in $AMS; do q -c "DROP TABLE s_$am"; done

# --- commit throughput ----------------------------------------------------
echo "INSERT INTO c VALUES (now(), 1, 1, repeat('x', 64));" > /tmp/bench_c1.sql
echo "INSERT INTO c SELECT $ROW FROM generate_series(1, 100) g;" > /tmp/bench_c100.sql
for clients in 1 8; do
	for rows in 1 100; do
		for run in $(seq "$RUNS"); do
			for am in $AMS; do
				q -c "DROP TABLE IF EXISTS c" -c "CREATE TABLE c ($COLS) USING $am" -c "CHECKPOINT"
				tps=$(pgbench -n -c "$clients" -j "$clients" -T "$DUR" \
					-f /tmp/bench_c$rows.sql 2>/dev/null | awk '/^tps/ {print $3}')
				r "commit_c${clients}_r$rows" $am commits_per_s "$tps"
			done
		done
	done
done
q -c "DROP TABLE IF EXISTS c"

# --- large values (EXTERNAL, ~100 KB each) --------------------------------
for run in $(seq "$RUNS"); do
	for am in $AMS; do
		q -c "DROP TABLE IF EXISTS v" -c "CREATE TABLE v (id int, body text) USING $am" \
		  -c "ALTER TABLE v ALTER COLUMN body SET STORAGE EXTERNAL" -c "CHECKPOINT"
		out=$(q <<-SQL
			SELECT clock_timestamp() AS t0 \gset
			INSERT INTO v SELECT g, repeat(md5(g::text), 3200) FROM generate_series(1, $NVAL) g;
			SELECT round(extract(epoch FROM clock_timestamp() - :'t0'::timestamptz)::numeric, 4);
		SQL
		)
		r values_insert $am mb_per_s "$(echo "scale=1; $NVAL * 102400 / 1048576 / $out" | bc)"
		times=$(q <<-SQL
			SELECT sum(length(body)) FROM v;
			\timing on
			SELECT sum(length(body)) FROM v;
		SQL
		)
		r values_read $am ms "$(grep -o 'Time: [0-9.]*' <<<"$times" | awk '{print $2}')"
	done
done
q -c "DROP TABLE IF EXISTS v"
rm -f /tmp/bench_rows.copy /tmp/bench_c1.sql /tmp/bench_c100.sql
echo DONE
