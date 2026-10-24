#!/usr/bin/env bash
# The plan-choice gate of the aggregate pushdown (docs/AGG.md, "Plan-choice gate"): the shapes of test/plan/shapes.sql run on the tables of
# test/plan/gen.sql, each three ways (native: sift.enable_agg off; chosen: the planner's own choice; scan: Custom Scan (SiftAgg) forced) by
# test/plan/run.sql, which compares the results, the plan chosen against the expectation of the shape and the cost ratio against its band.
#
#   scripts/plangate.sh [--rows N] [--seed N] [--runs N] [--shapes REGEX] [--lane stock|tuned] [--warn-time] [--update] [--keep]
#
#   --rows N      events (default 30000, the size the expectations are recorded at; they hold from 20000 to 50000), --seed N the generator seed (default 1)
#   --runs N      median of N warm runs of the native and the scan path (default 3; 0: no timings)
#   --shapes RE   only the shapes whose name matches
#   --lane tuned   the planner settings of a tuned profile (seq_page_cost 100, random_page_cost 4, workers 8, hash_mem_multiplier 2, work_mem 4GB): the results must agree and a
#                 lose still fails, but the plan and the cost band are those of the stock lane, so a different plan there is read, not failed
#   --warn-time   a shape that the scan makes slower than native by more than 1 ms and 20% (verdict lose) is a warning, not a failure: hosted runners are noisy
#   --update      rewrites the `-- expect:` line of the shapes of shapes.sql from this run (their bands; the plan too where it is `new`): read the diff
#   --keep        keeps the database sift_plangate
# Fails (exit 1) on a result that differs between the paths, a plan other than the expected one, a cost ratio outside its band and (without
# --warn-time) a lose. Needs the scripts/pgtest.sh cluster installed and started (SIFT_TMP, SIFT_PORT).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
export SIFT_TMP=${SIFT_TMP:-/tmp/sift-arch}
ROWS=30000 SEED=1 RUNS=3 ONLY= WARN= UPDATE= KEEP= LANE=stock
DB=${PLANGATE_DB:-sift_plangate}

while (( $# )); do
	case $1 in
		--warn-time) WARN=1 ;; --update) UPDATE=1 ;; --keep) KEEP=1 ;;
		--rows) ROWS=$2; shift ;; --seed) SEED=$2; shift ;; --runs) RUNS=$2; shift ;; --shapes) ONLY=$2; shift ;; --lane) LANE=$2; shift ;;
		*) sed -n '2,19p' "$0"; exit 1 ;;
	esac
	shift
done

sql() { PGOPTIONS='-c client_min_messages=warning' SIFT_DB=${DBNOW:-$DB} "$REPO/scripts/pgtest.sh" psql -v ON_ERROR_STOP=1 "$@"; }
log() { printf '[plangate %s] %s\n' "$(date +%T)" "$*" >&2; }
[[ -n $KEEP ]] || trap 'DBNOW=postgres sql -qAt -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null 2>&1 || true' EXIT

DBNOW=postgres sql -qAt -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" -c "CREATE DATABASE $DB"
# serial plans, no JIT and the stock planner costs: the bands are ratios of the costs the server ships with, whatever its configuration
settings=(max_parallel_workers_per_gather=0 jit=off seq_page_cost=1 random_page_cost=4 cpu_tuple_cost=0.01 effective_cache_size=4GB work_mem=4MB)
case $LANE in
	stock) ;;
	tuned) settings=(max_parallel_workers_per_gather=8 jit=off seq_page_cost=100 random_page_cost=4 cpu_tuple_cost=0.01 effective_cache_size=4GB work_mem=4GB hash_mem_multiplier=2) ;;
	*) log "unknown lane $LANE (stock or tuned)"; exit 1 ;;
