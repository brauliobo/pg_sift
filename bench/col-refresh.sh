#!/usr/bin/env bash
# usage: bench/col-refresh.sh ROWS...   (a cluster from scripts/pgtest.sh with SIFT_TMP and SIFT_PORT set and started)
# For each size: a table of 8 columns in four segments (the build and three promotions) and an index without cube_minmax. Then the plan cost of a
# min/max GROUP BY after 0, 1, 2, 3 single steps of the refresh and when all is done (the planner prices the units that have the min and max), the time of the refresh
# (median of 3 rounds: ALTER INDEX .. SET (cube_minmax = false), refresh, ALTER INDEX .. SET (cube_minmax = true), then one segment, then the rest),
# the index and pre-aggregate bytes before and after, a rebuild of the index with the option for comparison, and the time of the query scanned and
# answered from the groups. Output: one tab separated line per measure.
set -euo pipefail
cd "$(dirname "$0")/.."
TMP=${SIFT_TMP:-/tmp/sift-arch}
export PGHOST=$TMP/sock PGPORT=${SIFT_PORT:-56100}
P="psql -X -q -At -U postgres sift -v ON_ERROR_STOP=1"
Q="SELECT kind, min(val), max(val), min(amt), max(amt), count(*) FROM bm GROUP BY kind"
IDX="CREATE INDEX bm_idx ON bm USING sift_col (id, kind, grp, k8, kn, val, small, amt) WITH (write_buffer_limit = 4096"

median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }
timed() { # SQL -> elapsed ms of running it in one session
	local t0 t1
	t0=$(date +%s%N)
	$P -c "SET statement_timeout='60s'" -c "$1" >/dev/null
	t1=$(date +%s%N)
	echo $(((t1 - t0) / 1000000))
}
qtime() { # push preagg -> median ms of 5 runs of the query
	$P -c "SET statement_timeout='30s'" -c "SELECT bt(\$q\$$Q\$q\$, $1, $2)" | tail -1
}
cost() { # the total cost of the plan of the query
	$P -c "SET max_parallel_workers_per_gather = 0" -c "SET seq_page_cost = 10000" -c "SELECT bm_cost(\$q\$$Q\$q\$)" | tail -1
}
info() { $P -c "SELECT $1 FROM sift.col_index_info('bm_idx')"; }

echo -e "rows\tmeasure\tvalue"
for N in "$@"; do
	Q4=$((N / 4))
	cat <<SQL | $P >/dev/null
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET statement_timeout='60s';
DROP TABLE IF EXISTS bm;
CREATE TABLE bm (id int, kind int, grp text, k8 bigint, kn numeric(12,2), val bigint, small int, amt numeric(10,2), pad text) WITH (autovacuum_enabled=off);
CREATE OR REPLACE FUNCTION bm_fill(a int, b int) RETURNS void LANGUAGE sql AS \$f\$
INSERT INTO bm SELECT g, g % 6, 'g' || lpad((g % 30)::text, 2, '0'),
  CASE g % 9 WHEN 0 THEN 4611686018427387904 WHEN 1 THEN -4611686018427387904 WHEN 2 THEN NULL ELSE g % 5 * 100 END,
  (g % 13) * 0.25 + (g % 3) * 0.5, CASE WHEN g % 7 = 0 THEN NULL ELSE (g * 977) % 100000 - 50000 END,
  (g * 37) % 1000 - 400, ((g * 91) % 20000) / 100.0, repeat('p', 100) FROM generate_series(a, b) g \$f\$;
