/*
 * agg/two/int.h - what the planner (agg/two/plan.c) and the executor (agg/two/exec.c) of SiftAggV6Two share: the layout of the plan.
 *
 * CustomScan (scanrelid 0):
 *   custom_plans      = the subquery scan of each arm, whose tuple is the columns of the outer FROM item in order (a cell), then the plan of the spine if there is one
 *   custom_scan_tlist = the keys of the outer aggregate, then its aggregates: the tuple of a group
 *   custom_private    = list_make5(spec, inputs, labels, sort, spine)
 *     spec            = the packed SiftV6Spec of the outer aggregate: its columns are the inputs (attno i + 1 is input i)
 *     inputs          = the expression of each input over the columns of the FROM item, evaluated for every cell (the Vars name the FROM item; the executor reads them off the cell),
 *                       and over the columns of the spine's tuple (OUTER_VAR)
 *     labels          = TWOL_N String nodes for EXPLAIN
 *     sort            = NIL, or the groups leave in the order of the GROUP BY: of each key its column of the group tuple, sort operator, collation and nulls_first
 *     spine           = NIL, or list_make5(list_make2_int(left, relid), cell_keys, spine_keys, list_make3(ops, colls, null_safe), list_make2(match_quals, post_quals)):
 *                       the equalities and the other conditions of the join to the spine, and the WHERE above a LEFT JOIN (agg/two/hash.h)
 *   plan.qual         = the outer HAVING
 */
#ifndef SIFT_AGG_TWO_INT_H
#define SIFT_AGG_TWO_INT_H

typedef enum TwoLabel
{
	TWOL_KEYS,					/* the outer keys, in the terms of the first arm */
	TWOL_CELLS,					/* the group expressions of the arms that are neither the row id nor an outer key: the keys of the cells */
	TWOL_AGGS,					/* the outer aggregates, by what they read of a cell */
	TWOL_N
} TwoLabel;

#endif							/* SIFT_AGG_TWO_INT_H */
