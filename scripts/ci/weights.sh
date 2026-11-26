# shellcheck shell=bash
# The constants of the plan that other parts read: the release-library smoke list and the job weights that cut the shards.
# weight_of sets WEIGHT from scripts/weights-*.tsv. Sourced by scripts/ci.sh; not a script of its own.
STOCK_SMOKE="bm25/sift_basic bm25/count_scan bm25/filter/pushdown_gate bm25/rank/score_shapes table/basic bm25/edge_ddl bm25/ops_info"   # see release_library_tests
# SLOW_REGRESS: the regression tests that are jobs of their own, longest first: every test of scripts/regress-times.tsv of 20 s or more (a batch of REGRESS_BATCH_SECS with one in it is its straggler),
# and the three the plan queues first for their scratch (scripts/ci/plan.sh). scripts/check-test-budget.sh keeps the list from growing
SLOW_REGRESS="col/f2/ux_progress col/build/memory agg/parent_child agg/parallel_2stage_robust agg/par/distinct_dict_robust agg/parallel_col_delta agg/par/distinct_dict table/f2/col_delta
	col/preagg/elem table/writepath col/array/elems table/preagg/calendar agg/group/stream bm25/facets bm25/filter/pushdown bm25/index/build_postings"
JOB_WEIGHT_DEFAULT=10
REGRESS_BATCH_SECS=60 REGRESS_TEST_SECS=1
declare -A REGRESS_SECS=()
while IFS=$'\t' read -r t s; do [[ $t == '#'* || -z $t ]] || REGRESS_SECS[$t]=$s; done <"$REPO/scripts/regress-times.tsv"
regress_solo() { [[ $1 == @(core/roles/*|core/ext_recreate) ]]; }
# regress_pack TEST...: one line "SECONDS TEST..." for each batch, the heaviest first: the tests by measured seconds, longest first, each into the lightest of the batches that
# the total seconds need (the longest-processing-time rule), the names of a batch in alphabetical order
regress_pack() {
	local t
	for t in "$@"; do printf '%s\t%s\n' "${REGRESS_SECS[$t]:-$REGRESS_TEST_SECS}" "$t"; done | sort -t$'\t' -k1,1nr -k2,2 |
		awk -F'\t' -v cap="$REGRESS_BATCH_SECS" '
			{ sec[NR] = $1; name[NR] = $2; total += $1 }
			END {
				bins = int((total + cap - 1) / cap); if (bins < 1) bins = 1
				for (i = 1; i <= NR; i++) {
					b = 1; for (j = 2; j <= bins; j++) if (load[j] < load[b]) b = j
					load[b] += sec[i]; of[i] = b
				}
				for (i = 1; i <= NR; i++) printf "%d\t%.1f\t%s\n", of[i], load[of[i]], name[i]
			}' | sort -t$'\t' -k1,1n -k3,3 |
		awk -F'\t' '$1 != b { if (NR > 1) print line; b = $1; line = $2 " " } { line = line $3 " " } END { if (NR) print line }' | sort -t' ' -k1,1nr
}
# regress_lane_secs TEST...: the batch size that gives each lane three batches of the measured seconds of the tests (a run with assertions: the tests that cost 3 to 95 times their seconds there
# land in batches by chance, and a lane that finishes early takes the next batch, where one batch for each lane left the lane with the slow ones to the end of the time limit)
regress_lane_secs() { regress_part_secs $(( 3 * LANES )) "$@"; }
# regress_part_secs PARTS TEST...: the batch size that cuts the measured seconds of the tests in PARTS batches
regress_part_secs() {
	local t n=$1
	for t in "${@:2}"; do echo "${REGRESS_SECS[$t]:-$REGRESS_TEST_SECS}"; done | awk -v n="$n" '{ total += $1 } END { printf "%d", total / n + 1 }'
}
REGRESS_BATCH_OVERHEAD=2   # the seconds a batch spends outside its tests: the database, the leak and leftover checks (scripts/pgtest.sh regress)
declare -A W_BATCH=()   # the lane seconds of each regression batch, from the measured seconds of its tests (set by installcheck_batches of scripts/ci/jobs.sh; batch_weights for the plan of every test)
# batch_weight LOAD: sets BATCH_W to LOAD (one decimal, regress_pack) plus the overhead, rounded half up; no fork for each of the 50 batches of a plan
batch_weight() { BATCH_W=$(( ${1%.*} + REGRESS_BATCH_OVERHEAD + (${1#*.} >= 5) )); }
# batch_weights: W_BATCH of the batches of the plan with no filter (the regression tests that are no job of their own), for the checks that weigh a plan they did not make
batch_weights() {
	local i=0 t load slow sel=()
	slow=($SLOW_REGRESS)
	for t in $(tests sql .sql); do [[ " ${slow[*]} " == *" $t "* ]] || regress_solo "$t" || sel+=("$t"); done
	while read -r load _; do batch_weight "$load"; W_BATCH[installcheck-batch$((++i))]=$BATCH_W; done < <(regress_pack "${sel[@]}")
}
AFTER_LANES=4   # the lanes of a hosted runner: a job that runs after the pool runs alone, so each second of it costs this many lane seconds of the shard
# the shards are cut by the lane seconds each job took on its PostgreSQL major in a hosted run (scripts/ci-times.py --weights, the run is named in the header of the file; the other jobs
# weigh JOB_WEIGHT_DEFAULT): scripts/weights-quick.tsv. Each line is: job, seconds on 17, on 18, on 19 and on arm64.
declare -A W_17 W_18 W_19 W_ARM
while IFS=$'\t' read -r job a17 a18 a19 aarm; do
	[[ $job == '#'* ]] || { W_17[$job]=$a17; W_18[$job]=$a18; W_19[$job]=${a19:-$a18}; W_ARM[$job]=${aarm:-$a18}; }
done <"$REPO/scripts/weights-quick.tsv"
# weight_of JOB [FLAVOUR]: sets WEIGHT to the weight in seconds of a lane (no subshell: the shard checks ask for it a million times); FLAVOUR (17, 18, 19, arm; else the one of this runner,
# FLAVOUR of config.sh) picks the column
weight_of() {
	local fl=${2:-${FLAVOUR:-${PG_MAJOR:-18}}}
	[[ $fl == @(17|19|arm) ]] || fl=18
	local -n tier_w=W_${fl^^}
	[[ ! -v W_BATCH[$1] ]] || { WEIGHT=${W_BATCH[$1]}; return; }   # a batch is cut anew by every plan: no hosted run weighed it
	[[ $1 != installcheck-batch* ]] || { [[ -v W_BATCH[installcheck-batch1] ]] || batch_weights; WEIGHT=${W_BATCH[$1]:-$JOB_WEIGHT_DEFAULT}; return; }
	[[ $1 != installcheck-* || -v tier_w[$1] || ! -v REGRESS_SECS[${1#installcheck-}] ]] || { batch_weight "${REGRESS_SECS[${1#installcheck-}]}"; WEIGHT=$BATCH_W; return; }   # a solo test no hosted run weighed
	WEIGHT=${tier_w[$1]:-$JOB_WEIGHT_DEFAULT}
}