esac
for s in "${settings[@]}"; do printf "ALTER DATABASE %s SET %s TO '%s';\n" "$DB" "${s%%=*}" "${s#*=}"; done | sql -q >/dev/null
sql -qAt -c "CREATE EXTENSION pg_sift" >/dev/null
log "tables: $ROWS events, seed $SEED"
sql -q -v n="$ROWS" -v seed="$SEED" -f "$REPO/test/col/run.sql" -f "$REPO/test/plan/gen.sql" -f "$REPO/test/plan/run.sql" >/dev/null
log "shapes"
sql -q -v runs="$RUNS" -v pattern="$ONLY" >/dev/null <<-SQL
	\\set text \`cat '$REPO/test/plan/shapes.sql'\`
	SELECT plangate.run_all(:'text', :runs, :'pattern');
SQL

if [[ -n $UPDATE ]]; then
	sql -qAt -F $'\t' -c "SELECT name, expect FROM plangate.suggestions" >"${SIFT_TMP}/plangate-expect.tsv"
	awk -F'\t' 'NR == FNR { e[$1] = $2; next }
		/^-- name: / { n = substr($0, 10) }
		/^-- expect: / && (n in e) { print "-- expect: " e[n]; next }
		{ print }' "${SIFT_TMP}/plangate-expect.tsv" "$REPO/test/plan/shapes.sql" >"${SIFT_TMP}/plangate-shapes.sql"
	cat "${SIFT_TMP}/plangate-shapes.sql" >"$REPO/test/plan/shapes.sql"
	rm -f "${SIFT_TMP}/plangate-expect.tsv" "${SIFT_TMP}/plangate-shapes.sql"
	log "updated test/plan/shapes.sql"
fi

# the expected plans and cost bands are the costs of PostgreSQL 18: on another major their drift is a warning, a wrong result still fails
hard=true
(( $(sql -qAt -c 'SHOW server_version_num') / 10000 == 18 )) || hard="p !~ '^(plan|ratio):'"
[[ $LANE == stock ]] || hard="p !~ '^(plan|ratio):'"
sql -q -P footer=off <<-SQL
	SELECT name AS shape, chosen, plan AS expected, round(ratio::numeric, 3) AS cost_ratio, concat_ws(' ', lo, hi) AS band,
		   round(native_ms::numeric, 1) AS native_ms, round(scan_ms::numeric, 1) AS scan_ms, verdict,
		   CASE WHEN EXISTS (SELECT 1 FROM unnest(problems) p WHERE $hard) THEN 'FAIL' END AS check
	FROM plangate.results ORDER BY name
SQL
sql -qAt <<-SQL
	SELECT CASE WHEN $hard THEN 'FAIL ' ELSE 'WARN ' END || name || ': ' || p FROM plangate.results, unnest(problems) p ORDER BY name LIMIT 40;
	SELECT format('plangate: %s shapes, %s on the scan, %s wins, %s ties, %s loses, %s missed wins, %s failures',
		count(*), count(*) FILTER (WHERE chosen = 'scan'), count(*) FILTER (WHERE verdict = 'win'), count(*) FILTER (WHERE verdict = 'tie'),
		count(*) FILTER (WHERE verdict = 'lose'), count(*) FILTER (WHERE verdict = 'miss'),
		count(*) FILTER (WHERE EXISTS (SELECT 1 FROM unnest(problems) p WHERE $hard)))
	FROM plangate.results;
SQL
bad=$(sql -qAt -c "SELECT count(*) FILTER (WHERE EXISTS (SELECT 1 FROM unnest(problems) p WHERE $hard)) + CASE WHEN ${WARN:-0} = 1 THEN 0 ELSE count(*) FILTER (WHERE verdict = 'lose') END FROM plangate.results")
total=$(sql -qAt -c "SELECT count(*) FROM plangate.results")
(( total > 0 )) || { log "FAIL: no shape ran"; exit 1; }
if [[ -n $WARN ]]; then sql -qAt -c "SELECT 'WARN ' || name || ': the scan is slower than native (' || round(scan_ms::numeric, 1) || ' ms against ' || round(native_ms::numeric, 1) || ')' FROM plangate.results WHERE verdict = 'lose' ORDER BY name"; fi
(( bad == 0 )) || exit 1
