#!/usr/bin/env bash
# Differential test of the aggregate pushdown (docs/AGG.md, "Differential suite"): every query of a corpus runs in one session with
# sift.enable_agg off (the reference: the plan PostgreSQL has without sift) and in variants (parallel workers 0 and 2, sift.simd scalar
# and auto), the results are compared in the database (test/col/run.sql), and the plans are read to report which queries used
# Custom Scan (SiftAgg).
#
#   scripts/coldiff.sh [--suite fixtures|analytics|all] [--corpus REGEX] [--rows N] [--bench] [--strict-shape] [--keep]
#   scripts/coldiff.sh --selftest
#
#   --suite S           fixtures: the small orders table of test/col/fixture.sql, the corpus test/col/fixtures/*.sql and the scenarios
#                       (the DML heap states); analytics: the corpus of bench/sql/analytics (SIFT_ANALYTICS_DIR, queries --queries GLOB)
#                       on its generator (--rows orders, default 20000, --stores N); all (default) runs both
#   --corpus REGEX      only the queries whose file name matches (name@K400 is the query on the set K400)
#   --variants LIST     comma separated: off-par on on-par on-scalar on-par-scalar (the reference "off" always runs; default all five)
#   --scenarios LIST    fixtures: insert update delete abort vacuum hot recycle held maint, or all (default) or none; dml (the inserts, updates and
#                       deletes of the first three in one state, one check) and held-lite (held without its repeatable read twin and the check after the writer committed: the dml and vacuum states cover them) are named only
#   --bench [--runs N]  only the corpus, with the median of N runs (default 5) of the reference and of "on"; prints the go/no-go lines:
#                       the K4000 queries of the shapes that must push down at least 20 times faster, K10 and K400 not slower (0.95)
#   --bench-workers N   the "on" runs of --bench use max_parallel_workers_per_gather N with the real parallel costs (default 0; the reference stays serial)
#                       A query whose plan does not meet its `-- expect:` tag (agg: SiftAgg; plain: no SiftAgg) fails.
#   --strict-shape      a bench target missed fails too (default: reported)
#   --rel X --abs X     tolerance of float columns: |x - y| <= abs + rel * max(|x|, |y|) (defaults 1e-9 and 1e-12)
#   --no-index          without the column index: plain against plain (the self-check of the harness); without the flag a missing
#                       access method or a failing CREATE INDEX skips the suite with a message
#   --col-am NAME       the access method (default: sift_col, else sift_colstub when only the stub exists)
#   --col-paths SPEC    the `paths` reloption of the fixture index; --col-index FILE the DDL of the analytics index (default
#                       test/col/col_index.sql, USING sift_col replaced by --col-am)
#   --scenario-slices N each scenario runs the scenario queries of one slice of N (query i is in slice i mod N), the slice moving from one scenario to
#                       the next, so that every query meets a heap state and every state meets queries of every kind (default 1: all of them everywhere)
#   --fixture-rows N    rows of the fixture table (default 4000), --seed N its generator seed; above 4000 the t_* tables grow with it
#   --default-costs     the fixtures run at the server's costs: the page costs of a fixture that expects the scan are not raised (they are on the
#                       default tiny table, where the btree plan looks cheap too); with --fixture-rows 300000 the plan choice is the planner's own
#   --mutate KIND       perturbs the "on" results (int float float-tiny text null rows dup order type cols), to prove a failure is seen
#   --selftest          runs the fixture corpus plain against plain (must pass), then every mutation (must fail, float-tiny pass)
#   --keep              keeps the databases (sift_coldiff_fix, sift_coldiff_ana) and the scripts in $SIFT_TMP/coldiff
# Needs the scripts/pgtest.sh cluster installed and started (SIFT_TMP, SIFT_PORT). Exit 1 on a failure: a result that differs, a
# reference run that fails, a query without a result (and, with --strict-shape, a shape expectation that does not hold).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
export SIFT_TMP=${SIFT_TMP:-/tmp/sift-arch}
OUT=$SIFT_TMP/coldiff
SUITE=all CORPUS= ROWS=20000 STORES= BENCH= BENCH_WORKERS=0 RUNS=5 STRICT= KEEP= NOINDEX= MUTATE= SELFTEST= SEED=0 FIXROWS=4000 DEFAULT_COSTS=0 REL=1e-9 ABS=1e-12
VARIANTS=off-par,on,on-par,on-scalar,on-par-scalar SCENARIOS=all SLICES=1 COL_AM= COL_INDEX= VERBOSE=
COL_PATHS='attributes->channel text, attributes->normalized bool, attributes->weight float8, attributes->tier text, attributes->code astext, metrics.* float8, items[]{category text, sku text, kind text, qty int4, price float8, discount float8}, notes[]{type text, author_kind text}'
ANALYTICS_DIR=${SIFT_ANALYTICS_DIR:-$REPO/bench/sql/analytics}
QUERIES=
DB_PREFIX=${COLDIFF_DB:-sift_coldiff}

