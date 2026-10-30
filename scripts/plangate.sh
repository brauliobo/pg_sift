#!/usr/bin/env bash
# The plan-choice gate of the aggregate pushdown (docs/AGG.md, "Plan-choice gate"): the shapes of test/plan/shapes.sql run on the tables of
# test/plan/gen.sql, each three ways (native: sift.enable_agg off; chosen: the planner's own choice; scan: Custom Scan (SiftAgg) forced) by
# test/plan/run.sql, which compares the results, the plan chosen against the expectation of the shape and the cost ratio against its band.
#
#   scripts/plangate.sh [--rows N] [--seed N] [--runs N] [--shapes REGEX] [--lane stock|tuned] [--table] [--warn-time] [--update] [--keep]
#   scripts/plangate.sh --calibrate [--rows N] [--lane stock|tuned] [--shapes REGEX]   the ranking of the estimates against the instructions (below)
#   scripts/plangate.sh --planning [--rows N] [--shapes REGEX] [--update]   the instructions to plan each shape, scan on and off, against test/plan/planning.tsv (below)
#
#   --rows N      events (default 30000, the size the expectations are recorded at; they hold from 20000 to 50000), --seed N the generator seed (default 1)
#   --runs N      median of N warm runs of the native and the scan path (default 3; 0: no timings)
#   --shapes RE   only the shapes whose name matches
#   --lane tuned   the planner settings of a tuned profile (seq_page_cost 100, random_page_cost 4, workers 8, hash_mem_multiplier 2, work_mem 4GB): the results must agree and a
#                 lose still fails, but the plan and the cost band are those of the lane in test/plan/bands.tsv, so a different plan or a ratio outside its band fails on PostgreSQL 18, as in the stock lane
#   --table       the tables as sift_table (test/plan/table.sql): the results must agree as ever; the plan and the cost band are those of the lane in
#                 test/plan/bands.tsv (table, tabletuned with --lane tuned), so a different plan or a ratio outside its band fails on PostgreSQL 18, as in the stock lane
#   --warn-time   a shape that the scan makes slower than native by more than 1 ms and 20% (verdict lose) is a warning, not a failure: hosted runners are noisy
#   --update      rewrites the `-- expect:` line of the shapes of shapes.sql from this run (their bands; the plan too where it is `new`): read the diff. In another
#                 lane (--lane tuned, --table) it rewrites the rows of that lane in test/plan/bands.tsv (the plan the lane chooses and the band around its ratio)
#   --keep        keeps the database sift_plangate
# --calibrate  instead of the shapes: the shapes of test/plan/calibrate.tsv (every shape of shapes.sql, in a class: point, range, order and limit, count, group, join, aggregate, other; known: a wrong choice that is open work, reported and not failing)
#               are planned each way the planner can go (the scan, a sequential scan, a b-tree index, a bitmap scan, PostgreSQL without sift, the planner's own choice: EXPLAIN
#               only), and each distinct plan runs under `perf stat` (user instructions of the backend, planning taken off, serial plans). Prints the table; fails when the
#               plan the planner chooses is slower than the best by more than 20% and 1,000,000 instructions. Seconds, needs perf (skipped, exit 0, without it). docs/AGG.md
# --planning  the user instructions of EXPLAIN of each shape of calibrate.tsv with the scan on and with it off (the library loaded), warm, under `perf stat`; a shape fails at 30% and 20,000
#               instructions over its ceiling in test/plan/planning.tsv, the sum of the shapes at 10%; --update writes the ceilings from this run. Skipped, exit 0, without perf
# Fails (exit 1) on a result that differs between the paths, a plan other than the expected one, a cost ratio outside its band and (without
# --warn-time) a lose. Needs the scripts/pgtest.sh cluster installed and started (SIFT_TMP, SIFT_PORT).
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
export SIFT_TMP=${SIFT_TMP:-/tmp/sift-arch}
ROWS=30000 SEED=1 RUNS=3 ONLY= WARN= UPDATE= KEEP= LANE=stock TABLE= CAL= PLAN=
DB=${PLANGATE_DB:-sift_plangate}

while (( $# )); do
	case $1 in
		--warn-time) WARN=1 ;; --calibrate) CAL=1 ;; --planning) CAL=1 PLAN=1 ;; --update) UPDATE=1 ;; --keep) KEEP=1 ;; --table) TABLE=1 ;;
		--rows) ROWS=$2; shift ;; --seed) SEED=$2; shift ;; --runs) RUNS=$2; shift ;; --shapes) ONLY=$2; shift ;; --lane) LANE=$2; shift ;;
		*) sed -n 2,27p "$0"; exit 1 ;;
	esac
	shift
