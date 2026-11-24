# shellcheck shell=bash
# The constants of the plan that other parts read: the release-library smoke list, the column slices and the job weights that cut the shards.
# weight_of sets WEIGHT from scripts/weights-*.tsv. Sourced by scripts/ci.sh; not a script of its own.
STOCK_SMOKE="sift_basic count_scan filter_pushdown_gate rank_score_shapes tbl_basic edge_ddl ops_info"   # see release_library_tests
SLOW_REGRESS="tbl_bigtx tbl_writepath build_large_docs index_build_postings filter_pushdown facets parallel_scan insert_backpressure parallel_pushdown index_build_thresholds
	edge_documents rank_semi_join analyze_invariant rank_join rank_prune phrase_positions kernels_simd col_kernels_simd col_enc parallel_build"   # the regression tests that are jobs of their own, longest first
COL_SLICES_LEAN=8 COL_SLICES_DEEP=12 COL_SLICES_DEEP17=18   # slices of the column differential jobs of the full tier (below): the deep tier cuts them finer so that no job passes the hour, and before 18 runs the first half of twelfths
COL_SLICES=$COL_SLICES_LEAN; [[ -z ${SIFT_CI_DEEP:-} ]] || COL_SLICES=$COL_SLICES_DEEP
JOB_WEIGHT_DEFAULT=10
REGRESS_BATCH_SECS=60 REGRESS_TEST_SECS=1
declare -A REGRESS_SECS=()
while IFS=$'\t' read -r t s; do [[ $t == '#'* || -z $t ]] || REGRESS_SECS[$t]=$s; done <"$REPO/scripts/regress-times.tsv"
regress_solo() { [[ $1 == @(roles_*|ext_recreate) ]]; }
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
					load[b] += sec[i]; tests[b] = tests[b] " " name[i]
				}
				for (b = 1; b <= bins; b++) if (tests[b] != "") printf "%.1f\t%s\n", load[b], tests[b]
			}' |
		while IFS=$'\t' read -r load names; do echo "$load $(tr ' ' '\n' <<<"$names" | sort | tr '\n' ' ')"; done | sort -t' ' -k1,1nr
}
# regress_lane_secs TEST...: the batch size that gives each lane three batches of the measured seconds of the tests (a run with assertions: the tests that cost 3 to 95 times their seconds there
# land in batches by chance, and a lane that finishes early takes the next batch, where one batch for each lane left the lane with the slow ones to the end of the time limit)
regress_lane_secs() {
	local t
	for t in "$@"; do echo "${REGRESS_SECS[$t]:-$REGRESS_TEST_SECS}"; done | awk -v lanes="$LANES" '{ total += $1 } END { printf "%d", total / (3 * lanes) + 1 }'
}
REGRESS_BATCH_OVERHEAD=2   # the seconds a batch spends outside its tests: the database, the leak and leftover checks (scripts/pgtest.sh regress)
declare -A W_BATCH=()   # the lane seconds of each regression batch, from the measured seconds of its tests (set by installcheck_batches of scripts/ci/jobs.sh; batch_weights for the plan of every test)
batch_weight() { awk -v l="$1" -v o="$REGRESS_BATCH_OVERHEAD" 'BEGIN { printf "%d", l + o + 0.5 }'; }
# batch_weights: W_BATCH of the batches of the plan with no filter (the regression tests that are no job of their own), for the checks that weigh a plan they did not make
batch_weights() {
	local i=0 t load names slow sel=()
	slow=($SLOW_REGRESS)
	for t in $(basename -a -s .sql "$REPO"/test/sql/*.sql); do [[ " ${slow[*]} " == *" $t "* ]] || regress_solo "$t" || sel+=("$t"); done
	while read -r load names; do W_BATCH[installcheck-batch$((++i))]=$(batch_weight "$load"); done < <(regress_pack "${sel[@]}")
}
AFTER_LANES=4   # the lanes of a hosted runner: a job that runs after the pool runs alone, so each second of it costs this many lane seconds of the shard
# the shards are cut by the lane seconds each job took on its PostgreSQL major in a hosted run (scripts/ci-times.py --weights, the run is named in the header of each file; the other jobs
# weigh JOB_WEIGHT_DEFAULT): scripts/weights-quick.tsv (the quick tier), weights-full.tsv (the full tier), weights-deep.tsv (the deep full tier).
# Each line is: job, seconds on 17, on 18, on 19 and on arm64; a file without the last two (the full tiers run on x86-64 17 and 18) weighs them like 18.
declare -A W_QUICK17 W_QUICK18 W_QUICK19 W_QUICKARM W_FULL17 W_FULL18 W_FULL19 W_FULLARM W_DEEP17 W_DEEP18 W_DEEP19 W_DEEPARM
for tier in quick full deep; do
	declare -n w17=W_${tier^^}17 w18=W_${tier^^}18 w19=W_${tier^^}19 warm=W_${tier^^}ARM
	while IFS=$'\t' read -r job a17 a18 a19 aarm; do
		[[ $job == '#'* ]] || { w17[$job]=$a17; w18[$job]=$a18; w19[$job]=${a19:-$a18}; warm[$job]=${aarm:-$a18}; }
	done <"$REPO/scripts/weights-$tier.tsv"
	unset -n w17 w18 w19 warm
done
# weight_of JOB [deep [FLAVOUR]]: sets WEIGHT to the weight in seconds of a lane (no subshell: the shard checks ask for it a million times); deep, else the mode of the run, picks the
# file, FLAVOUR (17, 18, 19, arm; else the one of this runner, FLAVOUR of config.sh) the column
weight_of() {
	local tier=${MODE:-full} fl=${3:-${FLAVOUR:-${PG_MAJOR:-18}}}
	[[ -z ${2:-} ]] || tier=deep
	[[ $tier == quick || $tier == deep ]] || tier=full
	[[ $fl == @(17|19|arm) ]] || fl=18
	local -n tier_w=W_${tier^^}${fl^^}
	local dflt=$JOB_WEIGHT_DEFAULT
	[[ $1 != installcheck-batch* ]] || { (( ${#W_BATCH[@]} )) || batch_weights; WEIGHT=${W_BATCH[$1]:-$dflt}; return; }   # a batch is cut anew by every plan: no hosted run weighed it
	WEIGHT=${tier_w[$1]:-$dflt}
}