while (( $# )); do
	case $1 in
		--bench) BENCH=1 ;; --bench-workers) BENCH_WORKERS=$2; shift ;; --strict-shape) STRICT=1 ;; --keep) KEEP=1 ;; --no-index) NOINDEX=1 ;; --selftest) SELFTEST=1 ;; -v|--verbose) VERBOSE=1 ;;
		--suite) SUITE=$2; shift ;; --corpus) CORPUS=$2; shift ;; --rows) ROWS=$2; shift ;; --stores) STORES=$2; shift ;; --runs) RUNS=$2; shift ;;
		--variants) VARIANTS=$2; shift ;; --scenarios) SCENARIOS=$2; shift ;; --rel) REL=$2; shift ;; --abs) ABS=$2; shift ;;
		--col-am) COL_AM=$2; shift ;; --col-paths) COL_PATHS=$2; shift ;; --col-index) COL_INDEX=$2; shift ;; --mutate) MUTATE=$2; shift ;;
		--fixture-rows) FIXROWS=$2; shift ;; --scenario-slices) SLICES=$2; shift ;; --default-costs) DEFAULT_COSTS=1 ;; --seed) SEED=$2; shift ;; --analytics-dir) ANALYTICS_DIR=$2; shift ;; --queries) QUERIES=$2; shift ;;
		*) sed -n '2,40p' "$0"; exit 1 ;;
	esac
	shift
