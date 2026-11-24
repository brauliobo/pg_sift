/*
 * agg/exec/init.c - the start of the library's aggregate side
 * The settings (GUCs), the hooks and the custom scan methods registered when the library
 * loads, and the state a scan starts from.
 */
#include "postgres.h"

#include "agg/agg.h"
#include "agg/refuse.h"
#include "agg/rows.h"
#include "agg/exec/int.h"
#include "agg/v6/v6_live.h"
#include "col/compress.h"
#include "core/par/pool.h"

void
sift_agg_init(void)
{
	sift_par_pool_on_thread_exit(sift_colz_thread_fini);
	sift_aggkern_init();
	sift_aggcol_register();
	sift_aggcte_init();
	sift_agg_probe_init();
	sift_search_init();
#ifdef SIFT_TESTING
	sift_agg_scope_init();
#endif
	sift_agg_rows_init();
	sift_agg_refuse_init();
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_left_lateral",
							 "Pushes down LEFT JOIN LATERAL over a jsonb_to_recordset or jsonb_array_elements list of the scanned row: a row none of whose elements passes is one tuple with NULL element columns.",
							 NULL, &sift_agg_left_lateral, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_preagg",
							 "Answers the units of an aggregate scan whose rows are all visible from the pre-aggregates the sift_col index stored with them.",
							 NULL, &sift_agg_preagg, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_preagg_distinct",
							 "Answers count(DISTINCT col) of a column the cube stores a histogram of from the groups of the histograms of the units whose rows are all visible, with sift.agg_preagg.",
							 NULL, &sift_agg_preagg_distinct, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_dist_stream",
							 "Visits the superblocks of a count(DISTINCT) of an integer column in the order of the zone minimum of the column when that keeps its sets small: the values below the minimum of the superblocks to come are dropped.",
							 NULL, &sift_agg_dist_stream, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_semi_lut",
							 "Answers a semi or anti join (IN, EXISTS, NOT EXISTS) on an integer leaf from a byte table over the keys of the joined table.",
							 NULL, &sift_agg_semi_lut, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_connect_found",
							 "Answers a semi, anti or inner join to the parent of a connected sift_col index that reads no column of the parent from the flag the index keeps that the parent row was found, less the keys that changed in the parent since.",
							 NULL, &sift_agg_connect_found, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_child_first",
							 "Plans the count of the rows of a table that have (or have no) row in another by a unique key, and the aggregates of a table left-joined to a table by its key, from the other table first: its rows counted by their keys or grouped by them, and keeps that plan where it costs less.",
							 NULL, &sift_agg_child_first, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_lookup_pushdown",
							 "Loads the rows a lookup join can match only: an index scan or scan keys on the table's own `column = value` conditions, and the other own conditions tested while loading.",
							 NULL, &sift_agg_lookup_pushdown, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_case_filter",
							 "Plans agg(CASE WHEN p THEN x [ELSE NULL | 0] END) of sum, avg, min, max and count as agg(x) FILTER (WHERE p), which the scan computes on the kernels of x under a test of p.",
							 NULL, &sift_agg_case_filter_enabled, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_lookup_columns",
							 "Loads the rows of a lookup table from the leaves of a sift_col index of it that holds the columns the join reads, not from the heap.",
							 NULL, &sift_agg_lookup_columns, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_join_filter",
							 "Tests the integer key leaf of an inner or semi lookup join against the keys of the loaded table before tuples are made (zone pruning, source filters, row bitmaps).",
							 NULL, &sift_agg_join_filter, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_key_skew",
							 "Prices the probes of the normal plan for the keys of a lookup join by the rows a key owns by the column's most common values, not by the mean rows of a key.",
							 NULL, &sift_agg_key_skew, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_share_lookups",
							 "Lets the Custom Scan (SiftAgg) nodes of one statement that join the same table share the rows they load (UNION ALL arms).",
							 NULL, &sift_agg_share_lookups, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.test_topn_heads",
							 "Testing: a top-N scan answers a unit of which every row passes from the head of the sorted run of its first sort column (column format minor 18) instead of reading it.",
							 NULL, &sift_topn_heads, true,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomStringVariable("sift.test_probe_hook",
							   "Testing: STAGE:N:COMMAND, a command run at a stage of the direct probe of a correlated child (agg/combo/probe_test.c).",
							   NULL, &sift_test_probe_hook, "",
							   PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, sift_test_probe_hook_assign, NULL);
	DefineCustomBoolVariable("sift.test_child_warm",
							 "Testing: the direct probe of a correlated child asks for the heap blocks of the entries it found from the first, whatever the statement has read from the OS.",
							 NULL, &sift_child_warm_test, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_topn_gorder",
							 "Testing: a top-N scan of one integer or time key reads the global order of the key (column format minor 21) in place of the units.",
							 NULL, &sift_topn_gorder_on, true,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_topn_gbudget",
							"Testing: the heap fetches a global-order walk with a condition on another column may make before the units read less (0: from the size of the heap).",
							NULL, &sift_topn_gbudget, 0, 0, INT_MAX,
							PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
#endif
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.test_verdict_shm_fail",
							 "Testing: the table of the shared verdicts cannot be had (an error in the registry), once for a backend.",
							 NULL, &sift_test_verdict_fail, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_unified_agg",
							 "Testing: Format 1 statements are planned by SiftAggV6 alone, the legacy Format 1 planner is left out.",
							 NULL, &sift_test_unified_agg, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_fail_pick",
							"Testing: planning the n-th column index (1 is the second) of a table in an aggregate stage raises an error (0: none).",
							NULL, &sift_test_fail_pick, 0, 0, 100,
							PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_direct_max",
							"Testing: groups above which a group by leaves its direct arrays for the hash table, a single key included (0: 1,048,576 for several keys, the memory limit for one).",
							NULL, &sift_test_direct_max, 0, 0, INT_MAX,
							PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_agg_pass_w",
							"Testing: the shares (of 32) a pass of a scan in passes reads: n always reads in passes of n, 0 as the memory asks, -1 never.",
							NULL, &sift_test_agg_pass_w, 0, -1, 31,
							PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_agg_verdict_cache",
							 "Testing: a statement with a shared CTE is planned from the verdict kept for it; off plans every one in full.",
							 NULL, &sift_agg_verdict_cache, true,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_verdict_gate",
							 "Testing: a statement whose verdict is not cached waits, before it is planned in full, for the advisory lock (20984, 1) to be free.",
							 NULL, &sift_test_verdict_gate, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_agg_probe_rewrite",
							 "Testing: a join to a lookup table is planned as a probe of its keys.",
							 NULL, &sift_agg_probe_rewrite, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_agg_two_level",
							 "Testing: a left join of aggregates is one two-level scan node.",
							 NULL, &sift_agg_two_level_on, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_agg_probe_direct",
							 "Testing: the probe of a correlated child reads its entries directly.",
							 NULL, &sift_agg_probe_direct, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_agg_float_step",
							 "Testing: a float8 sum restarts its steps when the values change in size.",
							 NULL, &sift_agg_float_step, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_agg_dead_delta",
							"Testing: percent of dead rows a segment may hold before a scan reads its units instead of the pre-aggregates.",
							NULL, &sift_agg_dead_delta, 15, 0, 100, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.test_plan_scale",
							 "Testing: the factor the cost of a plan without a scan is scaled by.",
							 NULL, &sift_plan_scale, 1.0, 1.0, 1e6, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.test_agg_rollup_cost",
							 "Testing: the factor the cost of reading a rollup is scaled by.",
							 NULL, &sift_agg_rollup_cost, 1.0, 0.0, 100.0, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_agg_super",
							 "Testing: a scan answers from the summaries of groups of units.",
							 NULL, &sift_agg_super, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_topn_zone_keys",
							 "Testing: a top-N scan orders the units by the zone keys of its sort column.",
							 NULL, &sift_topn_zone_keys, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_topn_walk",
							 "Testing: a top-N scan walks the order of its sort column.",
							 NULL, &sift_topn_walk, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_agg_dense",
							 "Testing: a group by of a small integer key uses direct arrays.",
							 NULL, &sift_agg_dense, true, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
#endif
}

Node *
sift_agg_create_state(CustomScan *cscan)
{
	AggExec    *ex = (AggExec *) newNode(sizeof(AggExec), T_CustomScanState);

	ex->css.methods = &agg_exec_methods;
	ex->css.flags = cscan->flags;
	return (Node *) ex;
}
