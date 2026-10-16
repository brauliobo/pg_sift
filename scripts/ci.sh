#!/usr/bin/env bash
# Single entry point for the test suites, with a pass/fail/skip summary.
#
#   scripts/ci.sh [quick|full]        (make check | make check-full)
#
# quick (default, a few minutes): non-ASan build, small corpora and short runs. installcheck,
#   isolation (emulated and real), the Perl and shell TAP tests (sift only, fewer crash
#   iterations), a 3 000 document differential test and 8 of the 16 mutants of the harness (the
#   fast-to-detect ones), a 30 s soak and a 20 s fuzz budget.
# full: ASan build and clusters, installcheck, the differential test at 5 000 documents, all 16
#   mutants, every isolation and TAP test (GIN control included), all fuzz targets, GIN-control
#   and sift soaks (600 s) and the server sanitizer report.
# SANITIZE=1 makes quick run on the ASan build too.
#
# The suites run in four lanes in parallel (one cluster per lane, one shared install), at nice 19,
# in a scratch directory $SIFT_CI_TMP (default /tmp/sift-ci-<pid>) that is removed when everything
# passed (KEEP=1 keeps it; a failure keeps it for the logs). The ports are SIFT_PORT (default
# 56130) and +10 per lane. A watchdog stops the run when the directory exceeds CI_MAX_MB (quick
# 4096, full 8192). Logs: <scratch>/logs/<suite>.log. Stages whose sift code has not landed are
# skipped, not failed. Exit 1 if any suite fails.
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:-quick}
[[ $MODE == quick || $MODE == full ]] || { sed -n '2,20p' "$0"; exit 1; }
renice -n 19 $$ >/dev/null
S=$REPO/scripts
BASE=${SIFT_CI_TMP:-/tmp/sift-ci-$$}
LOG=$BASE/logs
BASE_PORT=${SIFT_PORT:-56130}
if [[ $MODE == full ]]; then
	export SANITIZE=1; MAX_MB=${CI_MAX_MB:-8192}; unset SIFT_QUICK
else
	export SIFT_QUICK=1; MAX_MB=${CI_MAX_MB:-4096}
fi
export SIFT_ROOT=$BASE/root
rm -rf "$BASE" && mkdir -p "$LOG"
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
cleanup() {
	kill "$WATCHDOG" 2>/dev/null
	if [[ -z ${KEEP:-} && ${failed:-1} == 0 ]]; then rm -rf "$BASE"; else echo "scratch kept: $BASE"; fi
}
trap cleanup EXIT

# suite NAME CMD...: runs one suite, appends "lane name status seconds skips" to the lane's result file
suite() {
	local name=$1 t=$SECONDS status skips
	shift
	[[ -e $LOG/cap_exceeded ]] && return
	if "$@" >"$LOG/$name.log" 2>&1; then status=PASS; else status=FAIL; fi
	skips=$(grep -ciE '\bskip' "$LOG/$name.log" || true)
	printf '%s %s %s %s\n' "$name" "$status" $((SECONDS - t)) "$skips" >>"$LOG/$LANE.result"
}

# lane N CLUSTER SUITES: own SIFT_TMP and port; CLUSTER=1 starts a server for the suites that need one
lane() {
	LANE=lane$1
	export SIFT_TMP=$BASE/$LANE SIFT_PORT=$((BASE_PORT + 10 * ($1 - 1)))
	mkdir -p "$SIFT_TMP"
	[[ $2 == 1 ]] && "$S/pgtest.sh" start >"$LOG/$LANE.start.log" 2>&1
	"$3"
	if [[ $2 == 1 ]]; then
		[[ $MODE == full ]] && suite "sanitizer-$LANE" "$S/pgtest.sh" sanitizer-report
		"$S/pgtest.sh" stop
	fi
}

