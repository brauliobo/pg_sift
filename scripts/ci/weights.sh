# The constants of the plan that other parts read: the release smoke list, the column slices and the job weights that cut the shards.
# weight_of sets WEIGHT from JOB_WEIGHT or scripts/weights-deep.tsv. Sourced by scripts/ci.sh; not a script of its own.
STOCK_SMOKE="sift_basic count_scan filter_pushdown_gate rank_score_shapes facet_counts col_scan tbl_basic tbl_agg agg_rows agg_window agg_refusals agg_privileges roles_ddl roles_dml_query roles_maintenance edge_ddl cache_discard ops_info"   # see release_library_tests
COL_SLICES_LEAN=4 COL_SLICES_DEEP=12 COL_SLICES_DEEP17=18   # slices of the column differential jobs of the full tier (below): the deep tier cuts them finer so that no job passes the hour, and before 18 runs the first half of twelfths
COL_SLICES=$COL_SLICES_LEAN; [[ -z ${SIFT_CI_DEEP:-} ]] || COL_SLICES=$COL_SLICES_DEEP
declare -A JOB_WEIGHT=(
	[difftest-col-3]=1203 [difftest-col-1]=1138 [difftest-fuzz-tbl]=550 [difftest-col-tbl-4]=1010 [difftest-col-4]=961 [difftest]=945
	[difftest-col-tbl-2]=920 [difftest-mutants]=862 [difftest-col-2]=813 [difftest-col-tbl-3]=780 [difftest-fuzz]=330 [fuzz]=428
	[tap-shell-store_crash]=372 [tap-shell-col_index_crash]=304 [installcheck-tbl_writepath]=285 [difftest-nopos]=259
	[installcheck-col_topn_parallel]=245 [tap-shell-pg_upgrade_32]=240 [tap-001_crash]=239 [tap-shell-index_build_crash]=227
	[tap-shell-pg_upgrade_33]=224 [installcheck-tbl_preagg_calendar]=216 [installcheck-agg_partitions_many]=212 [tap-014_tbl_writepath]=207
	[tap-shell-col_auto_merge_partition]=204 [soak]=202 [installcheck-agg_distinct_narrow]=200 [installcheck-col_scan]=198
	[installcheck-agg_tid_range]=180 [installcheck-col_topn_gorder]=180 [tap-shell-col_crash_matrix]=172 [isolation-emulated]=165 [hardened]=164
	[installcheck-tbl_key_table]=153 [tap-shell-pg_upgrade]=152 [installcheck-col_preagg_elem]=136 [tap-shell-index_random_crash]=135 [isolation]=126
	[installcheck-col_lazy_filter]=123 [soak-col]=123 [installcheck-col_sorted_run]=118 [installcheck-agg_preagg_elem]=115
	[tap-shell-col_run_heads_crash]=114 [tap-006_truncate]=113 [installcheck-agg_trunc_zone]=112 [installcheck-tbl_preagg]=112
	[tap-shell-col_wbuf_crash]=110 [tap-017_tbl_autodefrag]=108 [plan-gate-table-tuned]=108 [plan-gate-calibrate]=374 [plan-gate-calibrate-tuned]=355 [installcheck-parallel_pushdown]=108
	[installcheck-index_build_postings]=107 [difftest-col-selftest]=107 [installcheck-agg_preagg_dimfilter]=104 [installcheck-agg_par_arms]=100
	[tap-shell-pg_upgrade_31]=97 [tap-shell-sb_mask_small]=94 [tap-shell-maint_fanin]=90
)
JOB_WEIGHT_DEFAULT=10
# the deep full tier is cut by the lane seconds each job took on its PostgreSQL major (run 40193282663; the other jobs weigh 10, a major from 19 on weighs like 18):
# scripts/weights-deep.tsv, job, seconds on 17, seconds on 18
declare -A JOB_WEIGHT_DEEP17 JOB_WEIGHT_DEEP18
while IFS=$'\t' read -r job w17 w18; do
	[[ $job == '#'* ]] || { JOB_WEIGHT_DEEP17[$job]=$w17; JOB_WEIGHT_DEEP18[$job]=$w18; }
done <"$REPO/scripts/weights-deep.tsv"
# weight_of JOB [deep [MAJOR]]: sets WEIGHT to the weight in seconds of a lane (no subshell: the shard checks ask for it a million times)
weight_of() {
	local -n deep_w=JOB_WEIGHT_DEEP$([[ ${3:-${PG_MAJOR:-18}} == 17 ]] && echo 17 || echo 18)
	if [[ -n ${2:-} ]]; then WEIGHT=${deep_w[$1]:-$JOB_WEIGHT_DEFAULT}; else WEIGHT=${JOB_WEIGHT[$1]:-$JOB_WEIGHT_DEFAULT}; fi
}