SELECT bm_fill(1, $Q4);
$IDX);
SELECT bm_fill($Q4 + 1, 2 * $Q4);
SELECT sift.col_promote('bm_idx');
SELECT bm_fill(2 * $Q4 + 1, 3 * $Q4);
SELECT sift.col_promote('bm_idx');
SELECT bm_fill(3 * $Q4 + 1, $N);
SELECT sift.col_promote('bm_idx');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) bm;
CREATE OR REPLACE FUNCTION bt(q text, push bool, pre bool, n int DEFAULT 5) RETURNS numeric LANGUAGE plpgsql AS \$f\$
DECLARE t timestamptz; a numeric[] := '{}'; i int; r record;
BEGIN
  PERFORM set_config('sift.enable_agg', push::text, false); PERFORM set_config('sift.agg_preagg', pre::text, false);
  PERFORM set_config('max_parallel_workers_per_gather', '0', false);
  FOR i IN 0..n LOOP
    t := clock_timestamp(); FOR r IN EXECUTE q LOOP NULL; END LOOP; IF i > 0 THEN a := a || extract(epoch FROM clock_timestamp() - t)::numeric * 1000; END IF;
  END LOOP;
  RETURN (SELECT round(percentile_cont(0.5) WITHIN GROUP (ORDER BY x)::numeric, 3) FROM unnest(a) x);
END \$f\$;
CREATE OR REPLACE FUNCTION bm_cost(q text) RETURNS numeric LANGUAGE plpgsql AS \$f\$
DECLARE j jsonb;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
  RETURN round((j -> 0 -> 'Plan' ->> 'Total Cost')::numeric, 2);
END \$f\$;
SQL
	echo -e "$N\tsegments\t$(info 'segments - buffers')"
	echo -e "$N\tunits\t$(info 'units')"
	echo -e "$N\tindex_bytes_without\t$(info 'bytes')"
	echo -e "$N\tpreagg_bytes_without\t$(info 'preagg_bytes')"
	echo -e "$N\tquery_ms_scanned_without\t$(qtime true false)"
	echo -e "$N\tplan_cost_option_off_before\t$(cost)"
	$P -c "ALTER INDEX bm_idx SET (cube_minmax = true)"
	echo -e "$N\tplan_cost_after_0_steps\t$(cost)"
	for k in 1 2 3; do
		$P -c "SELECT * FROM sift.col_refresh('bm_idx', 1)" >/dev/null
		echo -e "$N\tplan_cost_after_${k}_steps\t$(cost)"
	done
	$P -c "SELECT * FROM sift.col_refresh('bm_idx')" >/dev/null
	echo -e "$N\tplan_cost_all_refreshed\t$(cost)"
	on=() off=() one=()
	for _ in 1 2 3; do
		$P -c "ALTER INDEX bm_idx SET (cube_minmax = false)"
		off+=("$(timed "SELECT * FROM sift.col_refresh('bm_idx')")")
		$P -c "ALTER INDEX bm_idx SET (cube_minmax = true)"
		one+=("$(timed "SELECT * FROM sift.col_refresh('bm_idx', 1)")")
		on+=("$(timed "SELECT * FROM sift.col_refresh('bm_idx')")")
	done
	echo -e "$N\trefresh_ms_one_segment\t$(printf '%s\n' "${one[@]}" | median)"
	echo -e "$N\trefresh_ms_rest_adding_minmax\t$(printf '%s\n' "${on[@]}" | median)"
	echo -e "$N\trefresh_ms_all_dropping_minmax\t$(printf '%s\n' "${off[@]}" | median)"
	echo -e "$N\tindex_bytes_with\t$(info 'bytes')"
	echo -e "$N\tpreagg_bytes_with\t$(info 'preagg_bytes')"
	echo -e "$N\trelation_bytes_after_rounds\t$($P -c "SELECT pg_relation_size('bm_idx')")"
	$P -c "SELECT 'verify', sift.col_verify('bm_idx', true)" >&2
	echo -e "$N\tquery_ms_scanned_with\t$(qtime true false)"
	echo -e "$N\tquery_ms_groups_with\t$(qtime true true)"
	echo -e "$N\tquery_ms_normal_plan\t$(qtime false false)"
	echo -e "$N\trebuild_ms_with_minmax\t$(timed "DROP INDEX bm_idx; $IDX, cube_minmax = true)")"
	echo -e "$N\trebuild_index_bytes_with\t$(info 'bytes')"
done
