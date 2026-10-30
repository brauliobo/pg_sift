# The run itself: the build, the harness checks of shard 1, the lanes of the pool, the jobs after it, and the leak gate.
# Sourced by scripts/ci.sh; not a script of its own.
# ---------- build, the pool, the leak gate ----------
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
step build build "$S/pgtest.sh" install || { wait ${lint:-} ${spantest:-}; cat "$LOG/build.log"; failed=1; exit 1; }
step simd-level simd-level "$S/simd_level.sh" || cat "$LOG/simd-level.log"

pids=()
for (( n = 1; n <= LANES; n++ )); do
	( lane "$n" ) &
	pids+=($!)
done
wait "${pids[@]}" ${lint:-} ${spantest:-}
# the jobs that install a flavor of the server's own library (flavor_job) run alone, now that no worker of a lane is left to load it
for id in "${J_ID[@]}"; do [[ -v J_AFTER[$id] ]] && run_job "$id" && tidy "$id"; done
step leakcheck leakcheck "$S/leakcheck.sh" scan "$BASE"
