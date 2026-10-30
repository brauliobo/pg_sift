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

void
sift_agg_init(void)
{
	sift_aggkern_init();
	sift_aggcol_register();
	sift_aggcte_init();
	sift_search_init();
	sift_agg_rows_init();
	sift_agg_refuse_init();
	DefineCustomBoolVariable("sift.enable_agg",
							 "Enables aggregate pushdown into sift_col column indexes (Custom Scan (SiftAgg)).",
							 NULL, &sift_enable_agg, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_force",
							 "A diagnostic: takes the aggregate scan wherever one is possible, whatever the plans cost (an arm that is refused for its shape stays refused).",
							 "The scan is priced at a millionth of its cost (the paths with workers and without keep their order) and the inline plan of a shared CTE is kept when it has a scan, to time the plan the cost model refused.",
							 &sift_agg_force, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.agg_scale_horizon",
							 "Size, as a multiple of the table, up to which an inline shared CTE that is a close call must stay the better plan (1: only the size it is).",
							 "Used where the scans of the inline plan cost less than the work of the shared CTE but not by the margin: the plans are made again at the square root of the horizon and at the horizon times the rows (sift.plan_scale), and the inline one is kept if it wins at both.",
							 &sift_agg_scale_horizon, 100.0, 1.0, 1e6,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_left_lateral",
							 "Pushes down LEFT JOIN LATERAL over a jsonb_to_recordset or jsonb_array_elements list of the scanned row: a row none of whose elements passes is one tuple with NULL element columns.",
							 NULL, &sift_agg_left_lateral, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_preagg",
							 "Answers the units of an aggregate scan whose rows are all visible from the pre-aggregates the sift_col index stored with them.",
							 NULL, &sift_agg_preagg, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_dead_delta",
							"Most percent of the rows of a unit that may be dead for the unit to be answered from its pre-aggregates net of them (counts and integer sums), with sift.agg_preagg; 0 scans such a unit.",
							"A VACUUM that removes a row version clears its liveness bit in the unit, and the unit then has no exact pre-aggregates: it was read row by row. When every page it has rows on is all-visible, "
							"the values of its dead rows are read at their positions and taken off the groups of its pre-aggregates, one row at a time. Minimum, maximum, count(DISTINCT), sketches, float sums and "
							"leaves of elements still scan the unit.",
							&sift_agg_dead_delta, 15, 0, 100, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_preagg_distinct",
							 "Answers count(DISTINCT col) of a column the cube stores a histogram of from the groups of the histograms of the units whose rows are all visible, with sift.agg_preagg.",
							 NULL, &sift_agg_preagg_distinct, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_super",
							 "Answers a group of exact units from the summary the sift_col index stored for the group (reloption superunit) instead of from each unit.",
							 NULL, &sift_agg_super, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.agg_rollup_cost",
							 "Scale of the cost of a tuple of a unit's rollup against the scan of an element: a unit is answered from its rollup only when that costs less (0: always).",
							 NULL, &sift_agg_rollup_cost, 1.0, 0.0, 1000.0,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_dense",
							 "Aggregates the chunks of counts, sums and averages from the bitmap of their rows, without a list of tuples.",
							 NULL, &sift_agg_dense, true,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_dist_stream",
							 "Visits the superblocks of a count(DISTINCT) of an integer column in the order of the zone minimum of the column when that keeps its sets small: the values below the minimum of the superblocks to come are dropped.",
							 NULL, &sift_agg_dist_stream, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_semi_lut",
							 "Answers a semi or anti join (IN, EXISTS, NOT EXISTS) on an integer leaf from a byte table over the keys of the joined table.",
							 NULL, &sift_agg_semi_lut, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_two_level",
							 "Aggregates an aggregate over per-row aggregates (UNION ALL arms grouped by a row id) in one scan of the index: the cells of a row are kept in the scan.",
							 NULL, &sift_agg_two_level_on, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_lookup_pushdown",
							 "Loads the rows a lookup join can match only: an index scan or scan keys on the table's own `column = value` conditions, and the other own conditions tested while loading.",
							 NULL, &sift_agg_lookup_pushdown, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_lookup_columns",
							 "Loads the rows of a lookup table from the leaves of a sift_col index of it that holds the columns the join reads, not from the heap.",
							 NULL, &sift_agg_lookup_columns, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_join_filter",
							 "Tests the integer key leaf of an inner or semi lookup join against the keys of the loaded table before tuples are made (zone pruning, source filters, row bitmaps).",
							 NULL, &sift_agg_join_filter, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_key_skew",
							 "Prices the probes of the normal plan for the keys of a lookup join by the rows a key owns by the column's most common values, not by the mean rows of a key.",
							 NULL, &sift_agg_key_skew, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_share_lookups",
							 "Lets the Custom Scan (SiftAgg) nodes of one statement that join the same table share the rows they load (UNION ALL arms).",
							 NULL, &sift_agg_share_lookups, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_fuse",
							 "Runs the UNION ALL arms that read the same rows in one pass over the index, as the grouping sets of one scan.",
							 NULL, &sift_agg_fuse, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_lookup_cache_kb",
							"Memory per backend that keeps the rows of the joined tables a scan loaded for the next query, while the table does not change (0: off).",
							NULL, &sift_agg_lookup_cache_kb, 256 * 1024, 0, MAX_KILOBYTES,
							PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_lookup_shared_kb",
							"Shared memory, for all backends together, that keeps the rows of the joined tables a scan loaded for the queries of other backends, while the table does not change (0: off, -1: the smaller of sift.agg_lookup_cache_kb and a twentieth of shared_buffers).",
							"Dynamic shared memory, taken as entries are kept and returned as they go, so it needs no restart; with posix shared memory it comes from /dev/shm, which it must stay below. "
							"Backends that run the same query at once wait for the one that loads the rows (30 s at most); a segment the system has no room for gives a notice and the backend loads its own tables.",
							&sift_agg_lookup_shared_kb, -1, -1, MAX_KILOBYTES,
							PGC_SUSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.verdict_cache_kb",
							"Shared memory, for all backends together, that keeps the choice between a shared CTE and its inline plan, and between the flattened form of a statement and the one as written, so that a new backend plans the form that won only (0: off).",
							"The table is allocated, at the size of this setting, by the first backend that plans such a statement after the server starts; a later change turns the use of it on and off (0), not its size. An entry is 56 bytes and a statement keeps up to three; the largest table is about 1 MB.",
							&sift_verdict_cache_kb, 256, 0, 1024,
							PGC_SUSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_verdict_cache",
							 "Keeps the choice between a shared CTE and its inline plan, and between the flattened form of a statement and the one as written, for the statements of a backend and the others (off: every statement is planned in full, so a plan does not depend on the statements planned before it).",
							 NULL, &sift_agg_verdict_cache, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_semi_lut_size",
							"Memory of the byte table of a semi or anti join, per scan and per worker; keys that span more keep the generic join.",
							NULL, &sift_agg_semi_lut_size, 4096, 0, 1024 * 1024,
							PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.enable_topn",
							 "Enables ORDER BY .. LIMIT from the column units (Custom Scan (SiftAgg) with Top-N), with sift.enable_agg.",
							 NULL, &sift_enable_topn, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.topn_walk",
							 "Lets a top-N scan visit the units of a sort column that does not follow the table by the best value of their zones, and stop at the first that cannot beat the n-th.",
							 NULL, &sift_topn_walk, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.topn_zone_keys",
							 "Lets the walk of a top-N bound the units by the zones of the sort keys after the first too: a first key that ties between units is decided by the next.",
							 NULL, &sift_topn_zone_keys, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.test_topn_heads",
							 "Testing: a top-N scan answers a unit of which every row passes from the head of the sorted run of its first sort column (column format minor 18) instead of reading it.",
							 NULL, &sift_topn_heads, true,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_child_warm",
							 "Testing: a correlated child asks the OS for the relations it reads after a few runs, whatever they read and however long they took.",
							 NULL, &sift_child_warm_test, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_topn_gorder",
							 "Testing: a top-N scan of one integer or time key reads the global order of the key (column format minor 21) in place of the units.",
							 NULL, &sift_topn_gorder_on, true,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
#endif
	DefineCustomBoolVariable("sift.topn_runs",
							 "Lets a top-N scan read a unit through the sorted run of its first sort column (an index with the option order_columns) instead of testing every row of the unit.",
							 NULL, &sift_topn_runs, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_param_set_size",
							"Elements assumed for an array parameter of a set filter (col = ANY($1)) in a generic plan, to cost the index and bitmap scans against Custom Scan (SiftAgg); 0 keeps the planner's own guess.",
							NULL, &sift_agg_param_set_size, 1000, 0, 100000000,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_par_min_rows",
							"Rows a scan must keep (in the units the zones and the keys of its lookups leave) to start the workers its plan asks for, when the planner priced them for the rows the scan reads rather than for the join rows PostgreSQL expects; 0 starts them for any scan.",
							NULL, &sift_agg_par_min_rows, 80000, 0, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_lookup_max_kb",
							"Memory of the rows a sub-select, VALUES list, function or spine joined to Custom Scan (SiftAgg) may return, as the planner estimates them: the rows go into a lookup table in memory when the scan starts; a larger one keeps the normal join.",
							NULL, &sift_agg_lookup_max_kb, 256 * 1024, 0, MAX_KILOBYTES,
							PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_lookup_max_rows",
							"The older form of sift.agg_lookup_max_kb: rows a sub-select, VALUES list or function joined to Custom Scan (SiftAgg) may return, whatever their width (-1: the memory limit decides).",
							NULL, &sift_agg_lookup_max_rows, -1, -1, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.test_verdict_shm_fail",
							 "Testing: the table of the shared verdicts cannot be had (an error in the registry), once for a backend.",
							 NULL, &sift_test_verdict_fail, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_fail_pick",
							"Testing: planning the n-th column index (1 is the second) of a table in an aggregate stage raises an error (0: none).",
							NULL, &sift_test_fail_pick, 0, 0, 100,
							PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_verdict_gate",
							 "Testing: a statement whose verdict is not cached waits, before it is planned in full, for the advisory lock (20984, 1) to be free.",
							 NULL, &sift_test_verdict_gate, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
#endif
	DefineCustomBoolVariable("sift.test_agg_wait",
							 "Testing: a top-N scan waits, once its source is open and before it reads a unit, for the advisory lock (20983, 1) to be free.",
							 NULL, &sift_test_agg_wait, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	sift_aggbe_init();
	DefineCustomIntVariable("sift.agg_combo_cache",
							"Distinct value combinations of an expression PostgreSQL evaluates for the aggregate scan kept before the cache starts again.",
							NULL, &sift_agg_combo_cache, 1 << 20, 16, 1 << 24,
							PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}

Node *
sift_agg_create_state(CustomScan *cscan)
{
	AggExec    *ex = (AggExec *) newNode(sizeof(AggExec), T_CustomScanState);

	ex->css.methods = &agg_exec_methods;
	ex->css.flags = cscan->flags;
	return (Node *) ex;
}
