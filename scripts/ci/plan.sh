# The plan of the quick and the full tier: which jobs are queued, in the order of the pool, and which run after it.
# Edit here to add a job; --plan prints it and the selftest checks it. Sourced by scripts/ci.sh; not a script of its own.
# sessions of a differential test: its parallel stages start 3 workers each and a cluster allows 8, the mutants run on the reference only
DIFF_JOBS=$(( LANES > 1 ? 2 : 1 )) MUTANT_JOBS=$(( LANES > 1 ? 4 : 1 ))
# by measured time, the long jobs of each kind first
SLOW_REGRESS="tbl_bigtx tbl_writepath build_large_docs index_build_postings filter_pushdown facets parallel_scan insert_backpressure parallel_pushdown index_build_thresholds
	edge_documents rank_semi_join analyze_invariant rank_join rank_prune phrase_positions kernels_simd col_kernels_simd col_enc parallel_build"
SLOW_PERL="001_crash 004_wal_consistency"
SLOW_SHELL="leakcheck_gate store_crash replication store_replica index_build_crash store_concurrency col_wbuf_crash"
# shellcheck disable=SC2046,SC2086  # the names are words
if [[ $MODE == full ]]; then
	# The full tier has two sizes. The lean one (every dispatch and the run that is the evidence of a release) fits a job in 45 minutes and the run in 75 on the hosted
	# runners: the same jobs, the same checks, shorter loops, a smaller corpus, fixed seeds. The deep one (SIFT_CI_DEEP=1: the nightly run, or a dispatch with deep) has the
	# sizes of before: lean_or_deep LEAN DEEP
	lean_or_deep() { [[ -n ${SIFT_CI_DEEP:-} ]] && echo "$2" || echo "$1"; }
	# A job that exercises no code of its own major (the engine of the corpus, the fuzz harnesses, the control index of the soak, the hardened build of the decoders) runs
	# on PostgreSQL 18 in the lean tier: the quick tier runs its small form on every major, and what depends on the major (the regression tests, the TAP tests, the plan gate,
	# the pushdown differential tests, the soaks of the server) runs on both
	once_on_18() { (( PG_MAJOR >= 18 )) || [[ -n ${SIFT_CI_DEEP:-} ]]; }
	job soak soak 1 "$S/soak.sh" --duration "$(lean_or_deep 180 600)" --check-every 30
	once_on_18 && job soak-gin soak-gin 1 "$S/soak.sh" --am gin --duration "$(lean_or_deep 60 120)" --rows 20000 --clients 2 --check-every 20
	job soak-col soak-col 1 "$S/colsoak.sh" --duration "$(lean_or_deep 120 300)"
	# The differential families of the corpus run on PostgreSQL 18 only (they are the half of the lane seconds of its tier, and the quick tier runs them on every major).
	# The deep tier cuts them so that no job passes 25 minutes: the mutants alternate between three jobs, four seeds of the corpus run one round each
	if [[ -z ${SIFT_CI_DEEP:-} ]]; then
		once_on_18 && {
			job difftest-mutants difftest-mutants 1 "$S/difftest.sh" --jobs "$MUTANT_JOBS" --engine mutants --docs 4000 --queries 40 --rounds 1 --minimize 1
			job difftest difftest 1 "$S/difftest.sh" --jobs "$DIFF_JOBS" --docs 4000 --queries 100 --rounds 3
			job difftest-nopos difftest-nopos 1 env "INDEX_OPTS=WITH (positions = off)" "$S/difftest.sh" --jobs "$DIFF_JOBS" --docs 2500 --queries 40 --rounds 1
		}
	elif (( PG_MAJOR >= 18 )); then
		for k in 1 2 3; do
			job difftest-mutants-$k difftest-mutants 1 "$S/difftest.sh" --jobs "$MUTANT_JOBS" --engine mutants --mutant-slice $k/3 --docs 8000 --queries 40 --rounds 2 --minimize 1
		done
		for k in 1 2 3 4; do
			job difftest-$k difftest 1 "$S/difftest.sh" --jobs "$DIFF_JOBS" --seed $k --docs 5000 --queries 100 --rounds 1
		done
		job difftest-nopos difftest-nopos 1 env "INDEX_OPTS=WITH (positions = off)" "$S/difftest.sh" --jobs "$DIFF_JOBS" --docs 3000 --queries 60 --rounds 2
	fi
	# the shape tags and 0 DIFFER hold the same at 1500 fixture rows and 20000 analytics rows as at 4000 and 100000 (395 s against 483 s)
	# the two differential tests of the column index take 160 minutes of one lane each on PostgreSQL 18, and more on 17, longer than the rest of the tier together
	# in four lanes: they are cut into COL_SLICES disjoint slices of the corpus (coldiff.sh --slice K/N) that the lanes and the shards take like any other job. Every
	# slice runs every variant and scenario on its share of the queries (--selftest proves the slices are the whole corpus, each query once)
	# Lean: each scenario runs a third of the scenario queries (the slice moves from one heap state to the next, so every query meets several states and every state
	# meets queries of every kind), and each query runs on one major: slices 1 and 2 of 4 before PostgreSQL 18, 3 and 4 on 18 (the corpus is the same on every major, the
	# quick tier runs all of it on each with two variants). The deep tier runs the first half on 17 and all of it on 18
	slices=$COL_SLICES; (( PG_MAJOR >= 18 )) || [[ -z ${SIFT_CI_DEEP:-} ]] || slices=$COL_SLICES_DEEP17
	first=1 last=$slices
	if (( PG_MAJOR < 18 )); then last=$(( slices / 2 )); elif [[ -z ${SIFT_CI_DEEP:-} ]]; then first=$(( slices / 2 + 1 )); fi
	for (( k = first; k <= last; k++ )); do
		job difftest-col-$k difftest-col 1 "$S/coldiff.sh" --fail-fast --suite fixtures --slice $k/$slices --fixture-rows 1500 --scenario-slices "$(lean_or_deep 3 1)"
		job difftest-col-tbl-$k difftest-col-tbl 1 "$S/coldiff.sh" --fail-fast --table --suite fixtures --slice $k/$slices --fixture-rows 1500 --scenario-slices "$(lean_or_deep 3 1)"
	done
	job difftest-col-analytics difftest-col 1 "$S/coldiff.sh" --fail-fast --suite analytics --rows 20000
	once_on_18 && job difftest-col-selftest difftest-col-selftest 1 "$S/coldiff.sh" --selftest
	# the plan the planner picks, the cost ratios and the results of 96 shapes at 50000 rows; no timings (--runs 0): a timing that loses only warns on the hosted runners, which are noisy
	job plan-gate plan-gate 1 "$S/plangate.sh" --rows 50000 --runs 0
	job plan-gate-tuned plan-gate-tuned 1 "$S/plangate.sh" --lane tuned --rows 50000 --runs 0
	job plan-gate-table plan-gate-table 1 "$S/plangate.sh" --table --rows 50000 --runs 0
	job plan-gate-table-tuned plan-gate-table-tuned 1 "$S/plangate.sh" --table --lane tuned --rows 50000 --runs 0
	# the ranking of the estimates of the plans (the scan, b-tree, bitmap, sequential, PostgreSQL without sift) against their user instructions, of the 99 shapes at 30000 rows (docs/AGG.md); skipped without perf
	job plan-gate-calibrate plan-gate-calibrate 1 "$S/plangate.sh" --calibrate --rows 30000
	job plan-gate-calibrate-tuned plan-gate-calibrate-tuned 1 "$S/plangate.sh" --calibrate --lane tuned --rows 30000
	# the instructions to plan the 99 shapes (30000 events: planning does not depend on the rows) against their ceilings (test/plan/planning.tsv); skipped without perf
	job plan-gate-planning plan-gate-planning 1 "$S/plangate.sh" --planning --rows 30000
	# the instructions of 20 statement classes (the reads, the writes) at 300000 orders against their ceilings (test/plan/execution.tsv); skipped without perf
	once_on_18 && job exec-budget exec-budget 1 "$S/exec-budget.sh"   # recorded on PostgreSQL 18: a count differs a little on another major
	# seeded random queries of the pushdown shapes, plain plan against the scan, the pre-aggregates and the arms alone (test/fuzz/diff_fuzz.py); --leader-on until the
	# parallel scan of a partitioned table without leader participation no longer crashes a worker
	# Lean: the seeds 1-3 run before PostgreSQL 18 and 4-6 on 18, each once (a seed is one set of random queries, the quick tier runs 1-4 on every major)
	if [[ -z ${SIFT_CI_DEEP:-} ]]; then
		seeds=1-3; (( PG_MAJOR < 18 )) || seeds=4-6
		job difftest-fuzz difftest-fuzz 1 python3 "$REPO/test/fuzz/diff_fuzz.py" --seeds $seeds --queries 100 --rows 6000 --leader-on --stop-after 3
		job difftest-fuzz-tbl difftest-fuzz 1 python3 "$REPO/test/fuzz/diff_fuzz.py" --table --seeds $seeds --queries 100 --rows 6000 --leader-on --stop-after 3
	elif (( PG_MAJOR >= 18 )); then
		for k in 1 2 3 4; do
			job difftest-fuzz-$k difftest-fuzz 1 python3 "$REPO/test/fuzz/diff_fuzz.py" --seeds $(( k * 3 - 2 ))-$(( k * 3 )) --queries 150 --rows 6000 --leader-on --stop-after 3
			job difftest-fuzz-tbl-$k difftest-fuzz 1 python3 "$REPO/test/fuzz/diff_fuzz.py" --table --seeds $(( k * 3 - 2 ))-$(( k * 3 )) --queries 150 --rows 6000 --leader-on --stop-after 3
		done
	fi
	once_on_18 && job fuzz fuzz 1 "$S/fuzz.sh" --scale "$(lean_or_deep 4 10)"
	once_on_18 && flavor_job hardened hardened "$S/hardened.sh"
	job static static 0 "$S/static.sh"
	tap_perl $(named $SLOW_PERL) $(rest t .pl $SLOW_PERL)
	tap_shell $(named $SLOW_SHELL) $(rest t .sh $SLOW_SHELL)
	installcheck $(named $SLOW_REGRESS)
	installcheck_batches 12 $(rest sql .sql $SLOW_REGRESS)
	job isolation isolation 1 "$S/isolation.sh"
	job isolation-emulated isolation-emulated 1 "$S/isolation.sh" --emulate
	job isa-baseline isa-baseline 0 "$S/isa_check.sh"
	job tools-build tools-build 0 "$S/check-tools-build.sh"