done

sql() { PGOPTIONS='-c client_min_messages=warning' SIFT_DB=${DBNOW:-$DB} "$REPO/scripts/pgtest.sh" psql -v ON_ERROR_STOP=1 "$@"; }
log() { printf '[plangate %s] %s\n' "$(date +%T)" "$*" >&2; }
[[ -n $KEEP ]] || trap 'DBNOW=postgres sql -qAt -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null 2>&1 || true' EXIT

# one psql session whose backend perf counts (globals pid, pf, PSQ); instr NAME VARIANT PLAN_ONLY prints the user instructions of three runs of a shape under the settings of
# a way (plangate.cal_set); returns 1, and says so, without perf
perf_session() {
	perf stat -e instructions:u -x, true 2>&1 | grep -q 'instructions' || { log "$1: perf stat of user instructions is not available, skipped"; return 1; }
	pf=$(mktemp)
	coproc PSQ { sql -qAtX; }
	echo "SELECT pg_backend_pid();" >&"${PSQ[1]}"
	read -r -u "${PSQ[0]}" pid
}
wait_for() { while IFS= read -r -u "${PSQ[0]}" x; do [[ $x == "$1" ]] && break; done; }
instr() {
	printf "SELECT plangate.cal_set('%s');\nSELECT 'ready';\n" "$2" >&"${PSQ[1]}"; wait_for ready
	perf stat -x, -e instructions:u -p "$pid" -o "$pf" & p=$!
	sleep 0.1
	printf "SELECT plangate.cal_run('%s', 3, %s);\nSELECT 'done';\n" "$1" "$3" >&"${PSQ[1]}"; wait_for done
	kill -INT "$p"; wait "$p" 2>/dev/null || true
	awk -F, '/instructions/ { print $1 }' "$pf"
}
perf_close() { echo "\\q" >&"${PSQ[1]}"; wait "$PSQ_PID" 2>/dev/null || true; rm -f "$pf"; }
cal_load() {
	sql -q <<-SQL >/dev/null
		\\set text \`cat '$REPO/test/plan/shapes.sql'\`
		\\set tsv \`grep -E '${ONLY:-.}' '$REPO/test/plan/calibrate.tsv'\`
		SELECT plangate.cal_load(:'text', :'tsv');
	SQL
}

# --planning: the planning of each shape, scan on against the scan off (the library loaded), in thousands of user instructions of one EXPLAIN, against the ceilings of
# test/plan/planning.tsv (--update rewrites them): a shape fails at 30% and 20,000 instructions over its ceiling, the sum at 10%
planning() {
	local name on off f=$REPO/test/plan/planning.tsv out
	perf_session "plangate --planning" || return 0
	cal_load
	out=$(mktemp)
	while read -r name; do
		printf "SELECT plangate.cal_set('chosen');\nSELECT plangate.cal_run('%s', 1, true);\nSELECT 'ready';\n" "$name" >&"${PSQ[1]}"; wait_for ready
		on=$(instr "$name" chosen true); off=$(instr "$name" native true)
		printf '%s\t%s\t%s\n' "$name" $(( on / 3000 )) $(( off / 3000 )) >>"$out"
	done < <(sql -qAt -c "SELECT name FROM plangate.cal_shapes ORDER BY name")
	perf_close
	if [[ -n $UPDATE ]]; then
		{ echo "# name<TAB>ceiling: thousands of user instructions of one EXPLAIN with the scan on (scripts/plangate.sh --planning --update)"; cut -f1,2 "$out"; } >"$f"
		log "updated test/plan/planning.tsv"
	fi
	awk -F'\t' 'NR == FNR { if ($0 !~ /^#/) base[$1] = $2; next }
		{ n++; on += $2; off += $3; b = ($1 in base) ? base[$1] : -1; sum += (b > 0 ? b : 0)
		  printf "%-34s %8d %8d %6.2f %8s\n", $1, $3, $2, $2 / ($3 ? $3 : 1), b >= 0 ? b : "-"
		  if (b < 0) { print "FAIL " $1 ": no ceiling in test/plan/planning.tsv (--update)"; fail = 1 }
		  else if ($2 > 1.3 * b + 20) { print "FAIL " $1 ": " $2 " k instructions to plan, ceiling " b; fail = 1 } }
		END { printf "plangate --planning: %d shapes, %d k instructions to plan with the scan on against %d k off (%.2f), ceilings %d k\n", n, on, off, on / (off ? off : 1), sum
		      if (n == 0) { print "FAIL no shape ran"; fail = 1 }
		      if (on > 1.1 * sum) { print "FAIL the planning of the shapes together: " on " k instructions, ceilings " sum " k"; fail = 1 }
		      exit fail }' "$f" "$out"
	local rc=$?
	rm -f "$out"
	# the ceilings are the instructions of PostgreSQL 18: on another major a shape over its ceiling is a warning
	(( $(sql -qAt -c 'SHOW server_version_num') / 10000 == 18 )) || { (( rc == 0 )) || log "WARN: ceilings are those of PostgreSQL 18"; return 0; }
	return $rc
}

# --calibrate: the estimates of each way of a shape against the instructions it takes (test/plan/calibrate.sql)
calibrate() {
	local name sig variant x p
	perf_session "plangate --calibrate" || return 0
	cal_load
	sql -qAt -c "SELECT plangate.cal_estimate()" >/dev/null
	local rows=""
	while IFS=$'\t' read -r name sig variant; do
		printf "SELECT plangate.cal_set('%s');\nSELECT plangate.cal_run('%s', 1, false);\nSELECT 'ready';\n" "$variant" "$name" >&"${PSQ[1]}"; wait_for ready
		x=$(instr "$name" "$variant" false); p=$(instr "$name" "$variant" true)
		rows+="('$name', '$sig', $(( (x - p) / 3 ))),"
	done < <(sql -qAt -F $'\t' -c "SELECT name, sig, min(variant) FROM plangate.cal_plans GROUP BY name, sig ORDER BY name, sig")
	perf_close
	sql -qAt -c "INSERT INTO plangate.cal_measured VALUES ${rows%,}" >/dev/null
	sql -q -P footer=off -c "SELECT class, name, variants, CASE WHEN chosen THEN 'chosen' END AS pick, cost, rank_est, m_instr AS minstr, rank_meas, CASE WHEN wrong THEN 'WRONG' END AS check
		FROM plangate.cal_verdicts ORDER BY class, name, rank_est"
	sql -q -P footer=off -c "SELECT class, name, estimated_cheaper, estimated_dearer, cost_a, cost_b, m_a AS minstr_a, m_b AS minstr_b, CASE WHEN sift THEN 'sift' END AS check FROM plangate.cal_pairs ORDER BY class, name, cost_a" | sed '1s/^/mis-ranked pairs (the dearer by estimate takes 20% and 1M instructions fewer):\n/'
	sql -qAt -c "SELECT format('plangate --calibrate: %s of %s pairs of plans with a scan ranked the wrong way round, in %s of %s shapes', (SELECT count(*) FROM plangate.cal_pairs WHERE sift), (SELECT count(*) FROM plangate.cal_verdicts a JOIN plangate.cal_verdicts b ON a.name = b.name AND NOT a.chosen AND NOT b.chosen AND a.cost < b.cost AND (a.sift OR b.sift)), (SELECT count(DISTINCT name) FROM plangate.cal_pairs WHERE sift), (SELECT count(*) FROM plangate.cal_shapes))"
	sql -qAt -c "SELECT format('plangate --calibrate: %s shapes, %s plans, %s chosen plans slower than the best by over 20%% and 1M instructions', count(DISTINCT name), count(*), count(*) FILTER (WHERE wrong)) FROM plangate.cal_verdicts"
	sql -qAt -c "SELECT 'WRONG ' || name || ' (' || class || '): the planner chooses ' || variants || ' at ' || m_instr || 'M instructions, the best is ' || best_m_instr || 'M' FROM plangate.cal_verdicts WHERE wrong ORDER BY class, name"
	(( $(sql -qAt -c "SELECT count(*) FROM plangate.cal_verdicts WHERE wrong AND class <> 'known'") == 0 ))
}

DBNOW=postgres sql -qAt -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" -c "CREATE DATABASE $DB"
# serial plans, no JIT and the stock planner costs: the bands are ratios of the costs the server ships with, whatever its configuration
settings=(max_parallel_workers_per_gather=0 jit=off seq_page_cost=1 random_page_cost=4 cpu_tuple_cost=0.01 effective_cache_size=4GB work_mem=4MB)
case $LANE in
	stock) ;;
	tuned) settings=(max_parallel_workers_per_gather=8 jit=off seq_page_cost=100 random_page_cost=4 cpu_tuple_cost=0.01 effective_cache_size=4GB work_mem=4GB hash_mem_multiplier=2) ;;
	*) log "unknown lane $LANE (stock or tuned)"; exit 1 ;;
esac
# the lane of the bands of test/plan/bands.tsv: none for stock on the heap, else table, tuned or tabletuned
LK=${TABLE:+table}$([[ $LANE != tuned ]] || echo tuned)
[[ -z $LK ]] || settings+=("plangate.lane=$LK")
[[ -z $CAL ]] || settings+=(max_parallel_workers_per_gather=0)
for s in "${settings[@]}"; do printf "ALTER DATABASE %s SET %s TO '%s';\n" "$DB" "${s%%=*}" "${s#*=}"; done | sql -q >/dev/null
sql -qAt -c "CREATE EXTENSION pg_sift" >/dev/null
log "tables: $ROWS events, seed $SEED"
sql -q -v n="$ROWS" -v seed="$SEED" -f "$REPO/test/col/run.sql" -f "$REPO/test/plan/gen.sql" ${TABLE:+-f "$REPO/test/plan/table.sql"} -f "$REPO/test/plan/run.sql" ${CAL:+-f "$REPO/test/plan/calibrate.sql"} >/dev/null
[[ -z $CAL ]] || { if [[ -n $PLAN ]]; then planning; else calibrate; fi; exit $?; }
log "shapes"
sql -q <<-SQL >/dev/null
	\\copy plangate.bands FROM '$REPO/test/plan/bands.tsv'
SQL
sql -q -v runs="$RUNS" -v pattern="$ONLY" >/dev/null <<-SQL
	\\set text \`cat '$REPO/test/plan/shapes.sql'\`
	SELECT plangate.run_all(:'text', :runs, :'pattern');
SQL

if [[ -n $UPDATE && -n $LK ]]; then
	# the plan this lane chooses (either stays) and the band of a factor of 2 around its ratio, in 2 significant digits
	sql -qAt -F $'\t' -c "SELECT '$LK', name, CASE WHEN plan = 'either' THEN plan ELSE chosen END,
		coalesce(round(ratio::numeric / 2, 1 - floor(log(ratio::numeric / 2))::int)::text, '-'), coalesce(round(ratio::numeric * 2, 1 - floor(log(ratio::numeric * 2))::int)::text, '-')
		FROM plangate.results" >"${SIFT_TMP}/plangate-bands.tsv"
	{ awk -F'\t' 'NR == FNR { new[$1 FS $2] = 1; next } !(($1 FS $2) in new)' "${SIFT_TMP}/plangate-bands.tsv" "$REPO/test/plan/bands.tsv"; cat "${SIFT_TMP}/plangate-bands.tsv"; } | LC_ALL=C sort -t $'\t' -k1,1 -k2,2 >"${SIFT_TMP}/plangate-all.tsv"
	cat "${SIFT_TMP}/plangate-all.tsv" >"$REPO/test/plan/bands.tsv"
	rm -f "${SIFT_TMP}/plangate-bands.tsv" "${SIFT_TMP}/plangate-all.tsv"
	log "updated the $LK rows of test/plan/bands.tsv"
elif [[ -n $UPDATE ]]; then
	sql -qAt -F $'\t' -c "SELECT name, expect FROM plangate.suggestions" >"${SIFT_TMP}/plangate-expect.tsv"
	awk -F'\t' 'NR == FNR { e[$1] = $2; next }
		/^-- name: / { n = substr($0, 10) }
		/^-- expect: / && (n in e) { print "-- expect: " e[n]; next }
		{ print }' "${SIFT_TMP}/plangate-expect.tsv" "$REPO/test/plan/shapes.sql" >"${SIFT_TMP}/plangate-shapes.sql"
	cat "${SIFT_TMP}/plangate-shapes.sql" >"$REPO/test/plan/shapes.sql"
	rm -f "${SIFT_TMP}/plangate-expect.tsv" "${SIFT_TMP}/plangate-shapes.sql"
	log "updated test/plan/shapes.sql"
fi

# the expected plans and cost bands, of every lane, are the costs of PostgreSQL 18: on another major their drift is a warning, a wrong result still fails
hard=true
(( $(sql -qAt -c 'SHOW server_version_num') / 10000 == 18 )) || hard="p !~ '^(plan|ratio):'"
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
