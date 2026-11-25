#!/usr/bin/env bash
# The fast high-level gate (Spec 84): the differential, plan, fuzz and lifecycle suites that see the whole product, the same for CI and for a developer's or an agent's verification.
# `--area all` (CI) is six stages over four lanes, about a minute, each lane with a cluster of its own, one install shared; `--diff BASE` and `--files` run only the stages the
# changed paths can move, in 15 to 35 s:
#
#   lane 1   corpus (col, table)       scripts/corpus.sh [--table] [--only GLOB|GLOB]
#            plangate (col, table, 4t) scripts/plangate.sh --runs 0 --slice smoke|AREA|all [--table | --lane stock-4t]
#            lifecycle                 DML, VACUUM, ADD COLUMN, ADD PRIMARY KEY, DROP COLUMN, VACUUM FULL, REINDEX and sift.check on a sift_col index and a sift_table
#            coldiff (col, table)      scripts/coldiff.sh --fast [--table]: DML, abort and VACUUM scenarios against the plain plan
#   lane 2   surface_sweep             test/fuzz/surface_sweep.py --tier quick: every SQL function with hostile arguments, the roles
#   lane 3   shapes_matrix             test/fuzz/shapes_matrix.py --tier quick
#   lane 4   txn_fuzz                  test/fuzz/txn/cli.py --shapes: two sessions, savepoints, cursors, prepared statements
#            tbl_twin_fuzz             test/fuzz/tbl_twin.py: a sift_table against its heap twin
#            bm25_fuzz, diff_fuzz      only for an area or a harness that names them
#   lane 5   regress                   scripts/pgtest.sh regress TEST...: the test/sql files of the change, on Format 1 (--include-sql, or a test/sql file edited)
#   every lane ends with the leak gate of its cluster (scripts/leakcheck.sh, run by pgtest.sh stop)
#
#   scripts/fast-gate.sh [--format f2|f1|release] [--diff BASE | --files PATH... | --area AREA[,AREA]] [--include-sql] [--assert] [--quick|--full] [--root DIR] [--keep] [--dry-run]
#   scripts/fast-gate.sh --selftest
#
#   --format F     f2 (default): the testing library, every index and table Format 2.0; f1: the testing library, Format 1 indexes (the compat-f1 lane); release: the release library,
#                  no sift.test_* setting and no sift._test_ function (what a user installs). SIFT_FORMAT of scripts/pgtest.sh, which starts each cluster
#   --area AREA    the areas to run (star clickbench coexist jsonb col table bm25 gpu sql, smoke or all: the six stages). Default, and what CI runs: all
#   --diff BASE    the paths of `git diff --name-only --no-renames BASE` (from the merge base, the working tree included) and the untracked files that scripts/areas.tsv maps
#   --files PATH.. the paths named, for scripts and for scripts/test-fast-gate-select.py
#                  A path goes to the areas scripts/areas.tsv gives it (scripts/areas.sh --owners: the {perf_AREA}, {fg_col} and {fg_sql} groups), the same map as scripts/perf-select.py and
#                  ci.sh affected; a path of an area beside others is run once for all (one corpus --only, one plangate --slice, one shapes_matrix --only); test/sql/X.sql runs X alone; a
#                  harness (test/corpus, test/plan, test/col, test/fuzz, test/matrix and their scripts) runs itself; a path no area owns (src/core, the Makefile, a new file of src/) runs
#                  smoke, never nothing; documentation and what no job runs runs nothing and exits 0
#   --include-sql  also the regression tests that execute the functions the paths touch (scripts/focus/*.tsv, scripts/focus-pins.tsv): at most 40, all when CI is set
#   --assert       a PostgreSQL built with --enable-cassert (the one on PATH when it has it, else scripts/pg-assert.sh --inject) and a scan of the server logs for a TRAP,
#                  a memory-context warning and the other findings of scripts/assert.sh
#   --quick|--full the sizes of the stages: quick (default) one seed, the quick tiers; full the whole plan shapes, the full tiers, four seeds
#   --root DIR     a root a build staged (scripts/ci-build.sh, with its .flavor). Default: SIFT_PREBUILT_ROOT, SIFT_PREBUILT_RELEASE for release, SIFT_ROOT when its flavor fits,
#                  else the tree is built and installed (scripts/pgtest.sh install; a developer's incremental build)
#   --keep         keeps the scratch directory, which holds the log of each step (a failure keeps it)
#   --dry-run      prints the paths, the areas, the regression tests, the lanes and the commands and runs none of them (no PostgreSQL, a tenth of a second)
#
# A harness is given --format F when its usage has it, and --table, --fast, --ddl or the multi-glob `--only GLOB|GLOB` when it has them: a stage whose harness has not got what the
# format needs says so and is skipped, not failed (the release library has no test-only hooks, so a harness without --format is skipped there); a corpus --only it cannot union
# runs the whole corpus. Exit 1 when a step fails. The clusters listen on SIFT_PORT (default 56130) + 1 to + 5.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
set -uo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)

usage() { awk 'NR > 1 && /^#/ { print; next } NR > 1 { exit }' "$0"; }
AREAS="star clickbench coexist jsonb col table bm25 gpu sql smoke all"
BAD='TRAP: |detected write past chunk end|problem in alloc set|could not find block containing chunk|pfree called with invalid pointer|invalid memory alloc request|missing lock for relation'

