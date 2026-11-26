/*
 * agg/sip_scan.h - the join filter of agg/sip.h as a scan of the fact table of a join `fact JOIN dim ON fact.k = dim.k` over an index of format major 2 (SiftAggV6Sip): the rows of the dimension
 * that pass its own conditions give their keys to a filter (an exact set up to SIFT_SIP_EXACT_MAX keys, a Bloom filter above), and the scan of the fact table gives only the rows whose key the filter
 * may hold, the batches whose zone it rules out never read. The node is a scan of the fact table as the planner sees it: PostgreSQL's join above it still matches the rows exactly, so a false
 * positive of the filter costs a row and no result; a filter never drops a row of a key it was given. The dimension is read through the table access method, so it may be a heap table or a sift_table.
 * Chosen only for a join that drops the fact rows without a match (an inner join, or a semi join of the dimension, under no outer join that may null either) of two tables on one equality of
 * integer, date or timestamp columns, a dimension of at most SIFT_V6_SIP_DIM_ROWS rows
 * whose conditions keep at most half of its distinct keys, and never under row security of the query, whose dimension rows must not reach a filter another role could read.
 */
#ifndef SIFT_AGG_V6_SIP_H
#define SIFT_AGG_V6_SIP_H

#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

#include "agg/sip.h"
#include "agg/vexpr.h"

#define SIFT_V6_SIP_DIM_ROWS		(1 << 20)
#define SIFT_V6_SIP_DIM_KEEP		0.5

extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_sip_plan_methods;

/* set_rel_pathlist_hook: the path of the fact table of a join with a filter of its dimension */
extern void sift_agg_v6_sip_rel_paths(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte);
extern Plan *sift_agg_v6_sip_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);

/*
 * The scan of the source with the filter applied to its batches: the rows whose key (the column key_slot of the input) the filter cannot hold leave the mask of the batch, and a batch with no
 * row left is not given. The filter is probed under the adaptive state of sip.h (stopped while nearly every row passes). The source is closed with the wrapper.
 */
extern SiftV6Source *sift_v6_sip_wrap(MemoryContext cxt, SiftV6Source *inner, const SiftSipFilter *sip, int key_slot, SiftSipStats *stats);

/* The filter of the keys of the dimension: sift_v6_sip_add_key for each row that passes, sift_v6_sip_finish once; est_ndv sizes it within the budget of sip.h */
extern SiftSipFilter *sift_v6_sip_start(MemoryContext cxt, SiftVecType key_type, double est_ndv);
extern void sift_v6_sip_add_key(SiftSipFilter *sip, int64 key);
extern void sift_v6_sip_finish(SiftSipFilter *sip);

#endif							/* SIFT_AGG_V6_SIP_H */
