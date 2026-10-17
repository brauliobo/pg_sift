#!/usr/bin/env bash
# Single entry point for the test suites, with a pass/fail/skip summary.
#
#   scripts/ci.sh [quick|full]        (make check | make check-full)
#
# quick (default, about a minute on 8 free cores): non-ASan build, small corpora and short runs. installcheck (also against the
#   production library, without the test-only functions: scripts/production-check.sh), isolation (emulated and real), the Perl and shell TAP tests (sift only, fewer crash
#   iterations), a 2 500 document differential test and 8 of the 16 mutants of the harness (the
#   fast-to-detect ones), a 30 s soak and a 20 s fuzz budget.
# full: ASan build and clusters, installcheck, the differential test at 5 000 documents, all 16
#   mutants, every isolation and TAP test (GIN control included), all fuzz targets, GIN-control
#   and sift soaks (600 s) and the server sanitizer report.
# SANITIZE=1 makes quick run on the ASan build too.
#
# The suites are cut into jobs (a regression test, a TAP file, a differential test...) that a pool of lanes takes from one
# queue, longest first: SIFT_LANES lanes (default: the cores, at most 8), each with its own cluster (started when its first
# job needs one) and one shared install, at nice 19. A lane starts a job when the scratch estimated for the jobs running (EST_MB,
# JOB_MB) plus its own fits CI_BUDGET_MB (quick 1200, full 2000; the first job always runs), so the run stays at about 1 GB
# however many lanes there are. The scratch directory is $SIFT_CI_TMP (default $TMPDIR/sift-ci-<pid>, /tmp when unset), removed
# when everything passed (KEEP=1 keeps it; a failure keeps it for the logs); a job that passed leaves it at once. The ports are
# SIFT_PORT (default 56130) and +10 per lane. A watchdog stops the run when the directory exceeds CI_MAX_MB (twice the budget; full
# 8192, 512 more per lane above four). SIFT_CI_STATS=1 prints the footprint of the run (peak by component, lane and job, and the
# memory of the postgres processes) and is how EST_MB is kept up to date. SIFT_CI_JOBS, a regex on the job names
# (installcheck-facets, tap-001_crash, tap-shell-store_crash, difftest-mutants, soak, fuzz...), runs just those. Logs:
# <scratch>/logs/<suite>.log, and <job>.log for each job of a split suite. Stages whose sift code has not landed are skipped, not
# failed. Exit 1 if any suite fails.
#
# The last stage, leakcheck, is the leak gate over everything the lanes ran (scripts/leakcheck.sh): the
# server logs of every cluster and node, the sanitizer and LeakSanitizer reports, and the cluster checks
# recorded when each one stopped. leakcheck-selftest first proves that the gate fails on each kind of leak.
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:-quick}
[[ $MODE == quick || $MODE == full ]] || { sed -n '2,26p' "$0"; exit 1; }
renice -n 19 $$ >/dev/null
S=$REPO/scripts
CORES=$(nproc 2>/dev/null || echo 4)
[[ $(pg_config --version) =~ ^PostgreSQL\ ([0-9]+) ]] && PG_MAJOR=${BASH_REMATCH[1]} || PG_MAJOR=0
LANES=${SIFT_LANES:-$(( CORES < 8 ? CORES : 8 ))}
# BUDGET_MB, the scratch the running jobs may need together (estimates below): quick 900, full 2000, CI_BUDGET_MB. The cap, CI_MAX_MB, stops the
# run when the directory really exceeds it: twice the budget (full: 8192 up to four lanes, 512 more for each lane above)
if [[ $MODE == full ]]; then
	export SANITIZE=1; BUDGET_MB=${CI_BUDGET_MB:-2000}; MAX_MB=${CI_MAX_MB:-$(( 8192 + 512 * (LANES > 4 ? LANES - 4 : 0) ))}; unset SIFT_QUICK
else
	export SIFT_QUICK=1; BUDGET_MB=${CI_BUDGET_MB:-1200}; MAX_MB=${CI_MAX_MB:-$(( 2 * BUDGET_MB ))}
fi

