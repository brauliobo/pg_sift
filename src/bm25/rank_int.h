/*
 * rank_int.h - what rank.c and rank_search.c share: the includes and the functions that cross them.
 */
#ifndef SIFT_BM25_RANK_INT_H
#define SIFT_BM25_RANK_INT_H

#include "core/host.h"

#include <math.h>

#include "bm25/eval.h"
#include "bm25/eval_int.h"
#include "bm25/kernels.h"
#include "bm25/wbuf.h"
#include "bm25/prefix.h"
#include "bm25/segment.h"
#include "core/cmp.h"


extern void seg_rewind(RankSeg *rs);
extern const SiftGroupBounds * term_bound_at(const RkTerm *t, uint32 gid);
extern float4 sum_ub(const SiftPlan *p, const float4 *ub, int leaf, float4 v);
extern bool block_terms(SiftPlan *p, RankSeg *rs, int b, float4 theta, float4 *ub);
extern int score_block(SiftRanker *r, RankSeg *rs, const sift_grp *g, const SiftFilterSB *fs, const SiftDocChunk *dc, int b, float4 theta, const float4 *ub);
extern void hot_forget(SiftRanker *r);

/* The cursors of the narrowed leaves rewind when used first after a rewind of the segment */
static inline void
term_sync(RankSeg *rs, SiftPostingCursor *pc, uint32 *epoch)
{
	if (*epoch != rs->epoch)
		sift_post_rewind(pc);
	*epoch = rs->epoch;
}

#endif