if [[ $MODE == full ]]; then
	core() {
		suite isa-baseline "$S/isa_check.sh"
		suite installcheck "$S/pgtest.sh" regress
		suite isolation-emulated "$S/isolation.sh" --emulate
		suite isolation "$S/isolation.sh"
		suite fuzz "$S/fuzz.sh"
	}
	mutants() { suite difftest-mutants "$S/difftest.sh" --engine mutants --docs 8000 --queries 40 --rounds 2 --minimize 1; }
	diff_gin() {
		suite difftest "$S/difftest.sh" --docs 5000 --queries 150 --rounds 4
		INDEX_OPTS="WITH (positions = off)" suite difftest-nopos "$S/difftest.sh" --docs 3000 --queries 60 --rounds 2
		suite soak-gin "$S/soak.sh" --am gin --duration 120 --rows 20000 --clients 2 --check-every 20
	}
	soak_shell() {
		suite soak "$S/soak.sh" --duration 600 --check-every 30
		suite tap-shell "$S/pgtest.sh" tap
	}
	core_tap() { core; suite tap "$S/tap.sh"; }
	# balanced by measured time: 1 mutants (SQL reference only), 2 difftest and GIN soak, 3 soak and shell TAP, 4 the rest
	LANES=("1 1 mutants" "2 1 diff_gin" "3 1 soak_shell" "4 1 core_tap")
else
	core() {
		suite isa-baseline "$S/isa_check.sh"
		suite installcheck "$S/pgtest.sh" regress
		suite isolation-emulated "$S/isolation.sh" --emulate cic_insert
		suite isolation "$S/isolation.sh"
		suite fuzz "$S/fuzz.sh" --budget 20
	}
	mutants_soak() {
		suite difftest-mutants "$S/difftest.sh" --engine mutants --docs 3000 --queries 10 --rounds 1 --minimize 0 \
			--mutants "float8_score idf_df_plus1 count_plus drop_high_offset drop_sb_boundary tie_ctid_desc nofold_query topk_short"
		suite soak "$S/soak.sh" --duration 30 --rows 20000 --rate 400 --clients 2 --check-every 10
	}
	diff_shell() {
		suite difftest "$S/difftest.sh" --docs 2500 --queries 25 --rounds 2 --minimize 1 --min-trials 10
		INDEX_OPTS="WITH (positions = off)" suite difftest-nopos "$S/difftest.sh" --docs 1500 --queries 25 --rounds 2 --minimize 1 --min-trials 10
		suite tap-shell "$S/pgtest.sh" tap
	}
	tap_perl() { suite tap "$S/tap.sh"; }
	LANES=("1 1 core" "2 1 mutants_soak" "3 1 diff_shell" "4 0 tap_perl")
fi

LANE=main
export SIFT_TMP=$BASE/main SIFT_PORT=$BASE_PORT
mkdir -p "$SIFT_TMP"
suite build "$S/pgtest.sh" install
grep -q PASS "$LOG/main.result" || { cat "$LOG/build.log"; failed=1; exit 1; }

pids=()
for spec in "${LANES[@]}"; do
	read -r n cluster fn <<<"$spec"
	( lane "$n" "$cluster" "$fn" ) &
	pids+=($!)
done
wait "${pids[@]}"

echo
green=$'\e[32m' red=$'\e[31m' off=$'\e[0m'
[[ -t 1 ]] || green= red= off=
failed=0
for f in "$LOG"/main.result "$LOG"/lane*.result; do
	[[ -f $f ]] || continue
	while read -r name status secs skips; do
		line=$(printf '%s %-20s %5ss  %s skip lines  (%s)' "$status" "$name" "$secs" "$skips" "$(basename "$f" .result)")
		[[ $status == PASS ]] && echo "$green$line$off" || { failed=1; echo "$red$line$off"; }
	done <"$f"
done
[[ ! -e $LOG/cap_exceeded ]] || { failed=1; echo "$red$(cat "$LOG/cap_exceeded")$off"; }
printf 'wall %ss (%s lanes), peak scratch %s MB, logs %s\n' "$((SECONDS - t0))" "${#LANES[@]}" "$(cat "$LOG/peak_mb" 2>/dev/null || echo '<2')" "$LOG"
exit $failed