# not /dev/shm by choice: the run needs about a gigabyte and fsync is off, so RAM buys nothing
BASE=${SIFT_CI_TMP:-${TMPDIR:-/tmp}/sift-ci-$$}
LOG=$BASE/logs
BASE_PORT=${SIFT_PORT:-56130}
# one initdb per run: every cluster starts as a copy (the shell tests and lane clusters, with 1 MB WAL segments; the
# PostgreSQL::Test::Cluster nodes, which are all WAL tests)
export SIFT_ROOT=$BASE/root SIFT_INITDB_TEMPLATE=$BASE/initdb-shell INITDB_TEMPLATE=$BASE/initdb-perl PG_TEST_PORT_DIR=$BASE/ports
rm -rf "$BASE" && mkdir -p "$LOG" "$BASE/claims" "$BASE/running" "$PG_TEST_PORT_DIR"
t0=$SECONDS

watchdog() { # stops every postmaster under $BASE once the scratch directory passes the cap
	local peak=0 mb
	while sleep 2; do
		mb=$(( $(du -sk "$BASE" 2>/dev/null | cut -f1) / 1024 ))
		(( mb > peak )) && { peak=$mb; echo "$peak" >"$LOG/peak_mb"; }
		if (( mb > MAX_MB )); then
			echo "disk cap exceeded: $mb MB > $MAX_MB MB (CI_MAX_MB)" >"$LOG/cap_exceeded"
			find "$BASE" -name postmaster.pid -exec head -1 {} \; | xargs -r kill -QUIT 2>/dev/null
			return
		fi
	done
}
watchdog &
WATCHDOG=$!
# SIFT_CI_STATS=1: the footprint of the run by component, lane and memory (scripts/ci_stats.sh), printed after the summary
[[ -z ${SIFT_CI_STATS:-} ]] || { "$S/ci_stats.sh" "$BASE" >"$LOG/stats.txt" 2>&1 & STATS=$!; }
cleanup() {
	kill "$WATCHDOG" ${STATS:-} 2>/dev/null
	if [[ -z ${KEEP:-} && ${failed:-1} == 0 ]]; then rm -rf "$BASE"; else echo "scratch kept: $BASE"; fi
}
trap cleanup EXIT

# step ID SUITE CMD...: runs one job or stage with its output in logs/ID.log; appends "id suite status seconds skips lane start end"
# to the results and returns its status
step() {
	local id=$1 suite=$2 t=$SECONDS status=PASS skips
	shift 2
	[[ -e $LOG/cap_exceeded ]] && return
	"$@" >"$LOG/$id.log" 2>&1 || status=FAIL
	skips=$(grep -ciE '\bskip' "$LOG/$id.log" || true)
	printf '%s %s %s %s %s %s %s %s\n' "$id" "$suite" "$status" $((SECONDS - t)) "$skips" "$LANE" "$t" "$SECONDS" >>"$LOG/results"
	[[ $status == PASS ]]
}