else
	# 2500 documents: past 2200 the corpus crosses a superblock boundary (the drop_sb_boundary mutant survives below it); 12 queries and 2 rounds still
	# visit every stage, the write buffers, the promotion and the merge
	job difftest difftest 1 "$S/difftest.sh" --jobs "$DIFF_JOBS" --docs 2500 --queries 12 --rounds 2 --minimize 1 --min-trials 10
	# the regression tests that need no test-only function, against the release library (SIFT_TESTING off). The quick tests only: the slow ones and the ones that need
	# the test-only build (TESTING_REGRESS of the Makefile; scripts/check-testing-regress.sh keeps a test that calls a sift._test_ function in it) are not run.
	# A test gives the same answers on both builds unless it refers to a test-only name (a sift.test_ setting, a sift._test_ function, pg_sift_test) behind a guard: those
	# run, with a smoke of STOCK_SMOKE, one test or more of each area, which loads and uses the library users get. The deep tier on 18 runs all of them (about 350 tests,
	# 760 to 1170 s on one lane in run 40193282663, against about 25 tests); on 17 the smoke is enough
	release_library_tests() {
		local t p f pats globs=()
		local -A guarded=() named=()   # guarded: the tests that name a test-only object, found in one grep; named: the names of the Makefile that are no pattern
		read -ra pats < <(sed -En 's/^TESTING_REGRESS \+?= //p' "$REPO/Makefile" | paste -sd' ' -)
		for p in "${pats[@]}"; do if [[ $p == *%* ]]; then globs+=("${p//%/*}"); else named[$p]=1; fi; done   # one regular expression for all of them is slower to match (4 s of CPU for the plan instead of 0.7)
		while IFS= read -r f; do f=${f##*/}; guarded[${f%.sql}]=1; done < <(grep -lE 'sift\.test_|sift\._test_|pg_sift_test' "$REPO"/test/sql/*.sql)
		for t in $(rest sql .sql $SLOW_REGRESS); do
			[[ ! -v named[$t] ]] || continue
			for p in "${globs[@]}"; do [[ $t == $p ]] && continue 2; done
			[[ ( -n ${SIFT_CI_DEEP:-} && $PG_MAJOR == 18 ) || " $STOCK_SMOKE " == *" $t "* || -v guarded[$t] ]] || continue
			echo "$t"
		done
	}
	flavor_job release-library-regress release-library-regress "$S/release-library-check.sh" $(release_library_tests)
	tap_perl 004_wal_consistency   # the biggest in scratch: it starts while the budget is free
	installcheck facets index_build_postings
	# one pass (--budget 1) of every target: its seeds and the inputs of the fixed findings are the same, the random and exhaustive loops shorter
	job fuzz fuzz 1 env FZ_SECS=2 FZ_KERNELS_MILLIONS=0.05 FZ_GRID_STRIDE=7 FZ_CODEC_ITERS=100 FZ_ANALYZE_ITERS=5000 "$S/fuzz.sh" --budget 1 ${SIFT_FUZZ_TARGETS:-}
	flavor_job hardened hardened "$S/hardened.sh" colcodec_basic col_enc kernels_simd shred_values verify_corrupt query_errors
	job difftest-nopos difftest-nopos 1 env "INDEX_OPTS=WITH (positions = off)" "$S/difftest.sh" --jobs "$DIFF_JOBS" \
		--docs 1000 --queries 12 --rounds 2 --minimize 1 --min-trials 10
	# the mutants are checked on the built corpus (round 0): all 8 die there, and the rounds with DML, promotion, merge and REINDEX without a bug are the job difftest
	job difftest-mutants difftest-mutants 1 "$S/difftest.sh" --jobs "$MUTANT_JOBS" --engine mutants --docs 3000 --queries 10 --rounds 0 --minimize 0 \
		--mutants "float8_score idf_df_plus1 count_plus drop_high_offset drop_sb_boundary tie_ctid_desc nofold_query topk_short"
	job difftest-col difftest-col 1 "$S/coldiff.sh" --fixture-rows 1500 --rows 3000 --variants on,on-par --scenarios dml,abort,vacuum,held-lite --scenario-slices 3
	job difftest-col-tbl difftest-col-tbl 1 "$S/coldiff.sh" --table --suite fixtures --fixture-rows 1500 --variants on,on-par --scenarios dml,abort,vacuum --scenario-slices 3
	job difftest-col-selftest difftest-col-selftest 1 "$S/coldiff.sh" --selftest --fixture-rows 1200
	job plan-gate plan-gate 1 "$S/plangate.sh" --runs 0
	job plan-gate-tuned plan-gate-tuned 1 "$S/plangate.sh" --lane tuned --runs 0
	job plan-gate-table plan-gate-table 1 "$S/plangate.sh" --table --runs 0
	job plan-gate-table-tuned plan-gate-table-tuned 1 "$S/plangate.sh" --table --lane tuned --runs 0
	# the ranking at 250,000 rows, where the search and spine shapes grow away from a price that does not follow their rows (6 minutes a lane)
	job plan-gate-calibrate plan-gate-calibrate 1 "$S/plangate.sh" --calibrate --rows 250000
	job plan-gate-calibrate-tuned plan-gate-calibrate-tuned 1 "$S/plangate.sh" --calibrate --lane tuned --rows 250000
	job difftest-fuzz difftest-fuzz 1 python3 "$REPO/test/fuzz/diff_fuzz.py" --seeds 1-4 --queries 100 --rows 3000 --leader-on
	job difftest-fuzz-tbl difftest-fuzz 1 python3 "$REPO/test/fuzz/diff_fuzz.py" --table --seeds 1-4 --queries 100 --rows 3000 --leader-on
	tap_shell leakcheck_gate
	tap_perl 001_crash
	installcheck filter_pushdown
	job soak soak 1 "$S/soak.sh" --duration 15 --rows 20000 --rate 400 --clients 2 --check-every 5
	job soak-col soak-col 1 "$S/colsoak.sh" --duration 18 --clients 6
	tap_perl $(named $SLOW_PERL)
	tap_shell $(named $SLOW_SHELL)
	installcheck $(named $SLOW_REGRESS)
	tap_perl $(rest t .pl $SLOW_PERL)
	tap_shell $(rest t .sh $SLOW_SHELL)
	installcheck_batches 12 $(rest sql .sql $SLOW_REGRESS)
	job isolation isolation 1 "$S/isolation.sh"
	job isolation-emulated isolation-emulated 1 "$S/isolation.sh" --emulate cic_insert
	job isa-baseline isa-baseline 0 "$S/isa_check.sh"
	job tools-build tools-build 0 "$S/check-tools-build.sh"
fi
# opt-in lanes on a PostgreSQL built from source (scripts/pg-assert.sh, cached outside the repo; docs/SAFETY.md): queued only when
# SIFT_CI_JOBS names them, e.g. `SIFT_CI_JOBS=assert scripts/ci.sh quick`
[[ ${SIFT_CI_JOBS:-} != *assert* ]] || job assert assert 0 "$S/assert.sh"
[[ ${SIFT_CI_JOBS:-} != *valgrind* ]] || job valgrind valgrind 0 "$S/valgrind.sh"