FORMAT=f2 TIER=quick AREA='' ASSERT='' ROOT_ARG='' KEEP='' DRY='' SELFTEST='' SEL_BASE='' SEL_FILES='' INCLUDE_SQL=''
PATHS=()
parse() {
	while (( $# )); do
		case $1 in
			--format) FORMAT=${2:-}; shift ;;
			--area) AREA=${2:-}; shift ;;
			--diff) SEL_BASE=${2:-}; shift ;;
			--files) SEL_FILES=1; while (( $# > 1 )) && [[ $2 != -* ]]; do PATHS+=("$2"); shift; done ;;
			--include-sql) INCLUDE_SQL=1 ;;
			--assert) ASSERT=1 ;;
			--quick) TIER=quick ;;
			--full) TIER=full ;;
			--root) ROOT_ARG=${2:-}; shift ;;
			--keep) KEEP=1 ;;
			--dry-run) DRY=1 ;;
			--selftest) SELFTEST=1 ;;
			-h|--help) usage; exit 0 ;;
			*) echo "fast-gate: unknown option $1" >&2; usage >&2; exit 2 ;;
		esac
		shift
	done
	[[ $FORMAT == f2 || $FORMAT == f1 || $FORMAT == release ]] || { echo "fast-gate: --format is f2, f1 or release, not '$FORMAT'" >&2; exit 2; }
	local a
	for a in ${AREA//,/ }; do [[ " $AREAS " == *" $a "* ]] || { echo "fast-gate: --area '$a' is none of: $AREAS" >&2; exit 2; }; done
	[[ $(printf '%s' "${AREA:+a}${SEL_BASE:+b}${SEL_FILES:+c}" | wc -c) -le 1 ]] || { echo "fast-gate: --area, --diff and --files are alternatives" >&2; exit 2; }
	[[ -z $SEL_FILES || ${#PATHS[@]} -gt 0 ]] || { echo "fast-gate: --files names no path" >&2; exit 2; }
	[[ -n $AREA$SEL_BASE$SEL_FILES ]] || AREA=all
	[[ -z $SEL_BASE || -n $(git -C "$REPO" rev-parse --verify -q "$SEL_BASE^{commit}") ]] || { echo "fast-gate: --diff $SEL_BASE is no commit of this checkout" >&2; exit 2; }
	[[ -z $ROOT_ARG || -d $ROOT_ARG ]] || { echo "fast-gate: --root $ROOT_ARG is no directory" >&2; exit 2; }
	return 0
}

# cassert: the PostgreSQL on PATH was built with assertions
cassert() { [[ $(pg_config --configure 2>/dev/null) == *--enable-cassert* ]]; }
# supports FLAG FILE: the harness names the flag (its usage), so that the gate works before and after the harness learns it
supports() { grep -qF -- "$1" "$REPO/$2" 2>/dev/null; }
supports_re() { grep -qE -- "$1" "$REPO/$2" 2>/dev/null; }

# the steps: S_NAME, S_LANE, S_CMD (quoted words, run by eval in the lane's environment), S_SKIP (why it does not run, or empty)
S_NAME=() S_LANE=() S_CMD=() S_SKIP=()
# add_step NAME LANE FILE "FLAGS the harness must have, or -" ARG...: --format F is added when the harness has it; the release library skips a harness that has not
add_step() {
	local name=$1 lane=$2 file=$3 need=$4 skip='' flag cmd
	shift 4
	local args=("$@")
	for flag in $need; do [[ $flag == - ]] || supports "$flag" "$file" || skip="${file##*/} has no $flag yet"; done
	if supports --format "$file"; then args+=(--format "$FORMAT")
	elif [[ $FORMAT == release ]]; then skip=${skip:-"${file##*/} has no --format: it needs test-only hooks the release library lacks"}; fi
	case $file in
		*.py) printf -v cmd '%q ' python3 "$REPO/$file" "${args[@]}" ;;
		*) printf -v cmd '%q ' "$REPO/$file" "${args[@]}" ;;
	esac
	S_NAME+=("$name") S_LANE+=("$lane") S_CMD+=("$cmd") S_SKIP+=("$skip")
}
# add_sql NAME LANE: the lifecycle smoke, the SQL below, ON_ERROR_STOP
add_sql() {
	local cmd
	printf -v cmd '%q ' "$REPO/scripts/pgtest.sh" psql -v ON_ERROR_STOP=1 -v "fmt=$FORMAT" -f "$WORK/lifecycle.sql"
	S_NAME+=("$1") S_LANE+=("$2") S_CMD+=("$cmd") S_SKIP+=('')
}

# ---------- the router: paths -> areas -> the stages ----------
# route turns PATHS (--files, or --diff BASE) into SEL_AREAS and into what each stage is asked (DEM); --area names the areas itself. The map is scripts/areas.tsv, read by
# scripts/areas.sh --owners as scripts/perf-select.py and ci.sh affected read it: {perf_AREA} rows for star, clickbench, coexist, jsonb, table, bm25 and gpu, {fg_col} and {fg_sql}
AREA_ORDER="star clickbench coexist jsonb col table bm25 gpu sql smoke"
OWNER_GROUPS=(perf_star perf_clickbench perf_coexist perf_jsonb perf_table perf_bm25 perf_gpu fg_col fg_sql)
declare -A DEM=() PATH_AREAS=() LANE_FMT=()
SEL_AREAS=() SQL_EDITED=() SQL_TESTS=() SQL_CUT=0 MB=''
# axis STEP AXIS SEP VALUES: the stage is asked for the VALUES (split at SEP) of an axis: the globs of corpus --only, the slices of plangate, the regexes of shapes_matrix. The asks of every
# area merge into one run of the stage; ALL widens an axis to everything, so the merged run is never narrower than any of them
axis() {
	local step=$1 key=$1:$2 sep=$3 v vs
	DEM[$step]=1
	IFS=$sep read -ra vs <<<"$4"
	for v in "${vs[@]}"; do
		[[ ${DEM[$key]-} == ALL ]] && return 0
		if [[ $v == ALL ]]; then DEM[$key]=ALL; return 0; fi
		[[ "$sep${DEM[$key]-}$sep" == *"$sep$v$sep"* ]] || DEM[$key]=${DEM[$key]:+${DEM[$key]}$sep}$v
	done
}
want() { DEM[$1]=1; }
# apply_area AREA: the stages and the slices of an area (Spec 84 section 3.1); smoke, which every path no area owns falls back to, is all
apply_area() {
	local slice=smoke
	[[ $TIER == quick ]] || slice=all
	case $1 in
		star) axis corpus only '|' '*join*|*scope*|*semi*|*anti*'; axis plangate slice , star; axis shapes_matrix only '|' join; axis shapes_matrix facts , ALL ;;
		clickbench) axis corpus only '|' '*agg*|*window*|*having*|*range*|*gsets*'; axis plangate slice , clickbench
			axis shapes_matrix only '|' 'group|win|topn|distinct'; axis shapes_matrix facts , ALL ;;
		coexist) axis corpus only '|' ALL; axis plangate slice , coexist,smoke; axis shapes_matrix only '|' ALL; axis shapes_matrix facts , ALL ;;
		jsonb) axis corpus only '|' '*json*|*unnest*|*path*|*elem*'; axis plangate slice , jsonb; axis coldiff corpus '|' 'json|unnest|path|elem|contains'
			axis coldiff scenarios , dml,vacuum ;;
		col) axis corpus only '|' ALL; axis plangate slice , smoke; axis coldiff corpus '|' ALL; axis coldiff scenarios , dml,abort,vacuum; want txn_fuzz
			axis surface_sweep only '|' 'col|check|maint|promote|merge|scope' ;;
		table) want corpus-table; axis plangate-table slice , table; axis coldiff-table corpus '|' ALL; axis coldiff-table scenarios , dml,abort,vacuum
			axis shapes_matrix only '|' ALL; axis shapes_matrix facts , tbl,tbl_bm25; want tbl_twin_fuzz ;;
		bm25) axis plangate slice , bm25; axis shapes_matrix only '|' ALL; axis shapes_matrix facts , bm25,tbl_bm25; want bm25_fuzz ;;
		gpu) axis plangate slice , gpu; axis corpus only '|' '*agg*' ;;
		sql) axis surface_sweep only '|' ALL; axis corpus only '|' ALL; axis plangate slice , smoke ;;
		smoke|all) axis corpus only '|' ALL; want corpus-table; axis plangate slice , "$slice"; axis plangate-table slice , "$slice"; axis plangate-4t slice , "$slice"; want lifecycle
			axis coldiff corpus '|' ALL; axis coldiff scenarios , dml,abort,vacuum; axis coldiff-table corpus '|' ALL; axis coldiff-table scenarios , dml,abort,vacuum
			axis surface_sweep only '|' ALL; axis shapes_matrix only '|' ALL; axis shapes_matrix facts , ALL; want txn_fuzz; want tbl_twin_fuzz ;;
	esac
}
# harness_path PATH: a harness, or a script of one, asks for itself (all of it, as the whole gate would run it); 1 for any other path
harness_path() {
	case $1 in
		test/corpus/*|scripts/corpus.sh) axis corpus only '|' ALL; want corpus-table ;;
		test/plan/*|scripts/plangate.sh) axis plangate slice , smoke; axis plangate-table slice , smoke; axis plangate-4t slice , smoke ;;
		test/col/*|test/matrix/*|scripts/coldiff.sh|scripts/coverage.sh) axis coldiff corpus '|' ALL; axis coldiff scenarios , dml,abort,vacuum
			axis coldiff-table corpus '|' ALL; axis coldiff-table scenarios , dml,abort,vacuum ;;
		test/fuzz/shapes_matrix*) axis shapes_matrix only '|' ALL; axis shapes_matrix facts , ALL ;;
		test/fuzz/surface_sweep*) axis surface_sweep only '|' ALL ;;
		test/fuzz/txn*) want txn_fuzz ;;
		test/fuzz/tbl_twin*|test/t/table/twin_fuzz*) want tbl_twin_fuzz ;;
		test/fuzz/diff*) want diff_fuzz ;;
		test/fuzz/bm25*) want bm25_fuzz ;;
		*) return 1 ;;
	esac
}
# owner_areas OWNER...: the areas of one path from the owners of scripts/areas.sh --owners: none of the groups (a core file, the Makefile, a new file of src/, a script) is smoke, `-` (a
# path no job builds or runs: documentation) is nothing; the column index is its own area only when no other area owns the path (src/col/shred/shred.c is jsonb, src/col/v1/amscan/cost.c coexist)
owner_areas() {
	local o=" $* " a=() g
	REPLY=
	[[ $o == ' - ' ]] && return 0
	for g in star clickbench coexist jsonb table bm25 gpu; do [[ $o == *" perf_$g "* ]] && a+=("$g"); done
	[[ $o == *' fg_sql '* ]] && a+=(sql)
	(( ${#a[@]} )) || { [[ $o == *' fg_col '* ]] && a+=(col); }
	(( ${#a[@]} )) || a=(smoke)
	REPLY=${a[*]}
}
# gather_paths: PATHS of --diff BASE: the changed files of the merge base to the working tree, and the untracked files the map covers (another lane's scratch in a shared tree is no change of this one)
gather_paths() {
	local f
	[[ -n $SEL_BASE ]] || return 0
	MB=$(git -C "$REPO" merge-base "$SEL_BASE" HEAD) || { echo "fast-gate: no merge base of $SEL_BASE and HEAD" >&2; exit 2; }
	mapfile -t PATHS < <(cd "$REPO" && { git diff --name-only --no-renames "$MB"; git ls-files --others --exclude-standard | { source scripts/areas.sh; areas_load scripts/areas.tsv
		while IFS= read -r f; do [[ -z $(areas_unmatched "$f") ]] && echo "$f"; done; }; } | sort -u)
}
route() {
	local p n i line rest=() owners=() out=() a
	INPUT=${SEL_BASE:+diff}${SEL_FILES:+files}${AREA:+area}
	if [[ -n $AREA ]]; then
		for a in ${AREA//,/ }; do apply_area "$a"; done
		SEL_AREAS=(${AREA//,/ })
		return 0
	fi
	gather_paths
	for p in "${PATHS[@]}"; do
		case $p in
			test/sql/*.sql|test/expected/*.out) n=${p#test/*/}; n=${n%.*}
				[[ -f $REPO/test/sql/$n.sql && -f $REPO/test/expected/$n.out && " ${SQL_EDITED[*]-} " != *" $n "* ]] && SQL_EDITED+=("$n"); PATH_AREAS[$p]=test/sql ;;
			*) if harness_path "$p"; then PATH_AREAS[$p]=harness; else rest+=("$p"); fi ;;
		esac
	done
	if (( ${#rest[@]} )); then
		mapfile -t out < <(bash "$REPO/scripts/areas.sh" --owners "$REPO/scripts/areas.tsv" "${OWNER_GROUPS[@]}" -- "${rest[@]}" </dev/null)
		i=0
		for line in "${out[@]}"; do
			if [[ $line == -- ]]; then owner_areas "${owners[@]}"; PATH_AREAS[${rest[i++]}]=$REPLY; owners=(); else owners+=("$line"); fi
		done
	fi
	for a in $AREA_ORDER; do
		for p in "${PATHS[@]}"; do [[ " ${PATH_AREAS[$p]-} " == *" $a "* ]] && { SEL_AREAS+=("$a"); apply_area "$a"; break; }; done
	done
}
# resolve_sql: SQL_TESTS, the regression tests to run: the test/sql files edited, and with --include-sql the ones that execute the functions of the src/ files (by hunk with --diff, by
# file with --files: scripts/ci/focus.sh), at most 40 unless CI is set
resolve_sql() {
	local p t src=() toks=() fnt=() bare=() names=() n
	names=("${SQL_EDITED[@]}")
	if [[ -n $INCLUDE_SQL ]]; then
		source "$REPO/scripts/ci/focus.sh"
		for p in "${PATHS[@]}"; do [[ $p =~ ^src/.*\.[ch]$ && -f $REPO/$p ]] && src+=("$p"); done
		if [[ -n $SEL_BASE ]] && (( ${#src[@]} )); then
			mapfile -t toks < <(focus_tokens "$MB")
			for t in "${toks[@]}"; do [[ $t == *:'*' ]] || fnt+=("$t"); done
			(( ${#fnt[@]} )) && names+=($(focus_select "${fnt[@]}" | sed -n 's/^T //p'))
			for p in "${src[@]}"; do [[ " ${fnt[*]-} " == *" $p:"* ]] || bare+=("$p"); done
		else bare=("${src[@]}"); fi
		(( ${#bare[@]} )) && names+=($(focus_file_tests "${bare[@]}"))
	fi
	for n in "${names[@]}"; do
		[[ -f $REPO/test/expected/$n.out && -f $REPO/test/sql/$n.sql && " ${SQL_TESTS[*]-} " != *" $n "* ]] && SQL_TESTS+=("$n")
	done
	if [[ -z ${CI:-} && ${#SQL_TESTS[@]} -gt 40 ]]; then SQL_CUT=$(( ${#SQL_TESTS[@]} - 40 )); SQL_TESTS=("${SQL_TESTS[@]:0:40}"); fi
}

# the plan of the stages the router asked for, in the order of the lanes
build_plan() {
	local seeds=1 secs=12 matrix=quick only facts f kept=() a=() sc
	[[ $TIER == quick ]] || { seeds=1-4 secs=40 matrix=full; }
	S_NAME=() S_LANE=() S_CMD=() S_SKIP=()
	route
	resolve_sql
	if [[ -n ${DEM[corpus]-} ]]; then
		only=${DEM[corpus:only]}
		if [[ $only != ALL && $only == *'|'* ]] && ! supports_re '--only GLOB\[?\|' scripts/corpus.sh; then only=ALL; fi
		a=(); [[ $only == ALL ]] || a=(--only "$only")
		add_step corpus 1 scripts/corpus.sh - ${a[@]+"${a[@]}"}
	fi
	[[ -z ${DEM[corpus-table]-} ]] || add_step corpus-table 1 scripts/corpus.sh --table --table
	[[ -z ${DEM[plangate]-} ]] || add_step plangate 1 scripts/plangate.sh - --runs 0 --slice "${DEM[plangate:slice]}"
	[[ -z ${DEM[plangate-table]-} ]] || add_step plangate-table 1 scripts/plangate.sh - --runs 0 --slice "${DEM[plangate-table:slice]}" --table
	[[ -z ${DEM[plangate-4t]-} ]] || add_step plangate-4t 1 scripts/plangate.sh - --runs 0 --slice "${DEM[plangate-4t:slice]}" --lane stock-4t
	[[ -z ${DEM[lifecycle]-} ]] || add_sql lifecycle 1
	for f in coldiff coldiff-table; do
		[[ -n ${DEM[$f]-} ]] || continue
		sc=${DEM[$f:scenarios]} a=(--variants on,on-par --scenarios "$sc")
		[[ $f == coldiff ]] || a+=(--table)
		if supports --fast scripts/coldiff.sh; then a+=(--fast); else a+=(--fail-fast --suite fixtures --fixture-rows 1500 --scenario-slices 3); fi
		if [[ ${DEM[$f:corpus]} != ALL ]]; then a+=(--corpus "${DEM[$f:corpus]}"); elif ! supports --fast scripts/coldiff.sh; then a+=(--slice 1/48); fi
		add_step "$f" 1 scripts/coldiff.sh "$([[ $f == coldiff ]] || echo --table)" "${a[@]}"
	done
	[[ -z ${DEM[surface_sweep]-} ]] || add_step surface_sweep 2 test/fuzz/surface_sweep.py - --tier "$matrix" --use-cluster $([[ ${DEM[surface_sweep:only]} == ALL ]] || echo --only "${DEM[surface_sweep:only]}")
	if [[ -n ${DEM[shapes_matrix]-} ]]; then
		a=(--tier "$matrix"); [[ ${DEM[shapes_matrix:only]} == ALL ]] || a+=(--only "${DEM[shapes_matrix:only]}")
		facts=${DEM[shapes_matrix:facts]}
		if [[ $facts != ALL ]]; then
			for f in ${facts//,/ }; do [[ $f != tbl* ]] || supports "'$f'" test/fuzz/shapes_matrix.py && kept+=("$f"); done
			(( ${#kept[@]} )) && a+=(--facts "$(IFS=,; echo "${kept[*]}")")
		fi
		add_step shapes_matrix 3 test/fuzz/shapes_matrix.py - "${a[@]}"
		[[ $facts == ALL || ${#kept[@]} -gt 0 ]] || S_SKIP[-1]="shapes_matrix.py has no fact $facts yet"
	fi
	if [[ -n ${DEM[txn_fuzz]-} ]]; then
		add_step txn_fuzz 4 test/fuzz/txn/cli.py - --shapes --seeds "$seeds" --stop-after 1
		! supports --table test/fuzz/txn/cli.py || add_step txn_fuzz-table 4 test/fuzz/txn/cli.py - --shapes --seeds "$seeds" --stop-after 1 --table
	fi
	[[ -z ${DEM[tbl_twin_fuzz]-} ]] || add_step tbl_twin_fuzz 4 test/fuzz/tbl_twin.py - --seeds "$seeds" --seconds "$secs" $(supports --ddl test/fuzz/tbl_twin.py && echo --ddl)
	[[ -z ${DEM[bm25_fuzz]-} ]] || add_step bm25_fuzz 4 test/fuzz/bm25.py - --seeds "$seeds" --queries 20 --stop-after 1
	if [[ -n ${DEM[diff_fuzz]-} ]]; then
		add_step diff_fuzz 4 test/fuzz/diff/cli.py - --seeds "$seeds" --queries 40 --rows 3000 --leader-on --stop-after 1
		add_step diff_fuzz-table 4 test/fuzz/diff/cli.py - --seeds "$seeds" --queries 40 --rows 3000 --leader-on --stop-after 1 --table
	fi
	if (( ${#SQL_TESTS[@]} )); then
		add_step regress 5 scripts/pgtest.sh - regress "${SQL_TESTS[@]}"
		LANE_FMT[5]=f1
		[[ $FORMAT != release ]] || S_SKIP[-1]="test/sql runs on the release library through scripts/release-library-check.sh (ci.sh release-library-regress)"
	fi
	return 0
}

# ---------- the library, the root and the PostgreSQL ----------
flavor_ok() {   # ROOT: the flavor its stage recorded has the testing library for f1 and f2, the release library for release
	local f; f=$(<"$1/.flavor") || return 1
	if [[ $FORMAT == release ]]; then [[ " $f " != *' testing '* ]]; else [[ " $f " == *' testing '* ]]; fi
}
# resolve_root: sets ROOT and ROOT_HOW
resolve_root() {
	local c cand=()
	if [[ -n $ROOT_ARG ]]; then cand=("$ROOT_ARG")
	elif [[ $FORMAT == release ]]; then cand=("${SIFT_PREBUILT_RELEASE:-}" "${SIFT_ROOT:-}")
	else cand=("${SIFT_PREBUILT_ROOT:-}" "${SIFT_ROOT:-}"); fi
	for c in "${cand[@]}"; do
		[[ -n $c && -f $c/.flavor ]] || continue
		flavor_ok "$c" && { ROOT=$c ROOT_HOW="staged ($(<"$c/.flavor"))"; return 0; }
		[[ $c != "$ROOT_ARG" ]] || { echo "fast-gate: $c holds the flavor '$(<"$c/.flavor")', not the one --format $FORMAT needs" >&2; exit 2; }
	done
	ROOT=$WORK/root ROOT_HOW="built here: scripts/pgtest.sh install$([[ $FORMAT == release ]] && echo ' (SIFT_TESTING= empty)')"
	return 0
}
library_line() {
	case $FORMAT in
		f2) echo "testing library (SIFT_TESTING=1), sift.test_create_format1 = off: Format 2.0 indexes and tables" ;;
		f1) echo "testing library (SIFT_TESTING=1), sift.test_create_format1 = on: Format 1 indexes" ;;
		release) echo "release library (SIFT_TESTING= empty): no sift.test_* setting, no sift._test_ function" ;;
	esac
}
pg_line() { if [[ -z $ASSERT ]]; then echo "pg_config on PATH ($(pg_config --version 2>/dev/null || echo none))"
	elif cassert; then echo "pg_config on PATH has assertions ($(pg_config --version))"
	else echo "assertions: the prefix of scripts/pg-assert.sh --inject, built when missing; server logs scanned for TRAP and memory-context findings"; fi; }

# ---------- the run ----------
BASE_PORT=${SIFT_PORT:-56130}
LANE_IDS() { printf '%s\n' "${S_LANE[@]}" | sort -un; }
lane_tmp() { echo "$WORK/lane$1"; }
lane_port() { echo $(( BASE_PORT + $1 )); }

print_plan() {
	local k i p st='' sk=''
	echo "fast-gate: format=$FORMAT tier=$TIER input=$INPUT assert=${ASSERT:+yes}${ASSERT:-no}"
	echo "paths:     ${#PATHS[@]}"
	for p in "${PATHS[@]}"; do echo "  $p -> ${PATH_AREAS[$p]:-nothing}"; done
	echo "areas:     ${SEL_AREAS[*]:--}"
	echo "sql:       ${SQL_TESTS[*]:--}${SQL_CUT:+$([[ $SQL_CUT == 0 ]] || echo " ($SQL_CUT more cut at 40: CI=1 runs them)")}"
	for i in "${!S_NAME[@]}"; do [[ -n ${S_SKIP[i]} ]] && sk+=" ${S_NAME[i]}" || st+=" ${S_NAME[i]}"; done
	st=${st:- -} sk=${sk:- -}
	echo "steps:     ${st# }"
	echo "skipped:   ${sk# }"
	echo "library:   $(library_line)"
	echo "postgres:  $(pg_line)"
	echo "root:      $ROOT_HOW"
	for k in $(LANE_IDS); do
		echo "lane $k:    SIFT_TMP=<work>/lane$k SIFT_PORT=$(lane_port "$k")"
		for i in "${!S_NAME[@]}"; do
			[[ ${S_LANE[i]} == "$k" ]] || continue
			if [[ -n ${S_SKIP[i]} ]]; then printf '  %-15s SKIP: %s\n' "${S_NAME[i]}" "${S_SKIP[i]}"
			else printf '  %-15s %s\n' "${S_NAME[i]}" "$(sed "s#$REPO/##g; s#$WORK#<work>#g; s#\\\\,#,#g; s# \$##" <<<"${S_CMD[i]}")"; fi
		done
		echo "  leakcheck       scripts/pgtest.sh stop (the leak gate of the cluster of lane $k)"
	done
}

# step_run INDEX: the step with its log, "STATUS SECONDS NOTE" in $OUT/NAME.res
step_run() {
	local i=$1 name=${S_NAME[$1]} t=$SECONDS rc
	if [[ -n ${S_SKIP[i]} ]]; then echo "SKIP 0 ${S_SKIP[i]}" >"$OUT/$name.res"; return 0; fi
	( cd "$REPO" && eval "${S_CMD[i]}" ) >"$OUT/$name.log" 2>&1; rc=$?
	if (( rc == 0 )); then echo "ok $((SECONDS - t)) $(tail -n 1 "$OUT/$name.log" | cut -c1-70)" >"$OUT/$name.res"
	else echo "FAIL $((SECONDS - t)) exit $rc" >"$OUT/$name.res"; fi
}
# lane_extensions K: the harnesses that run in the postgres database (tbl_twin_fuzz: PGHOST and PGPORT, the testing extension) find the extensions there
lane_extensions() {
	local i
	for i in "${!S_NAME[@]}"; do
		[[ ${S_LANE[i]} == "$1" && ${S_NAME[i]} == tbl_twin_fuzz && -z ${S_SKIP[i]} ]] || continue
		psql -X -qAt -d postgres -c 'CREATE EXTENSION pg_sift' -c 'CREATE EXTENSION pg_sift_test' >"$OUT/lane$1.ext.log" 2>&1
	done
}
# export_format F: f2, f1 or release as the clusters of scripts/pgtest.sh (SIFT_FORMAT) and the harnesses (SIFT_TEST_FORMAT) read it
export_format() { export SIFT_TEST_FORMAT=$1 SIFT_FORMAT=$([[ $1 == f1 ]] && echo 1 || echo 2); }
# lane_run K: the cluster of the lane, its steps in order, the leak gate and, with --assert, the scan of its logs
lane_run() {
	local k=$1 i t rc=0 d
	d=$(lane_tmp "$k")
	export SIFT_TMP=$d SIFT_PORT=$(lane_port "$k") SIFT_FUZZ_OUT=$d/fuzz
	export_format "${LANE_FMT[$k]:-$FORMAT}"
	mkdir -p "$d" "$SIFT_FUZZ_OUT"
	if [[ -z ${FAST_GATE_NO_CLUSTER:-} ]]; then
		"$REPO/scripts/pgtest.sh" start >"$OUT/lane$k.start.log" 2>&1 || { echo "FAIL 0 the cluster did not start (lane$k.start.log)" >"$OUT/cluster-$k.res"; return 1; }
	fi
	if [[ -z ${FAST_GATE_NO_CLUSTER:-} ]]; then
		eval "$("$REPO/scripts/pgtest.sh" env)"
		lane_extensions "$k"
	fi
	for i in "${!S_NAME[@]}"; do [[ ${S_LANE[i]} == "$k" ]] && step_run "$i"; done
	t=$SECONDS
	if [[ -z ${FAST_GATE_NO_CLUSTER:-} ]]; then
		"$REPO/scripts/pgtest.sh" stop >"$OUT/leakcheck-$k.log" 2>&1 || rc=1
		echo "$([[ $rc == 0 ]] && echo ok || echo FAIL) $((SECONDS - t)) the leak gate of lane $k" >"$OUT/leakcheck-$k.res"
		if [[ -n $ASSERT ]]; then
			if grep -rEh "$BAD" "$d" --include='*.log' >"$OUT/assert-$k.log" 2>/dev/null; then echo "FAIL 0 a server log has a finding (assert-$k.log)" >"$OUT/assert-$k.res"
			else echo "ok 0 no TRAP or memory-context finding in the logs of lane $k" >"$OUT/assert-$k.res"; fi
		fi
	fi
}

summary() {
	local i name res st secs note bad=0 f k
	printf '\n%-17s %-5s %-5s %5s  %s\n' step lane state secs note
	for i in "${!S_NAME[@]}"; do
		name=${S_NAME[i]}; read -r st secs note <"$OUT/$name.res" 2>/dev/null || { st=FAIL secs=0 note="did not run"; }
		printf '%-17s %-5s %-5s %5s  %s\n' "$name" "${S_LANE[i]}" "$st" "$secs" "$note"
		[[ $st != FAIL ]] || { bad=1; tail -n 20 "$OUT/$name.log" 2>/dev/null | sed 's/^/    | /'; }
	done
	for f in "$OUT"/cluster-*.res "$OUT"/leakcheck-*.res "$OUT"/assert-*.res; do
		[[ -f $f ]] || continue
		read -r st secs note <"$f"; name=$(basename "$f" .res)
		printf '%-17s %-5s %-5s %5s  %s\n' "$name" "${name##*-}" "$st" "$secs" "$note"
		[[ $st != FAIL ]] || { bad=1; tail -n 20 "$OUT/lane${name##*-}.start.log" "$OUT/$name.log" 2>/dev/null | sed 's/^/    | /'; }
	done
	echo "fast-gate: $FORMAT $TIER$([[ -n $ASSERT ]] && echo ' assert'): $((SECONDS - T0)) s wall, $( (( bad )) && echo FAILED || echo ok); logs in $OUT"
	return $bad
}

cleanup() { [[ -n $KEEP || -n ${FAILED:-} ]] || rm -rf "$WORK"; }
run_all() {
	local k pids=()
	if [[ -z ${FAST_GATE_NO_CLUSTER:-} ]]; then
		pgtest_env
		if [[ $ROOT == "$WORK/root" ]]; then
			( export SIFT_TMP=$WORK/install SIFT_STAGE=1; mkdir -p "$SIFT_TMP"
			  if [[ -n $ASSERT ]]; then
				  source "$REPO/scripts/source-snapshot.sh"; mkdir -p "$WORK/tree" && copy_git_sources "$REPO" "$WORK/tree" && cd "$WORK/tree"
			  else cd "$REPO"; fi
			  scripts/pgtest.sh install ) >"$OUT/install.log" 2>&1 || { echo "fast-gate: the install failed:"; tail -n 20 "$OUT/install.log"; FAILED=1; return 1; }
		fi
		"$REPO/scripts/pgtest.sh" initdb-template "$WORK/initdb" >/dev/null 2>&1 && export SIFT_INITDB_TEMPLATE=$WORK/initdb
	fi
	for k in $(LANE_IDS); do lane_run "$k" & pids+=($!); done
	wait "${pids[@]}"
	summary || { FAILED=1; return 1; }
}
# the environment pgtest.sh and the harnesses read: the format, the library, the root
pgtest_env() {
	export_format "$FORMAT"
	export SIFT_ROOT=$ROOT SIFT_TESTING=1
	[[ $FORMAT != release ]] || export SIFT_TESTING=
	if [[ $ROOT != "$WORK/root" ]]; then export SIFT_PREBUILT_ROOT=$ROOT; else unset SIFT_PREBUILT_ROOT; fi
	if [[ -n $ASSERT ]] && ! cassert; then
		PATH=$("$REPO/scripts/pg-assert.sh" --inject)/bin:$PATH; export PATH
		cassert || { echo "fast-gate: the PostgreSQL of scripts/pg-assert.sh has no assertions" >&2; exit 1; }
	fi
	return 0
}

# the lifecycle smoke: what a Format 2.0 table and index go through in a day, each step proven by sift.check and the rows against the plain plan
write_lifecycle_sql() {
	cat >"$WORK/lifecycle.sql" <<'SQL'
\set QUIET on
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
CREATE TABLE fg_col (id int, store int, total numeric, note text);
INSERT INTO fg_col SELECT i, i % 40, (i % 997) / 7.0, 'n' || (i % 13) FROM generate_series(1, 30000) i;
CREATE INDEX fg_col_i ON fg_col USING sift_col (id, store, total, note);
CREATE TABLE fg_tbl (id int, store int, total numeric, note text) USING sift_table;
INSERT INTO fg_tbl SELECT i, i % 40, (i % 997) / 7.0, 'n' || (i % 13) FROM generate_series(1, 30000) i;
-- the format the harnesses claim to run on is the format the objects have
CREATE FUNCTION fg_format(fmt text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE want int := CASE fmt WHEN 'f1' THEN 1 ELSE 2 END; n int;
BEGIN
	SELECT count(*) INTO n FROM sift.status() s WHERE s.format_major IS DISTINCT FROM want;
	IF n > 0 THEN RAISE EXCEPTION 'the sift objects are not all in format major % (--format %)', want, fmt; END IF;
END $$;
SELECT fg_format(:'fmt');
CREATE FUNCTION fg_sum(t regclass) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN EXECUTE format('SELECT md5(string_agg(x::text, ''|'' ORDER BY store)) FROM (SELECT store, count(*), sum(total) FROM %s GROUP BY store) x', t) INTO r; RETURN r; END $$;
CREATE FUNCTION fg_parity(t regclass) RETURNS void LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL sift.enable_agg = off; a := fg_sum(t);
	SET LOCAL sift.enable_agg = on; b := fg_sum(t);
	IF a IS DISTINCT FROM b THEN RAISE EXCEPTION 'rows of % differ with sift.enable_agg off and on', t; END IF;
END $$;
CREATE FUNCTION fg_check(step text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE n int;
BEGIN
	SELECT count(*) INTO n FROM sift.check();
	IF n > 0 THEN RAISE EXCEPTION 'sift.check finds % problem(s) after %', n, step; END IF;
	PERFORM fg_parity('fg_col'); PERFORM fg_parity('fg_tbl');
END $$;
SELECT fg_check('the load');
UPDATE fg_col SET total = total + 1 WHERE id % 7 = 0;      UPDATE fg_tbl SET total = total + 1 WHERE id % 7 = 0;
DELETE FROM fg_col WHERE id % 11 = 0;                      DELETE FROM fg_tbl WHERE id % 11 = 0;
INSERT INTO fg_col SELECT i, i % 40, 1, 'x' FROM generate_series(30001, 33000) i;
INSERT INTO fg_tbl SELECT i, i % 40, 1, 'x' FROM generate_series(30001, 33000) i;
SELECT fg_check('DML');
VACUUM fg_col; VACUUM fg_tbl;
SELECT fg_check('VACUUM');
SELECT sift.maintain('fg_col_i'::regclass) IS NOT NULL AS m1, sift.maintain('fg_tbl'::regclass) IS NOT NULL AS m2 \gset
SELECT fg_check('sift.maintain');
ALTER TABLE fg_tbl ADD COLUMN extra int DEFAULT 7;
INSERT INTO fg_tbl SELECT i, i % 40, 2, 'y', i FROM generate_series(40001, 41000) i;
SELECT fg_check('ADD COLUMN');
ALTER TABLE fg_tbl DROP COLUMN note;
SELECT fg_check('DROP COLUMN');
DELETE FROM fg_tbl WHERE id > 33000 AND id % 2 = 0;
ALTER TABLE fg_tbl ADD PRIMARY KEY (id);
UPDATE fg_tbl SET total = total + 3 WHERE id % 5 = 0;
SELECT fg_check('ADD PRIMARY KEY');
VACUUM FULL fg_tbl;
SELECT fg_check('VACUUM FULL');
REINDEX INDEX fg_col_i;
SELECT fg_check('REINDEX');
DROP TABLE fg_col, fg_tbl;
\echo lifecycle: DML, VACUUM, maintain, ADD/DROP COLUMN, ADD PRIMARY KEY, VACUUM FULL, REINDEX: sift.check and the rows clean
SQL
}

# ---------- the selftest: the arguments, the plan, the runner (no cluster) ----------
selftest() {
	local out bad=0 fmt tmp
	d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
	fail() { echo "FAIL: $*"; bad=1; }
	for fmt in f2 f1 release; do
		out=$("$0" --dry-run --format "$fmt" 2>&1) || fail "--dry-run --format $fmt exits $?"
		[[ $out == *"format=$fmt "* && $out == *'lane 1:'* && $out == *'lane 4:'* && $out == *'leakcheck'* ]] || fail "--dry-run --format $fmt misses the format, a lane or the leak gate: $out"
		for s in corpus plangate plangate-4t lifecycle coldiff shapes_matrix surface_sweep txn_fuzz tbl_twin_fuzz; do [[ $out == *"  $s "* ]] || fail "--dry-run --format $fmt lists no step $s"; done
	done
	out=$("$0" --dry-run --format release); [[ $out == *'release library (SIFT_TESTING= empty)'* && $out == *'skipped:   -'* ]] || fail "release: no release library line, or a harness is skipped"
	out=$("$0" --dry-run --format f2); [[ $out == *'test_create_format1 = off'* ]] || fail "f2 does not say sift.test_create_format1 = off"
	out=$("$0" --dry-run --format f1); [[ $out == *'test_create_format1 = on'* ]] || fail "f1 does not say sift.test_create_format1 = on"
	out=$("$0" --dry-run --format f2 --assert); [[ $out == *'assertions'* ]] || fail "--assert says nothing of assertions"
	out=$("$0" --dry-run --area star,table); [[ $out == *'--slice star'* && $out == *'areas:     star table'* && $out == *'coldiff-table'* ]] || fail "--area star,table does not run the plan slice star and the table stages"
	out=$("$0" --dry-run --full); [[ $out == *'--slice all'* && $out == *'--tier full'* ]] || fail "--full does not run every plan shape and the full tiers"
	out=$("$0" --dry-run); [[ $out == *'areas:     all'* && $out == *'--slice smoke'* && $out == *'--tier quick'* ]] || fail "the default (--area all, --quick) does not run the smoke shapes and the quick tiers"
	out=$("$0" --dry-run --files README.md); [[ $out == *'nothing to run'* ]] || fail "documentation runs a check"
	out=$("$0" --dry-run --files src/new_thing.c); [[ $out == *'areas:     smoke'* ]] || fail "an unmapped src/ file is not smoke"
	"$0" --area star --files README.md >/dev/null 2>&1; (( $? == 2 )) || fail "--area with --files does not exit 2"
	"$0" --diff no-such-commit >/dev/null 2>&1; (( $? == 2 )) || fail "--diff of no commit does not exit 2"
	"$0" --format nope >/dev/null 2>&1; (( $? == 2 )) || fail "an unknown format does not exit 2"
	"$0" --area nope >/dev/null 2>&1; (( $? == 2 )) || fail "an unknown area does not exit 2"
	"$0" --bogus >/dev/null 2>&1; (( $? == 2 )) || fail "an unknown option does not exit 2"
	# supports and the flags a harness has
	mkdir -p "$d/repo/scripts"; printf '#   h.sh [--format f2|f1|release] [--fast]\n' >"$d/repo/scripts/h.sh"
	( REPO=$d/repo; supports --format scripts/h.sh && supports --fast scripts/h.sh && ! supports --table scripts/h.sh && ! supports --format scripts/nofile.sh ) || fail "supports does not read the usage of a harness"
	( REPO=$d/repo; FORMAT=release; S_NAME=(); add_step t 1 scripts/h.sh "--table"; [[ -n ${S_SKIP[0]} && ${S_SKIP[0]} == *'no --table'* ]] ) || fail "a harness without the flag a step needs is not skipped"
	( REPO=$d/repo; FORMAT=release; add_step t 1 scripts/h.sh -; [[ -z ${S_SKIP[0]} && ${S_CMD[0]} == *'--format release'* ]] ) || fail "a harness with --format is not given the format"
	( REPO=$d/repo; printf '#   g.sh\n' >"$d/repo/scripts/g.sh"; FORMAT=release; add_step t 1 scripts/g.sh -; [[ ${S_SKIP[0]} == *'no --format'* ]] ) || fail "the release library runs a harness that has no --format"
	( REPO=$d/repo; printf '#   g.sh\n' >"$d/repo/scripts/g.sh"; FORMAT=f2; add_step t 1 scripts/g.sh -; [[ -z ${S_SKIP[0]} ]] ) || fail "f2 skips a harness that has no --format"
	# the runner: three lanes side by side, a failure fails the run and is named, a skip is not a failure
	WORK=$d/work OUT=$d/work/out FAST_GATE_NO_CLUSTER=1; mkdir -p "$OUT"
	S_NAME=(ok1 ok2 bad skipped) S_LANE=(1 2 3 3) S_CMD=("sleep 1 " "sleep 1 " "echo boom; false " "true ") S_SKIP=('' '' '' 'not yet')
	T0=$SECONDS; tmp=$SECONDS
	( ASSERT=; run_lane_set() { local k pids=(); for k in $(LANE_IDS); do lane_run "$k" & pids+=($!); done; wait "${pids[@]}"; }; run_lane_set; out=$(summary); rc=$?
	  (( rc == 1 )) || { echo "FAIL: a failed step does not fail the run (rc $rc)"; exit 1; }
	  [[ $out == *'bad '*FAIL* && $out == *'boom'* && $out == *'skipped '*SKIP*'not yet'* && $out == *'ok1 '*ok* ]] || { echo "FAIL: the summary does not show ok, FAIL with its log and SKIP: $out"; exit 1; } ) || bad=1
	(( SECONDS - tmp < 3 )) || fail "the lanes do not run side by side ($((SECONDS - tmp)) s for two 1 s steps)"
	(( bad )) || echo "fast-gate: the arguments, the plan of each format, the skips and the lanes of the runner"
	return $bad
}

main() {
	parse "$@"
	if [[ -n $SELFTEST ]]; then selftest; exit; fi
	WORK=$(mktemp -d "${SIFT_TMP:-${TMPDIR:-/tmp}}/fast-gate-XXXXXX") OUT=$WORK/out
	mkdir -p "$OUT"
	trap cleanup EXIT
	trap 'FAILED=1; exit 130' INT TERM
	resolve_root
	write_lifecycle_sql
	build_plan
	(( ${#S_NAME[@]} )) || { print_plan; echo 'fast-gate: nothing to run'; exit 0; }
	if [[ -n $DRY ]]; then print_plan; exit 0; fi
	print_plan
	T0=$SECONDS
	run_all
}
main "$@"
