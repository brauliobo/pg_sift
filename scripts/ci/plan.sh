# shellcheck shell=bash
# The plan of the quick tier: which jobs are queued, in the order of the pool, and which run after it.
# Edit here to add a job; --plan prints it and the selftest checks it. Sourced by scripts/ci.sh; not a script of its own.
# sessions of a differential test: its parallel stages start 3 workers each and a cluster allows 8, the mutants run on the reference only
DIFF_JOBS=$(( LANES > 1 ? 2 : 1 )) MUTANT_JOBS=$(( LANES > 1 ? 4 : 1 ))
# by measured time, the long jobs of each kind first (SLOW_REGRESS: scripts/ci/weights.sh)
SLOW_PERL="bm25/crash bm25/wal_consistency"
SLOW_SHELL="agg/viability_gate core/leakcheck_gate bm25/store/crash core/replication bm25/store/replica bm25/index/build_crash bm25/store/concurrency col/wbuf/crash"
# the memory slope gate of the quick tier in SLOPE_SHARDS jobs, each the cases whose crc32 of the name falls in its shard (the slope of a case does not depend on the others)
SLOPE_SHARDS=3
# the fault matrix of the quick tier is a smoke: the quick rows of the operations that publish the most (FAULT_SMOKE, a regex of the rows of test/t/fault/matrix.tsv) and the standby
# matrix of four of them (two of Format 2). The whole matrix, on every operation and with a standby on every family, runs on demand: a dispatch with only='^tap-shell-fault/matrix$' (SIFT_CI_JOBS names it below)
FAULT_SMOKE='^(col_build|col_promote|vacuum|tbl_seal)\.|^(bm25_promote|scope_promote|col_cic|col_merge|reindex_col_c|tbl_copy)\.(b1\.terminate|ak\.immediate)$|^f2_[a-z_]+\.gate-f2[a-z]+\.terminate$' FAULT_SMOKE_STANDBY='^(col_promote|vacuum|f2_compact|f2_col_delta)$'
# a Format 1.0 run has no row of the Format 2.0 fault matrix (the rows f2_*)
[[ $FORMAT == 2 ]] || { FAULT_SMOKE=${FAULT_SMOKE%|^f2_*}; FAULT_SMOKE_STANDBY='^(col_promote|vacuum)$'; }
fault_smoke() {
	job tap-shell-fault/matrix-smoke tap-shell 0 env "SIFT_FAULT_ONLY=$FAULT_SMOKE" "$S/pgtest.sh" tap fault/matrix
	job tap-shell-fault/matrix_standby-smoke tap-shell 0 env "SIFT_FAULT_ONLY=$FAULT_SMOKE_STANDBY" "$S/pgtest.sh" tap fault/matrix_standby
}
slope_shards() { local k; for (( k = 1; k <= SLOPE_SHARDS; k++ )); do job "tap-shell-mem/slope-$k" tap-shell 0 env "MEM_SLOPE_SHARD=$k/$SLOPE_SHARDS" "$S/pgtest.sh" tap mem/slope; done; }
# scripts/fast-gate.sh on each library: the release library the user installs (the staged one, PostgreSQL 18), Format 1 beside the default 2.0 (the compat-f1 lane: SIFT_CI_JOBS=$COMPAT_F1_JOBS
# also runs the upgrade tests of the v5 formats), and Format 2.0 on a PostgreSQL with assertions (the cassert shards and SIFT_CI_JOBS=assert)
fast_gate_jobs() {
	(( PG_MAJOR >= 18 )) || return 0
	[[ -z ${SIFT_PREBUILT_RELEASE:-} ]] || job fast-gate-release fast-gate 0 "$S/fast-gate.sh" --format release --root "$SIFT_PREBUILT_RELEASE"
	job compat-f1-fast-gate fast-gate 0 "$S/fast-gate.sh" --format f1
	[[ -z ${SIFT_CI_ASSERT:-} && ${SIFT_CI_JOBS:-} != *assert* ]] || job fast-gate-assert fast-gate 0 "$S/fast-gate.sh" --format f2 --assert
}
# the black-box canaries of scripts/perf/gate.py on the staged release library, a job for each area of scripts/areas.tsv (a change runs the jobs of the areas it is mapped to, the
# core and the build run the smoke set). They need PostgreSQL 18 (extension_control_path) and the staged release library: a local run has none and queues none, `scripts/perf-select.py --run gate` runs them there
perf_gate_jobs() {
	local area
	(( PG_MAJOR >= 18 )) && [[ -n ${SIFT_PREBUILT_RELEASE:-} ]] || return 0
	for area in star clickbench coexist jsonb table bm25 gpu; do job perf-gate-$area perf-gate 0 python3 -I "$S/perf/gate.py" --area $area --root "$SIFT_PREBUILT_RELEASE"; done
	job perf-gate-smoke perf-gate 0 python3 -I "$S/perf/gate.py" --smoke --root "$SIFT_PREBUILT_RELEASE"
}
# shellcheck disable=SC2046,SC2086  # the names are words
# A job that exercises no code of its own major (the engine of the corpus, the fuzz harnesses, the hardened build of the decoders) runs on PostgreSQL 18 only; the
# builds for arm64 and for an older or newer major run the regression, TAP and pushdown differential tests
once_on_18() { (( PG_MAJOR == 18 )); }
# 2500 documents: past 2200 the corpus crosses a superblock boundary (the drop_sb_boundary mutant survives below it); 12 queries and 2 rounds still
# visit every stage, the write buffers, the promotion and the merge
once_on_18 && job difftest difftest 1 "$S/difftest.sh" --jobs "$DIFF_JOBS" --docs 2500 --queries 12 --rounds 2 --minimize 1 --min-trials 10
# the regression tests that need no test-only function, against the release library (SIFT_TESTING off). The quick tests only: the slow ones and the ones that need
# the test-only build (TESTING_REGRESS of the Makefile; scripts/check-testing-regress.sh keeps a test that calls a sift._test_ function in it) are not run.
# A test gives the same answers on both builds unless it refers to a test-only name (a sift.test_ setting, a sift._test_ function, pg_sift_test) behind a guard, or forces a
# parallel build (max_parallel_maintenance_workers above 0): the release library builds some indexes in one process where the test library keeps the participants
# (build_pays in src/col/v1/build_par.c), so a test that counts the segments of that build says so only on the release library. Those run, with a smoke of STOCK_SMOKE, one test
# or more of each area, which loads and uses the library users get. The deep tier on 18 runs all of them (about 350 tests,
# 760 to 1170 s on one lane in run 40193282663, against about 25 tests); on 17 the smoke is enough
release_library_tests() {
	local t p f pats globs=()
	local -A guarded=() named=()   # guarded: the tests that name a test-only object or force a parallel build, found in one grep; named: the names of the Makefile that are no pattern
	read -ra pats < <(sed -En 's/^TESTING_REGRESS \+?= //p' "$REPO/Makefile" | paste -sd' ' -)
	for p in "${pats[@]}"; do if [[ $p == *%* ]]; then globs+=("${p//%/*}"); else named[$p]=1; fi; done   # one regular expression for all of them is slower to match (4 s of CPU for the plan instead of 0.7)
	while IFS= read -r f; do f=${f#"$REPO/test/sql/"}; guarded[${f%.sql}]=1; done < <(grep -rlE --include='*.sql' 'sift\.test_|sift\._test_|pg_sift_test|max_parallel_maintenance_workers (=|TO) [1-9]' "$REPO/test/sql")
	for t in $(rest sql .sql $SLOW_REGRESS); do
		[[ ! -v named[$t] ]] || continue
		for p in "${globs[@]}"; do [[ $t == $p ]] && continue 2; done
		[[ " $STOCK_SMOKE " == *" $t "* || -v guarded[$t] ]] || continue
		echo "$t"
	done
}
# one job of every other test: the staged release root (scripts/ci-build.sh) is installed, not built, so the run is the tests alone
flavor_job release-library-regress release-library-regress "$S/release-library-check.sh" $(release_library_tests)
tap_perl bm25/wal_consistency   # the biggest in scratch: it starts while the budget is free
installcheck bm25/facets bm25/index/build_postings
# one pass (--budget 1) of every target: its seeds and the inputs of the fixed findings are the same, the random and exhaustive loops shorter
once_on_18 && job fuzz fuzz 1 env FZ_SECS=2 FZ_GQUANT_N=10 FZ_KERNELS_MILLIONS=0.05 FZ_GRID_STRIDE=7 FZ_CODEC_ITERS=100 FZ_ANALYZE_ITERS=5000 "$S/fuzz.sh" --budget 1 ${SIFT_FUZZ_TARGETS:-}
once_on_18 && flavor_job hardened hardened "$S/hardened.sh" --quick
once_on_18 && job difftest-nopos difftest-nopos 1 env "INDEX_OPTS=WITH (positions = off)" "$S/difftest.sh" --jobs "$DIFF_JOBS" \
	--docs 1000 --queries 12 --rounds 2 --minimize 1 --min-trials 10
# the mutants are checked on the built corpus (round 0): all 8 die there, and the rounds with DML, promotion, merge and REINDEX without a bug are the job difftest
once_on_18 && job difftest-mutants difftest-mutants 1 "$S/difftest.sh" --jobs "$MUTANT_JOBS" --engine mutants --docs 3000 --queries 10 --rounds 0 --minimize 0 \
	--mutants "float8_score idf_df_plus1 count_plus drop_high_offset drop_sb_boundary tie_ctid_desc nofold_query topk_short"
job difftest-col difftest-col 1 "$S/coldiff.sh" --fixture-rows 1500 --rows 3000 --variants on,on-par --scenarios dml,abort,vacuum,held-lite --scenario-slices 3
job difftest-col-tbl difftest-col-tbl 1 "$S/coldiff.sh" --table --suite fixtures --fixture-rows 1500 --variants on,on-par --scenarios dml,abort,vacuum --scenario-slices 3
job difftest-col-selftest difftest-col-selftest 1 "$S/coldiff.sh" --selftest --fixture-rows 1200
# each lane at 30000 rows, the size whose bands the plan and the ratios are recorded at. The second pass at 250000 rows (the bands of the ratio that falls with the rows) and the ranking at 250,000 rows
# (6 minutes a lane, and a skip without perf) are not run in CI: scripts/plangate.sh --rows 250000 runs them by hand
job coverage coverage 1 "$S/coverage.sh" --check
job plan-gate plan-gate 1 "$S/plangate.sh" --runs 0
job plan-gate-tuned plan-gate-tuned 1 "$S/plangate.sh" --lane tuned --runs 0
job plan-gate-table plan-gate-table 1 "$S/plangate.sh" --table --runs 0
job plan-gate-table-tuned plan-gate-table-tuned 1 "$S/plangate.sh" --table --lane tuned --runs 0
job plan-gate-4t plan-gate-4t 1 "$S/plangate.sh" --lane stock-4t --runs 0
perf_gate_jobs
fast_gate_jobs
# two seeds of each fuzzer (and the seeds of its fixed findings: 18 of txn_fuzz, 9, 10 and 34 of its shapes); the ASan run (SANITIZE=1, assert.yml) runs one seed of each
fz_seeds=1-2 fz_queries=100 txn_seeds=1-2,18
[[ -z ${SANITIZE:-} ]] || fz_seeds=1 fz_queries=40 txn_seeds=1
once_on_18 && job difftest-fuzz difftest-fuzz 1 python3 "$REPO/test/fuzz/diff/cli.py" --seeds $fz_seeds --queries $fz_queries --rows 3000 --leader-on
once_on_18 && job difftest-fuzz-tbl difftest-fuzz 1 python3 "$REPO/test/fuzz/diff/cli.py" --table --seeds $fz_seeds --queries $fz_queries --rows 3000 --leader-on
once_on_18 && job difftest-fuzz-having difftest-fuzz 1 python3 "$REPO/test/fuzz/diff/cli.py" --families having --seeds 1-2 --queries 40 --rows 3000 --leader-on
job difftest-txn difftest-txn 1 python3 "$REPO/test/fuzz/txn/cli.py" --seeds $txn_seeds --stop-after 2
job difftest-txn-shapes difftest-txn 1 python3 "$REPO/test/fuzz/txn/cli.py" --shapes --seeds 1-2,9,10,34 --stop-after 2
job difftest-bm25 difftest-bm25 1 python3 "$REPO/test/fuzz/bm25.py" --seeds 1-2 --queries 30 --stop-after 2
job shapes-matrix shapes-matrix 1 python3 "$REPO/test/fuzz/shapes_matrix.py" --tier quick
job shapes-surface shapes-surface 1 python3 "$REPO/test/fuzz/surface_sweep.py" --tier quick
tap_shell core/leakcheck_gate
tap_perl bm25/crash
installcheck bm25/filter/pushdown
job soak soak 1 "$S/soak.sh" --duration 15 --rows 20000 --rate 400 --clients 2 --check-every 5
job soak-col soak-col 1 "$S/colsoak.sh" --duration 18 --clients 6
tap_perl $(named $SLOW_PERL)
tap_shell $(named $SLOW_SHELL)
installcheck $(named $SLOW_REGRESS)
tap_perl $(rest t .pl $SLOW_PERL)
# the scaling gate (a build of the 8-page flavour of the library and 50 s of timings) is a job of the quick tier only when the run was asked for it by the diff: scripts/areas.tsv names its files
if [[ -n ${SIFT_CI_AFFECTED:-} && tap-shell-core/scaling_slope =~ $SIFT_CI_AFFECTED ]]; then tap_shell $(rest t .sh $SLOW_SHELL mem/slope mem/slope_legacy fault/matrix fault/matrix_standby); else tap_shell $(rest t .sh $SLOW_SHELL mem/slope mem/slope_legacy fault/matrix fault/matrix_standby core/scaling_slope); fi
slope_shards
fault_smoke
installcheck_batches $(rest sql .sql $SLOW_REGRESS)
isolation_slices
job isolation-emulated isolation-emulated 1 "$S/isolation.sh" --emulate bm25/cic_insert
build_checks
job regress-batch regress-batch 0 "$S/test-regress-batch.sh"
# opt-in lanes on a PostgreSQL built from source (scripts/pg-assert.sh, cached outside the repo; docs/SAFETY.md): queued only when
# SIFT_CI_JOBS names them, e.g. `SIFT_CI_JOBS=assert scripts/ci.sh quick`
[[ ${SIFT_CI_JOBS:-} != *assert* ]] || job assert assert 0 "$S/assert.sh"
[[ ${SIFT_CI_JOBS:-} != *valgrind* ]] || job valgrind valgrind 0 "$S/valgrind.sh"
[[ ${SIFT_CI_JOBS:-} != *massif* ]] || job massif massif 0 "$S/valgrind.sh" --massif
# the leak gate: the curated tests under memcheck with leak checking and the reviewed allow-list, then the same tests on the ASan and UBSan build; SIFT_CI_LEAKS=1 adds it to quick
[[ ${SIFT_CI_LEAKS:-} != 1 && ${SIFT_CI_JOBS:-} != *valgrind-leaks* ]] || job valgrind-leaks valgrind 0 "$S/valgrind.sh" --leaks
[[ ${SIFT_CI_LEAKS:-} != 1 && ${SIFT_CI_JOBS:-} != *asan-leaks* ]] || flavor_job asan-leaks asan "$S/leaks-asan.sh"
# the whole fault matrix, the operations and the standby of every family (hundreds of cases): queued only when SIFT_CI_JOBS names it
[[ ${SIFT_CI_JOBS:-} != *fault/matrix* ]] || tap_shell fault/matrix fault/matrix_standby
