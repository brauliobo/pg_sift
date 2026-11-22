# shellcheck shell=bash
# The run itself: the build, the harness checks of shard 1, the lanes of the pool, the jobs after it, and the leak gate.
# Sourced by scripts/ci.sh; not a script of its own.
# ---------- build, the pool, the leak gate ----------
# SIFT_MEMCENSUS (src/core/memcensus.c): the servers of every lane and TAP node write SIFTMEM lines, leakcheck.sh harvests them into SIFT_MEMCENSUS_DIR and the last step ranks them
[[ -z ${SIFT_MEMCENSUS:-} ]] || export SIFT_MEMCENSUS_DIR=${SIFT_MEMCENSUS_DIR:-$BASE/memcensus} SIFT_MEMCENSUS_LABEL=${SIFT_MEMCENSUS_LABEL:-ci}
LANE=main
export SIFT_TMP=$BASE/main SIFT_PORT=$BASE_PORT
mkdir -p "$SIFT_TMP"
HARNESS=(lint span-test)
if (( SHARD == 1 )) && [[ -z ${SIFT_CI_NO_HARNESS:-} ]]; then
	step lint lint "$S/lint.sh" &
	lint=$!
	step span-test span-test bash -c 'make -s -C "$0/sqlite" B="$SIFT_TMP/span" span-test span-test-asan' "$REPO" &
	spantest=$!
else HARNESS=(); fi
# the second is the node PostgreSQL::Test::Cluster makes, with the environment its test utilities set (untranslated messages)
"$S/pgtest.sh" initdb-template "$SIFT_INITDB_TEMPLATE" || { failed=1; exit 1; }
env -u LANGUAGE -u LC_ALL LC_MESSAGES=C initdb --no-sync -D "$INITDB_TEMPLATE" -A trust >/dev/null || { failed=1; exit 1; }
step build build env ${PREBUILT:+SIFT_PREBUILT_ROOT="$PREBUILT"} "$S/pgtest.sh" install || { cat "$LOG/build.log"; failed=1; exit 1; }
step simd-level simd-level "$S/simd_level.sh" || cat "$LOG/simd-level.log"
# the 8-page flavour (scripts/scaling-gate.sh) is a second full build of the library, 1.1 trillion instructions: built once here, not inside the first test that asks for it and its time budget
if (( PG_MAJOR >= 18 )); then
	for f in $(grep -l 'scaling-gate\.sh' "$REPO"/test/t/*.sh); do
		[[ " ${J_ID[*]} " == *" tap-shell-$(basename "$f" .sh) "* ]] || continue
		step sb-flavour sb-flavour "$S/scaling-gate.sh" build || { cat "$LOG/sb-flavour.log"; failed=1; exit 1; }
		break
	done
fi

pids=()
for (( n = 1; n <= LANES; n++ )); do
	( lane "$n" ) &
	pids+=($!)
done
wait "${pids[@]}" ${lint:-} ${spantest:-}
# the jobs that install a flavor of the server's own library (flavor_job) run alone, now that no worker of a lane is left to load it
for id in "${J_ID[@]}"; do [[ -v J_AFTER[$id] ]] || continue; if run_job "$id"; then tidy "$id"; else shed "$id"; fi; done
step leakcheck leakcheck "$S/leakcheck.sh" scan "$BASE"
# reports and uploads, never fails the run: the memory slope tests are the gates
[[ -z ${SIFT_MEMCENSUS:-} ]] || step mem-report mem-report bash -c 'python3 -I "$0/mem-report.py" report "$1" ${SIFT_MEMREPORT_BASELINE:+--baseline "$SIFT_MEMREPORT_BASELINE"} --out "$1/report.md" --json "$1/report.json" || true' "$S" "$SIFT_MEMCENSUS_DIR"