done
[[ $SUITE == fixtures || $SUITE == analytics || $SUITE == all ]] || { sed -n '2,40p' "$0"; exit 1; }
[[ -n $STORES ]] || STORES=$(( ROWS / 30 > 100 ? ROWS / 30 : 100 ))
QUERIES=${QUERIES:-$ANALYTICS_DIR/queries/*.sql}
# a variant list as a PostgreSQL array literal: the reference always runs
VARIANT_ARRAY="{${VARIANTS}}"

sql() { PGOPTIONS='-c client_min_messages=warning' SIFT_DB=$DB "$REPO/scripts/pgtest.sh" psql -v ON_ERROR_STOP=1 -qAt "$@"; }
table() { PGOPTIONS='-c client_min_messages=warning' SIFT_DB=$DB "$REPO/scripts/pgtest.sh" psql -v ON_ERROR_STOP=1 -q -P footer=off "$@"; }
log() { printf '[coldiff %s] %s\n' "$(date +%T)" "$*" >&2; }
skip() { printf 'coldiff: SKIP: %s\n' "$*"; }
FAILED=0
DB=postgres
DBS=()

mkdir -p "$OUT"
cleanup() {
	[[ -n $KEEP ]] && { log "kept: ${DBS[*]-} (scripts in $OUT)"; return; }
	for DB in ${DBS[@]+"${DBS[@]}"}; do DB=postgres sql -c "DROP DATABASE IF EXISTS $DB WITH (FORCE)" >/dev/null 2>&1 || true; done
}
trap cleanup EXIT

# fresh_db NAME: an empty database with sift, the test-only extension (the stub of the access method lives there) and the runner
fresh_db() {
	DB=postgres sql -c "DROP DATABASE IF EXISTS $1 WITH (FORCE)" -c "CREATE DATABASE $1"
	DBS+=("$1")
	DB=$1
	sql -c "CREATE EXTENSION pg_sift"
	sql -c "CREATE EXTENSION IF NOT EXISTS pg_sift_test" 2>/dev/null || true
	sql -f "$REPO/test/col/run.sql"
}

# the access method of the column index: --col-am, else sift_col, else sift_colstub; empty when none exists (or --no-index)
resolve_am() {
	[[ -z $NOINDEX ]] || return 0
	if [[ -n $COL_AM ]]; then AM=$COL_AM; else
		AM=$(sql -c "SELECT amname FROM pg_am WHERE amname IN ('sift_col', 'sift_colstub') ORDER BY amname = 'sift_col' DESC LIMIT 1")
	fi
	[[ -n $AM && $(sql -c "SELECT count(*) FROM pg_am WHERE amname = '$AM'") == 1 ]] || { AM=; skip "no column index access method (sift_col, sift_colstub): run with --no-index for the plain-against-plain self-check"; return 1; }
}

# ---------- corpus ----------
# files SUITE: the corpus files of the suite whose name matches --corpus, one per line
files() {
	local f
	for f in "$@"; do [[ -z $CORPUS || $(basename "$f" .sql) =~ $CORPUS ]] && echo "$f"; done
}

# run_corpus SCEN FILES...: the files in one psql session of the database $DB, each in a transaction of its own (isolation of its tag);
# HELD=1: all in one REPEATABLE READ transaction whose snapshot is taken first (SNAP_SYNC=1: then wait for $OUT/snap.go, after
# announcing the snapshot in $OUT/snap.taken); a file with an isolation tag does not run held. Results go to coldiff.results.
run_corpus() {
	local scen=$1 f name names=()
	local script=$OUT/$scen.psql
	shift
	{
		echo "\\set ON_ERROR_STOP 0"
		echo "LOAD 'pg_sift';"
		echo "SET coldiff.bench_workers = $BENCH_WORKERS;"
		echo "SET coldiff.default_costs = $DEFAULT_COSTS;"
		echo "SET coldiff.no_index = '${NOINDEX:+1}';"
		echo "\\o /dev/null"
		[[ -z ${HELD:-} ]] || {
			echo "BEGIN ISOLATION LEVEL REPEATABLE READ;"
			echo "SELECT count(*) FROM orders;"
			[[ -z ${SNAP_SYNC:-} ]] || { echo "\\! touch '$OUT/snap.taken'"; echo "\\! while [ ! -e '$OUT/snap.go' ]; do sleep 0.1; done"; }
		}
		for f in "$@"; do
			name=$(basename "$f" .sql)
			if [[ -n ${HELD:-} ]] && grep -q '^-- isolation:' "$f"; then continue; fi
			names+=("$name")
			echo "\\set f \`cat '$f'\`"
			if [[ -z ${HELD:-} ]]; then
				echo "SELECT coldiff.iso(:'f') AS iso \\gset"
				echo "BEGIN ISOLATION LEVEL :iso;"
			fi
			echo "SELECT coldiff.run_query(:'scen', '$name', :'f', :'variants'::text[], :runs, nullif(:'mutate', ''), :'rel'::float8, :'abs'::float8);"
			[[ -n ${HELD:-} ]] || echo "COMMIT;"
		done
		[[ -z ${HELD:-} ]] || echo "COMMIT;"
	} >"$script"
	rm -f "$OUT/snap.go" "$OUT/snap.taken"
	[[ -z ${VERBOSE:-} ]] || log "$scen: ${#names[@]} files ($script)"
	sql -v scen="$scen" -v variants="$VARIANT_ARRAY" -v runs="${RUNS_NOW:-1}" -v mutate="$MUTATE" -v rel="$REL" -v abs="$ABS" -f "$script" >/dev/null || true
	# a file without a result raised an error that the runner did not catch
	local missing
	missing=$(sql -v scen="$scen" -v names="{$(IFS=,; echo "${names[*]-}")}" <<-'SQL'
		SELECT string_agg(n, ' ') FROM unnest(:'names'::text[]) n
		WHERE NOT EXISTS (SELECT 1 FROM coldiff.results r WHERE r.scenario = :'scen' AND (r.name = n OR r.name LIKE n || '@%'))
	SQL
	)
	if [[ -n $missing ]]; then FAILED=1; log "FAIL $scen: no result for: $missing"; fi
}

# failures SCEN: the number of results that fail (and the shape expectations that are not met, with --strict-shape)
failures() {
	sql -v scen="$1" <<-'SQL'
		SELECT count(*) FROM coldiff.results WHERE scenario = :'scen' AND (NOT ok OR shape_ok = false)
	SQL
}

# report SCEN [full]: the coverage table (full; shape notes are its last column), the failures with the rows that differ and one summary line
report() {
	local scen=$1
	if [[ ${2:-} == full ]]; then
		table -v scen="$scen" <<-'SQL'
			SELECT name AS query, coalesce(max(shape), '') AS shape, max(expect) AS expect,
				   CASE WHEN count(*) FILTER (WHERE variant = 'skip') > 0 THEN 'skip' WHEN bool_and(ok) THEN 'equal' ELSE 'DIFF' END AS result,
				   CASE WHEN count(*) FILTER (WHERE variant = 'on') = 0 THEN '-' WHEN bool_or(used) FILTER (WHERE variant = 'on') THEN 'yes' ELSE 'no' END AS siftagg,
				   round(max(ref_ms)::numeric, 1) AS off_ms, round(max(ms) FILTER (WHERE variant = 'on')::numeric, 1) AS on_ms,
				   round((max(ref_ms) / nullif(max(ms) FILTER (WHERE variant = 'on'), 0))::numeric, 1) AS speedup,
				   CASE WHEN bool_or(shape_ok = false) THEN 'FAIL: expect ' || max(expect) ELSE '' END AS shape_note
			FROM coldiff.results WHERE scenario = :'scen' GROUP BY name ORDER BY name
		SQL
	fi
	(( $(sql -v scen="$scen" <<<"SELECT count(*) FROM coldiff.results WHERE scenario = :'scen' AND NOT ok") == 0 )) || table -v scen="$scen" <<-'SQL'
		SELECT name AS "FAIL", variant, detail FROM coldiff.results WHERE scenario = :'scen' AND NOT ok ORDER BY id LIMIT 40
	SQL
	(( $(sql -v scen="$scen" <<<"SELECT count(*) FROM coldiff.results WHERE scenario = :'scen' AND shape_ok = false") == 0 )) || table -v scen="$scen" <<-'SQL'
		SELECT name AS "FAIL (plan against its expect tag)", variant, expect, CASE WHEN used THEN 'SiftAgg' ELSE 'plain' END AS plan FROM coldiff.results
		WHERE scenario = :'scen' AND shape_ok = false ORDER BY id LIMIT 40
	SQL
	sql -v scen="$scen" <<-'SQL'
		SELECT format('coldiff[%s]: %s queries, %s runs: %s equal, %s DIFFER, %s requires unmet; SiftAgg used by %s of %s; plans against their expect tag %s',
			:'scen', count(DISTINCT name), count(*) FILTER (WHERE variant <> 'skip'), count(*) FILTER (WHERE ok AND variant <> 'skip'), count(*) FILTER (WHERE NOT ok),
			count(DISTINCT name) FILTER (WHERE variant = 'skip'),
			count(DISTINCT name) FILTER (WHERE variant = 'on' AND used), count(DISTINCT name) FILTER (WHERE variant = 'on'),
			count(DISTINCT name) FILTER (WHERE shape_ok = false))
		FROM coldiff.results WHERE scenario = :'scen'
	SQL
	(( $(failures "$scen") == 0 )) || FAILED=1
}

# the go/no-go lines of --bench: K4000 queries that must push down: 20x; K10 and K400: no slowdown
bench_verdict() {
	table -v scen="$1" <<-'SQL'
		SELECT name AS query, CASE WHEN used THEN 'SiftAgg' ELSE 'plain' END AS plan, round(ref_ms::numeric, 1) AS off_ms, round(ms::numeric, 1) AS on_ms,
			   round((ref_ms / nullif(ms, 0))::numeric, 1) AS speedup, coldiff.bench_target(name, expect, used, ref_ms, ms) AS target
		FROM coldiff.results WHERE scenario = :'scen' AND variant = 'on' ORDER BY name
	SQL
	local missed
	missed=$(sql -v scen="$1" <<-'SQL'
		SELECT count(*) FROM coldiff.results WHERE scenario = :'scen' AND variant = 'on' AND coldiff.bench_target(name, expect, used, ref_ms, ms) ~ '^(MISSED|NOT|REGRESSION)'
	SQL
	)
	(( missed == 0 )) || { log "bench: $missed targets missed${STRICT:+ (strict: failing)}"; [[ -z $STRICT ]] || FAILED=1; }
}

# check SCEN FILES...: one corpus run and its report (the coverage table for the main run of a suite)
check() {
	local scen=$1 full=${REPORT_FULL:-}; shift
	if [[ -n $BENCH ]]; then RUNS_NOW=$RUNS run_corpus "$scen" "$@"; else run_corpus "$scen" "$@"; fi
	report "$scen" "$full"
	[[ -z $BENCH ]] || bench_verdict "$scen"
}

# ---------- fixtures ----------
fixture_indexes() {
	sql -c "ALTER TABLE orders ADD PRIMARY KEY (id)" \
		-c "CREATE INDEX orders_store_id_placed_at_index ON orders USING btree (tenant_id, placed_at, deleted_at)" \
		-c "CREATE INDEX orders_items_gin_index ON orders USING gin (items jsonb_path_ops)" \
		-c "CREATE INDEX orders_fts ON orders USING sift (memo)"
}

# col_index TABLE: the column index of a fixture table; the first one decides whether the suite runs
col_index() {
	local ddl
	case $1 in
		orders|orders_odd|orders_part)
			ddl="CREATE INDEX ${1}_col ON $1 USING $AM (id, tenant_id, customer_id, source, placed_at, country_code, lang, status, total, discount, deleted_at, attributes, metrics, items, notes) WITH (paths = :'paths')" ;;
		t_int) ddl="CREATE INDEX t_int_col ON t_int USING $AM (id, grp, i4, i8)" ;;
		t_num) ddl="CREATE INDEX t_num_col ON t_num USING $AM (id, grp, n2, n0, n6)" ;;
		t_nan) ddl="CREATE INDEX t_nan_col ON t_nan USING $AM (id, grp, n2, n6)" ;;
		t_float) ddl="CREATE INDEX t_float_col ON t_float USING $AM (id, grp, f4, f8)" ;;
		t_dist) ddl="CREATE INDEX t_dist_col ON t_dist USING $AM (id, grp, v, w, x)" ;;
		t_wide) ddl="CREATE INDEX t_wide_col ON t_wide USING $AM (id, grp, qty, n38, n19, nimg, nneg, nhuge, m, iv)" ;;
		t_misc) ddl="CREATE INDEX t_misc_col ON t_misc USING $AM (id, grp, qty, t, e, u, b, ia, ta, na, r, tr, mr)" ;;
		t_net) ddl="CREATE INDEX t_net_col ON t_net USING $AM (id, grp, qty, ip, cd, mac, mac8, tz, b, vb)" ;;
		t_text) ddl="CREATE INDEX t_text_col ON t_text USING $AM (id, grp, t, vc)" ;;
		t_jp) ddl="CREATE INDEX t_jp_col ON t_jp USING $AM (id, grp, doc, mdoc) WITH (paths = 'doc[]{s text, n int4, b bool}, mdoc[]{s text, n int4, b bool}')" ;;
		t_tags) ddl="CREATE INDEX t_tags_col ON t_tags USING $AM (id, grp, tags) WITH (paths = 'tags[] text')" ;;
		t_part) ddl="CREATE INDEX t_part_col ON t_part USING $AM (id, grp, d, qty, amount, tag, doc, note) WITH (paths = 'doc->k int4, doc->s text')" ;;
		t_part_ni) ddl="CREATE INDEX t_part_ni_a_col ON t_part_ni_a USING $AM (id, grp, d, qty)" ;;
		t_part_op) ddl="CREATE INDEX t_part_op_a_col ON t_part_op_a USING $AM (id, grp, d, qty, doc) WITH (paths = 'doc->k int4');
					   CREATE INDEX t_part_op_b_col ON t_part_op_b USING $AM (id, grp, d, qty, doc) WITH (paths = 'doc->k int8')" ;;
		t_inh) ddl="CREATE INDEX t_inh_a_col ON t_inh_a USING $AM (id, grp, d, qty); CREATE INDEX t_inh_b_col ON t_inh_b USING $AM (id, grp, d, qty)" ;;
	esac
	printf '%s\n' "$ddl" | sql -v paths="$COL_PATHS"
}

fixture_col_indexes() {
	local err t
	if ! err=$(col_index orders 2>&1); then skip "CREATE INDEX USING $AM failed: ${err#*ERROR:  }"; return 1; fi
	for t in t_int t_float t_num t_nan t_text t_dist t_wide t_jp t_misc t_net t_tags orders_odd orders_part t_part t_part_ni t_part_op t_inh; do
		err=$(col_index $t 2>&1) || log "NOTE: no column index on $t, its queries run plain against plain: ${err#*ERROR:  }"
	done
}

scenario_files() { local f; for f in "$@"; do grep -q '^-- scenario: yes' "$f" && echo "$f"; done; }

# wait_for FILE: until a script of this harness announces itself (60 s)
wait_for() { local i; for (( i = 0; i < 600; i++ )); do [[ -e $1 ]] && return 0; sleep 0.1; done; log "FAIL: timeout waiting for $1"; FAILED=1; return 1; }

# the scenarios: DML into the heap states that the column index must still answer for, each compared on the scenario subset
scenarios() {
	local list=$SCENARIOS names f turn=0
	[[ $list != none ]] || return 0
	mapfile -t names < <(scenario_files "$@")
	want() { [[ $list == all || ,$list, == *,$1,* ]]; }
	named() { [[ ,$list, == *,$1,* ]]; }
	# slice: SLICED is the slice of the scenario queries this scenario runs (--scenario-slices; all of them by default)
	slice() { local i; SLICED=(); for i in "${!names[@]}"; do (( i % SLICES == turn % SLICES )) && SLICED+=("${names[i]}"); done; turn=$((turn + 1)); }
	if named dml; then
		log "scenario dml: 20% new orders, indexed updates, hard and soft deletes, no VACUUM"
		sql -c "SELECT coldiff.sc_insert(0.2)" -c "SELECT coldiff.sc_update_indexed(0.03)" -c "SELECT coldiff.sc_delete(0.03)" >/dev/null
		slice; check scenario-dml "${SLICED[@]}"
	fi
	if want insert; then log "scenario insert: 20% new orders, no VACUUM"; sql -c "SELECT coldiff.sc_insert(0.2)" >/dev/null; slice; check scenario-insert "${SLICED[@]}"; fi
	if want update; then log "scenario update: indexed columns and jsonb"; sql -c "SELECT coldiff.sc_update_indexed(0.03)" >/dev/null; slice; check scenario-update "${SLICED[@]}"; fi
	if want delete; then log "scenario delete: hard and soft deletes"; sql -c "SELECT coldiff.sc_delete(0.03)" >/dev/null; slice; check scenario-delete "${SLICED[@]}"; fi
	if want abort; then
		log "scenario abort: rolled-back insert, update and delete"
		sql >/dev/null <<-'SQL'
			BEGIN; SELECT coldiff.sc_insert(0.1); SELECT coldiff.sc_update_indexed(0.05); SELECT coldiff.sc_delete(0.05); ROLLBACK;
		SQL
		slice; check scenario-abort "${SLICED[@]}"
	fi
	if want vacuum; then log "scenario vacuum"; sql -c "VACUUM (ANALYZE) orders" -c "VACUUM (ANALYZE) t_part"; slice; check scenario-vacuum "${SLICED[@]}"; fi
	if want hot; then
		sql -c "SELECT coldiff.sc_hot(0.05)" >/dev/null
		log "scenario hot: HOT updates of columns no index holds: $(sql -c "SELECT n_tup_hot_upd FROM pg_stat_user_tables WHERE relname = 'orders'")"
		slice; check scenario-hot "${SLICED[@]}"
	fi
	if want recycle; then
		sql -c "DROP TABLE IF EXISTS coldiff_gone" -c "CREATE TABLE coldiff_gone AS SELECT ctid AS tid, id FROM orders WHERE coldiff.sample(id, 906, 0.05)" \
			-c "DELETE FROM orders WHERE id IN (SELECT id FROM coldiff_gone)" -c "DELETE FROM t_part WHERE id IN (SELECT id FROM coldiff_gone)" \
			-c "VACUUM (TRUNCATE off) orders" -c "VACUUM (TRUNCATE off) t_part" -c "SELECT coldiff.sc_insert(0.06)" >/dev/null
		log "scenario recycle: new orders on recycled TIDs: $(sql -c "SELECT count(*) FROM orders WHERE ctid = ANY (ARRAY(SELECT tid FROM coldiff_gone))")"
		slice; check scenario-recycle "${SLICED[@]}"
	fi
	if want held || named held-lite; then slice; scenario_held "${SLICED[@]}"; fi
	if want maint && [[ -n ${AM:-} ]]; then slice; scenario_maint "${SLICED[@]}"; fi
}

# a writer with an uncommitted insert, update and delete next to the corpus (read committed, then repeatable read), and a repeatable
# read snapshot taken before a writer commits
scenario_held() {
	local hold=$OUT/hold.psql hp
	log "scenario held: uncommitted writer"
	rm -f "$OUT"/hold.open "$OUT"/hold.release
	cat >"$hold" <<-SQL
		BEGIN;
		SELECT coldiff.sc_insert(0.1); SELECT coldiff.sc_update_indexed(0.05); SELECT coldiff.sc_delete(0.05);
		\\! touch '$OUT/hold.open'
		\\! while [ ! -e '$OUT/hold.release' ]; do sleep 0.1; done
		ROLLBACK;
	SQL
	sql -f "$hold" >/dev/null & hp=$!
	if wait_for "$OUT/hold.open"; then
		check scenario-held-uncommitted "$@"
		[[ $SCENARIOS == *held-lite* ]] || HELD=1 check scenario-held-uncommitted-rr "$@"
	fi
	touch "$OUT/hold.release"; wait "$hp" || FAILED=1
	log "scenario held: snapshot, then a writer commits"
	rm -f "$OUT"/snap.taken "$OUT"/snap.go
	HELD=1 SNAP_SYNC=1 check scenario-held-snapshot "$@" & hp=$!
	if wait_for "$OUT/snap.taken"; then sql -c "SELECT coldiff.sc_insert(0.1)" -c "SELECT coldiff.sc_update_indexed(0.05)" -c "SELECT coldiff.sc_delete(0.05)" >/dev/null; fi
	touch "$OUT/snap.go"; wait "$hp" || FAILED=1
	[[ $SCENARIOS == *held-lite* ]] || check scenario-held-after "$@"
}

# maintenance of the index: REINDEX, then promotion and merge when the functions exist
scenario_maint() {
	local fn
	log "scenario maint: REINDEX, promote, merge"
	sql -c "REINDEX INDEX orders_col" -c "REINDEX TABLE t_part"; check scenario-reindex "$@"
	for fn in col_promote col_merge; do
		if [[ $(sql -c "SELECT to_regproc('sift.$fn') IS NOT NULL") == t ]]; then
			sql -c "SELECT sift.$fn('orders_col'$([[ $fn == col_merge ]] && echo ', true'))" >/dev/null; check "scenario-$fn" "$@"
		else log "scenario maint: sift.$fn does not exist, skipped"; fi
	done
}

fixtures_suite() {
	local fl
	log "fixtures: $FIXROWS orders (seed $SEED)"
	fresh_db "${DB_PREFIX}_fix"
	[[ -n $NOINDEX ]] || resolve_am || return 0
	sql -v n="$FIXROWS" -v seed="$SEED" -f "$REPO/test/col/fixture.sql" >/dev/null
	fixture_indexes
	[[ -n $NOINDEX ]] || fixture_col_indexes || return 0
	sql -c "VACUUM (FREEZE, ANALYZE)" >/dev/null
	mapfile -t fl < <(files "$REPO"/test/col/fixtures/*.sql)
	(( ${#fl[@]} )) || { log "fixtures: no query matches --corpus"; return 0; }
	REPORT_FULL=full check fixtures "${fl[@]}"
	[[ -n $BENCH ]] || scenarios "${fl[@]}"
}

# ---------- analytics ----------
analytics_suite() {
	local fl col
	if [[ ! -f $ANALYTICS_DIR/gen.sql ]]; then skip "analytics: no data directory $ANALYTICS_DIR (SIFT_ANALYTICS_DIR)"; return 0; fi
	log "analytics: $ROWS orders in $STORES tenants ($ANALYTICS_DIR)"
	fresh_db "${DB_PREFIX}_ana"
	[[ -n $NOINDEX ]] || resolve_am || return 0
	sql -1 -v n="$ROWS" -v tenants="$STORES" -v order_by= -f "$ANALYTICS_DIR/schema.sql" -f "$ANALYTICS_DIR/gen.sql" >/dev/null
	sql -f "$ANALYTICS_DIR/sets.sql" -f "$ANALYTICS_DIR/indexes.sql" >/dev/null
	if [[ -z $NOINDEX ]]; then
		col=${COL_INDEX:-$REPO/test/col/col_index.sql}
		if ! err=$(sed "s/USING sift_col\b/USING $AM/" "$col" | grep -v '^--' | sql -f - 2>&1); then skip "analytics: CREATE INDEX USING $AM failed: ${err#*ERROR:  }"; return 0; fi
	fi
	sql -c "VACUUM (FREEZE, ANALYZE)" >/dev/null
	# shellcheck disable=SC2206
	mapfile -t fl < <(files $QUERIES)
	(( ${#fl[@]} )) || { log "analytics: no query matches"; return 0; }
	REPORT_FULL=full check analytics "${fl[@]}"
}

# ---------- self-test ----------
# plain against plain must pass, every mutation of the "on" results that a bug could cause must fail, the one inside the tolerance pass
selftest() {
	local kind want got applied files fl
	# plain against plain: one variant is all the harness needs to compare and to mutate (the others repeat the same plan)
	NOINDEX=1 SCENARIOS=none VARIANT_ARRAY='{on}'
	log "selftest: $FIXROWS orders, plain against plain"
	fresh_db "${DB_PREFIX}_fix"
	sql -v n="$FIXROWS" -v seed="$SEED" -f "$REPO/test/col/fixture.sql" >/dev/null
	fixture_indexes
	sql -c "VACUUM (FREEZE, ANALYZE)" >/dev/null
	mapfile -t fl < <(files "$REPO"/test/col/fixtures/*.sql | grep -E '/(s1_metrics_summary|s2_item_category_counts|s4_month_series|s5_store_topn|f_count_sum_minmax|f_group_source|f_scalar_paths|t_float8_nan_inf|t_sum_int4_wide|t_text_collation|n_empty_global)\.sql$')
	for kind in none int float float-tiny text null rows dup order type cols; do
		MUTATE=; [[ $kind == none ]] || MUTATE=$kind
		want=FAIL; [[ $kind == none || $kind == float-tiny ]] && want=PASS
		run_corpus "selftest-$kind" "${fl[@]}"
		got=PASS; (( $(failures "selftest-$kind") == 0 )) || got=FAIL
		applied=$(sql -v scen="selftest-$kind" <<-'SQL'
			SELECT count(*) FROM coldiff.results WHERE scenario = :'scen' AND detail LIKE '%[mutated:%'
		SQL
		)
		if [[ $kind != none ]] && (( applied == 0 )); then printf 'selftest %-10s FAIL: the mutation applied to no result\n' "$kind"; FAILED=1; continue; fi
		if [[ $got == "$want" ]]; then printf 'selftest %-10s ok: %s as expected (%s results mutated)\n' "$kind" "$got" "$applied"
		else printf 'selftest %-10s FAIL: %s, expected %s (%s results mutated)\n' "$kind" "$got" "$want" "$applied"; FAILED=1; fi
	done
	[[ -z ${VERBOSE:-} ]] || table -c "SELECT scenario, name, variant, left(detail, 100) FROM coldiff.results WHERE NOT ok ORDER BY id LIMIT 20"
}

if [[ -n $SELFTEST ]]; then selftest; exit $FAILED; fi
[[ $SUITE == analytics ]] || fixtures_suite
[[ $SUITE == fixtures ]] || analytics_suite
exit $FAILED