# ---------- the jobs ----------
# job ID SUITE CLUSTER CMD...: queued in the order of the calls, so the longest come first (a job queued twice keeps its first
# place). CLUSTER=1 uses the cluster of the lane (the job names its own databases); the others get a directory of their own and
# the port block of the lane.
declare -A J_SUITE J_CLUSTER J_CMD
J_ID=() SUITES=()
job() {
	local id=$1
	[[ -z ${SIFT_CI_JOBS:-} || $id =~ $SIFT_CI_JOBS ]] && [[ ! -v J_SUITE[$id] ]] || return 0   # a job keeps its first place in the queue
	J_ID+=("$id")
	J_SUITE[$id]=$2 J_CLUSTER[$id]=$3
	[[ " ${SUITES[*]-} " == *" $2 "* ]] || SUITES+=("$2")
	shift 3
	printf -v "J_CMD[$id]" '%q ' "$@"
}
named() { local n; for n in "$@"; do echo "$n"; done; }
# rest DIR SUFFIX NAME...: the files test/DIR/*SUFFIX that are not named, so that a new test is queued without editing the lists
rest() {
	local n
	for n in $(basename -a -s "$2" "$REPO/test/$1"/*"$2"); do [[ " ${*:3} " == *" $n "* ]] || echo "$n"; done
}
# The scratch a job needs, in MB: the peak of its lane directory with the cluster of the lane in it (SIFT_CI_STATS=1 lists it per job). The
# jobs below grow the store (the churn of the store tests, WAL of the WAL tests, spill of a large build) and are the only ones above JOB_MB.
declare -A EST_MB=([tap-004_wal_consistency]=690 [tap-shell-store_replica]=400 [tap-002_standby]=380 [installcheck-index_build_postings]=230
	[tap-shell-replication]=200 [tap-shell-store_concurrency]=200 [tap-shell-index_build_crash]=180 [tap-shell-extension_lifecycle]=180
	[tap-shell-store_crash]=170 [tap-001_crash]=150 [installcheck-index_build_thresholds]=140)
JOB_MB=70

tap_perl() { local f; for f in "$@"; do job "tap-$f" tap 0 "$S/tap.sh" "t/$f.pl"; done; }
tap_shell() { local f; for f in "$@"; do job "tap-shell-$f" tap-shell 0 "$S/pgtest.sh" tap "$f"; done; }
installcheck() { local t; for t in "$@"; do job "installcheck-$t" installcheck 1 "$S/pgtest.sh" regress "$t"; done; }

# sessions of a differential test: its parallel stages start 3 workers each and a cluster allows 8, the mutants run on the reference only
DIFF_JOBS=$(( LANES > 1 ? 2 : 1 )) MUTANT_JOBS=$(( LANES > 1 ? 4 : 1 ))
# by measured time, the long jobs of each kind first
SLOW_REGRESS="facets index_build_postings filter_pushdown parallel_scan insert_backpressure parallel_pushdown index_build_thresholds
	edge_documents rank_semi_join analyze_invariant rank_join rank_prune phrase_positions kernels_simd parallel_build"
SLOW_PERL="001_crash 004_wal_consistency"
SLOW_SHELL="leakcheck_gate store_crash replication store_replica index_build_crash store_concurrency"
# shellcheck disable=SC2046,SC2086  # the names are words
if [[ $MODE == full ]]; then
	job soak soak 1 "$S/soak.sh" --duration 600 --check-every 30
	job soak-gin soak-gin 1 "$S/soak.sh" --am gin --duration 120 --rows 20000 --clients 2 --check-every 20
	job difftest-mutants difftest-mutants 1 "$S/difftest.sh" --jobs "$MUTANT_JOBS" --engine mutants --docs 8000 --queries 40 --rounds 2 --minimize 1
	job difftest difftest 1 "$S/difftest.sh" --jobs "$DIFF_JOBS" --docs 5000 --queries 150 --rounds 4
	job difftest-nopos difftest-nopos 1 env "INDEX_OPTS=WITH (positions = off)" "$S/difftest.sh" --jobs "$DIFF_JOBS" --docs 3000 --queries 60 --rounds 2
	job fuzz fuzz 1 "$S/fuzz.sh"
	tap_perl $(named $SLOW_PERL) $(rest t .pl $SLOW_PERL)
	tap_shell $(named $SLOW_SHELL) $(rest t .sh $SLOW_SHELL)
	installcheck $(named $SLOW_REGRESS) $(rest sql .sql $SLOW_REGRESS)
	job isolation isolation 1 "$S/isolation.sh"
	job isolation-emulated isolation-emulated 1 "$S/isolation.sh" --emulate
	job isa-baseline isa-baseline 0 "$S/isa_check.sh"
else
	job difftest difftest 1 "$S/difftest.sh" --jobs "$DIFF_JOBS" --docs 2500 --queries 25 --rounds 2 --minimize 1 --min-trials 10
	# the regression tests that need no test-only function, against the production library: beside the others on PostgreSQL 18,
	# after the pool on 17, where the install is the server's own
	# (the quick tests only: the slow ones and the ones that need the test-only build, TESTING_REGRESS of the Makefile, are not run)
	production_tests() {
		local t p
		for t in $(rest sql .sql $SLOW_REGRESS); do
			for p in $(sed -n 's/^TESTING_REGRESS = //p' "$REPO/Makefile"); do [[ $t == ${p//%/*} ]] && continue 2; done
			echo "$t"
		done
	}
	if (( PG_MAJOR >= 18 )); then job production-regress production-regress 0 "$S/production-check.sh" $(production_tests); else production_last=1; fi
	tap_perl 004_wal_consistency   # the biggest in scratch: it starts while the budget is free
	installcheck facets index_build_postings
	job fuzz fuzz 1 "$S/fuzz.sh" --budget 20
	job difftest-nopos difftest-nopos 1 env "INDEX_OPTS=WITH (positions = off)" "$S/difftest.sh" --jobs "$DIFF_JOBS" \
		--docs 1500 --queries 25 --rounds 2 --minimize 1 --min-trials 10
	job difftest-mutants difftest-mutants 1 "$S/difftest.sh" --jobs "$MUTANT_JOBS" --engine mutants --docs 3000 --queries 10 --rounds 1 --minimize 0 \
		--mutants "float8_score idf_df_plus1 count_plus drop_high_offset drop_sb_boundary tie_ctid_desc nofold_query topk_short"
	tap_shell leakcheck_gate
	tap_perl 001_crash
	installcheck filter_pushdown
	job soak soak 1 "$S/soak.sh" --duration 30 --rows 20000 --rate 400 --clients 2 --check-every 10
	tap_perl $(named $SLOW_PERL)
	tap_shell $(named $SLOW_SHELL)
	installcheck $(named $SLOW_REGRESS)
	tap_perl $(rest t .pl $SLOW_PERL)
	tap_shell $(rest t .sh $SLOW_SHELL)
	installcheck $(rest sql .sql $SLOW_REGRESS)
	job isolation isolation 1 "$S/isolation.sh"
	job isolation-emulated isolation-emulated 1 "$S/isolation.sh" --emulate cic_insert
	job isa-baseline isa-baseline 0 "$S/isa_check.sh"
fi
(( LANES <= ${#J_ID[@]} )) || LANES=${#J_ID[@]}

# tidy ID: the data of a job that passed goes at once, so that the scratch directory holds only what runs: its databases in the
# lane's cluster, or its directory after the leak gate scanned it (a finding keeps the directory for the scan of the whole run)
tidy() {
	if [[ ${J_CLUSTER[$1]} == 1 ]]; then "$S/pgtest.sh" clean >/dev/null 2>&1
	else "$S/leakcheck.sh" scan "$SIFT_TMP/$1" >/dev/null 2>&1 && rm -rf "${SIFT_TMP:?}/$1"; fi
}

# claim: sets CLAIMED to the first job of the queue that nobody has claimed and whose estimate fits the budget with the ones
# running (the first job always fits); returns 1 when the queue is empty, 2 when nothing fits yet
claim() {
	local id est used=0 left=0 f
	flock 9
	for f in "$BASE"/running/*; do [[ ! -e $f ]] || used=$((used + $(<"$f"))); done
	for id in "${J_ID[@]}"; do
		[[ ! -d $BASE/claims/$id ]] || continue
		left=1
		est=${EST_MB[$id]:-$JOB_MB}
		(( used == 0 || used + est <= BUDGET_MB )) || continue
		mkdir "$BASE/claims/$id"
		echo "$est" >"$BASE/running/$id"
		echo "$LANE" >"$BASE/claims/$id/lane"
		CLAIMED=$id
		return 0
	done
	return $(( left ? 2 : 1 ))
} 9>"$BASE/claim.lock"

# lane N: takes the jobs the budget admits, in queue order; its cluster starts with the first job that needs one
lane() {
	local id started='' rc
	LANE=lane$1
	export SIFT_TMP=$BASE/$LANE SIFT_PORT=$((BASE_PORT + 10 * ($1 - 1)))
	mkdir -p "$SIFT_TMP"
	while :; do
		claim && rc=0 || rc=$?
		case $rc in 1) break ;; 2) sleep 0.5; continue ;; esac
		id=$CLAIMED
		if [[ ${J_CLUSTER[$id]} == 1 && -z $started ]]; then started=1; "$S/pgtest.sh" start >"$LOG/$LANE.start.log" 2>&1; fi
		step "$id" "${J_SUITE[$id]}" bash -c '[[ $1 == 1 ]] || { SIFT_TMP=$SIFT_TMP/$2; mkdir -p "$SIFT_TMP"; export SIFT_TMP; }; eval "$3"' \
			_ "${J_CLUSTER[$id]}" "$id" "${J_CMD[$id]}" && tidy "$id"
		rm -f "$BASE/running/$id"
	done
	[[ -z $started ]] || "$S/pgtest.sh" stop
}

# ---------- build, the pool, the leak gate ----------
LANE=main
export SIFT_TMP=$BASE/main SIFT_PORT=$BASE_PORT
mkdir -p "$SIFT_TMP"
step leakcheck-selftest leakcheck-selftest "$S/leakcheck.sh" selftest &
selftest=$!
step portable-core portable-core "$S/check-portable.sh" &
portable=$!
# the second is the node PostgreSQL::Test::Cluster makes, with the environment its test utilities set (untranslated messages)
"$S/pgtest.sh" initdb-template "$SIFT_INITDB_TEMPLATE" || { failed=1; exit 1; }
env -u LANGUAGE -u LC_ALL LC_MESSAGES=C initdb --no-sync -D "$INITDB_TEMPLATE" -A trust >/dev/null || { failed=1; exit 1; }
step build build "$S/pgtest.sh" install || { wait "$selftest" "$portable"; cat "$LOG/build.log"; failed=1; exit 1; }

pids=()
for (( n = 1; n <= LANES; n++ )); do
	( lane "$n" ) &
	pids+=($!)
done
wait "${pids[@]}" "$selftest" "$portable"
if [[ -n ${production_last:-} ]] && [[ -z ${SIFT_CI_JOBS:-} || production-regress =~ $SIFT_CI_JOBS ]]; then
	SUITES+=(production-regress)
	step production-regress production-regress "$S/production-check.sh" $(production_tests)
fi
step leakcheck leakcheck "$S/leakcheck.sh" scan "$BASE"

# ---------- summary ----------
# SUITE: "status seconds skips where" of the jobs of one suite: FAIL when one failed or none ran, the time from the first start
# to the last end, the skip lines summed, the lane (or the number of lanes); then the failed jobs
summarize() {
	local expect=0 id
	for id in "${J_ID[@]}"; do [[ ${J_SUITE[$id]} == "$1" ]] && expect=$((expect + 1)); done
	awk -v s="$1" -v expect="$expect" '$2 == s { n++; sk += $5; if ($3 != "PASS") bad = bad " " $1
			if (!($6 in seen)) { seen[$6] = 1; nl++; lane = $6 }
			if (lo == "" || $7 < lo) lo = $7; if ($8 > hi) hi = $8 }
		END { printf "%s %d %d %s%s\n", (bad != "" || n == 0 || n < expect ? "FAIL" : "PASS"), hi - lo, sk, (nl > 1 ? nl "_lanes" : lane), bad }' "$LOG/results"
}
echo
green=$'\e[32m' red=$'\e[31m' off=$'\e[0m'
[[ -t 1 ]] || green= red= off=
failed=0
# the suites in the order of the report (a suite not listed follows the others)
ORDER=(build leakcheck-selftest portable-core)
for s in isa-baseline installcheck isolation-emulated isolation fuzz difftest-mutants soak soak-gin difftest difftest-nopos tap-shell tap "${SUITES[@]}"; do
	[[ " ${SUITES[*]} " != *" $s "* || " ${ORDER[*]} " == *" $s "* ]] || ORDER+=("$s")
done
for s in "${ORDER[@]}" leakcheck; do
	read -r status secs skips where bad <<<"$(summarize "$s")"
	line=$(printf '%s %-20s %5ss  %s skip lines  (%s)' "$status" "$s" "$secs" "$skips" "${where/_/ }")
	if [[ $status == PASS ]]; then echo "$green$line$off"; else failed=1; echo "$red$line$off"; fi
	# the log of a split suite: the logs of its jobs, in order
	[[ -f $LOG/$s.log ]] || for id in "${J_ID[@]}"; do [[ ${J_SUITE[$id]} == "$s" ]] && { echo "== $id"; cat "$LOG/$id.log"; }; done >"$LOG/$s.log"
	[[ -z ${bad:-} ]] || echo "$red  failed:$bad$off"
done
[[ ! -e $LOG/cap_exceeded ]] || { failed=1; echo "$red$(cat "$LOG/cap_exceeded")$off"; }
[[ -z ${STATS:-} ]] || { kill "$STATS"; wait "$STATS"; cat "$LOG/stats.txt"; }
printf 'wall %ss (%s lanes), peak scratch %s MB, logs %s\n' "$((SECONDS - t0))" "$LANES" "$(cat "$LOG/peak_mb" 2>/dev/null || echo '<2')" "$LOG"
exit $failed
