# The constants of the plan that other parts read: the release-library smoke list, the column slices and the job weights that cut the shards.
# weight_of sets WEIGHT from scripts/weights-*.tsv. Sourced by scripts/ci.sh; not a script of its own.
STOCK_SMOKE="sift_basic count_scan filter_pushdown_gate rank_score_shapes facet_counts col_scan tbl_basic tbl_agg agg_rows agg_window agg_refusals agg_privileges roles_ddl roles_dml_query roles_maintenance edge_ddl cache_discard ops_info"   # see release_library_tests
SLOW_REGRESS="tbl_bigtx tbl_writepath build_large_docs index_build_postings filter_pushdown facets parallel_scan insert_backpressure parallel_pushdown index_build_thresholds
	edge_documents rank_semi_join analyze_invariant rank_join rank_prune phrase_positions kernels_simd col_kernels_simd col_enc parallel_build"   # the regression tests that are jobs of their own, longest first
COL_SLICES_LEAN=8 COL_SLICES_DEEP=12 COL_SLICES_DEEP17=18   # slices of the column differential jobs of the full tier (below): the deep tier cuts them finer so that no job passes the hour, and before 18 runs the first half of twelfths
COL_SLICES=$COL_SLICES_LEAN; [[ -z ${SIFT_CI_DEEP:-} ]] || COL_SLICES=$COL_SLICES_DEEP
JOB_WEIGHT_DEFAULT=10
# the shards are cut by the lane seconds each job took on its PostgreSQL major in a hosted run (scripts/ci-times.py --weights, the run is named in the header of each file; the other jobs
# weigh JOB_WEIGHT_DEFAULT; a major from 19 on weighs like 18): scripts/weights-quick.tsv (the quick tier), weights-full.tsv (the full tier), weights-deep.tsv (the deep full tier).
# Each line is: job, seconds on 17, seconds on 18.
declare -A W_QUICK17 W_QUICK18 W_FULL17 W_FULL18 W_DEEP17 W_DEEP18
for tier in quick full deep; do
	declare -n w17=W_${tier^^}17 w18=W_${tier^^}18
	while IFS=$'\t' read -r job a17 a18; do
		[[ $job == '#'* ]] || { w17[$job]=$a17; w18[$job]=$a18; }
	done <"$REPO/scripts/weights-$tier.tsv"
	unset -n w17 w18
done
# weight_of JOB [deep [MAJOR]]: sets WEIGHT to the weight in seconds of a lane (no subshell: the shard checks ask for it a million times); deep, else the mode of the run, picks the file
weight_of() {
	local tier=${MODE:-full}
	[[ -z ${2:-} ]] || tier=deep
	[[ $tier == quick || $tier == deep ]] || tier=full
	local -n tier_w=W_${tier^^}$([[ ${3:-${PG_MAJOR:-18}} == 17 ]] && echo 17 || echo 18)
	WEIGHT=${tier_w[$1]:-$JOB_WEIGHT_DEFAULT}
}
