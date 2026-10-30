/*
 * agg/preagg/int.h - what the files of agg/preagg/ share
 * The functions the fold, the row filters, the group ids, the histograms and the rollups
 * call in one another.
 */
#ifndef SIFT_AGG_PREAGG_DIR_INT_H
#define SIFT_AGG_PREAGG_DIR_INT_H

#include "agg/gen_int.h"

extern void preagg_count(AggExec *ex, int64 rows);
extern bool rollup_dim_leaf(const AggExec *ex, int leaf);
extern bool preagg_zones(AggExec *ex);
extern bool preagg_zone_bounds(AggExec *ex);
extern bool preagg_sums(AggExec *ex);

extern uint64 * pred_pass(AggExec *ex, const SiftAggPred *p, int qlevel, int nt, uint64 *keep);
extern uint64 * preagg_keep(AggExec *ex, const SiftSrcHist *h, uint64 **fkeep);
extern bool cal_keep(AggExec *ex, const SiftSrcHist *h, uint64 **keep);



extern bool preagg_floats(AggExec *ex, int leaf, bool bounded);
extern bool rollup_net(AggExec *ex, SiftSrcRollup *r);
extern bool preagg_hist(AggExec *ex);

extern void rollup_need(AggExec *ex);
extern bool rollup_skip(AggExec *ex);
extern bool preagg_rollup(AggExec *ex);

/* the leaf an aggregate reads: its column, the only leaf of its numeric program; -1: count(*) */
static inline int
pa_leaf(const SiftAggAgg *a)
{
	sift_i128	mul,
				add;

	return a->kind == SAA_COUNT_STAR ? -1 : a->kind == SAA_GEN ? a->gen->hll : a->num.n ? sift_aggnum_linear(&a->num, &mul, &add) : a->leaf;
}

#endif							/* SIFT_AGG_PREAGG_DIR_INT_H */
