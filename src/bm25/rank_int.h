/*
 * rank_int.h - what rank.c and rank_search.c share: the includes and the functions that cross them.
 */
#ifndef SIFT_BM25_RANK_INT_H
#define SIFT_BM25_RANK_INT_H


#include <math.h>

#include "bm25/eval.h"
#include "bm25/eval_int.h"
#include "bm25/segment.h"


/*
 * How a search walks its units (chosen once per ranker by rank_choose_mode, the same on every host; no setting):
 * BATCH_BMW      units by descending bound, handed to the scorer in batches that run in ascending (segment, gid) order
 * MAXSCORE_SB    the best units seed the threshold, then every unit in ascending order, the ones whose bound is out of reach skipped
 * CONJUNCTIVE    as MAXSCORE_SB, for a root AND: the groups are aligned on their gids and page masks before anything is decoded
 */
typedef enum SiftRankMode
{
	SIFT_RM_BATCH_BMW = 0,
	SIFT_RM_MAXSCORE_SB = 1,
	SIFT_RM_CONJUNCTIVE = 2
} SiftRankMode;

/* The units a streamed search ranks first, before it sweeps the rest in order */
#define RANK_SEED	32

typedef struct UnitStream UnitStream;

extern bool unit_make(const SiftRanker *r, int s, uint32 gid, const float4 *const *ubp, Unit *out);
extern const SiftGroupBounds *term_bound_find(const RkTerm *t, uint32 gid);
extern bool leaf_bound_at(const SiftPlan *p, const RankSeg *rs, int leaf, uint32 gid, float4 *ub);
extern uint32 bridge_first_sb(const SiftTidBridge *br, uint32 sb);

/* rank_stream.c */
extern int rank_seed_units(SiftRanker *r, Unit *seed);
extern UnitStream *unit_stream_open(SiftRanker *r, int s, const Unit *seed, int nseed);
extern bool unit_stream_next(UnitStream *st, const SiftTopK *tk, Unit *u);

extern void rank_sort_units(SiftRanker *r);
extern void rank_widen(SiftRanker *r, float4 theta);
extern void seg_rewind(RankSeg *rs);
extern const SiftGroupBounds * term_bound_at(const RkTerm *t, uint32 gid);
extern float4 sum_ub(const SiftPlan *p, const float4 *ub, int leaf, float4 v);
extern bool block_terms(SiftPlan *p, RankSeg *rs, int b, float4 theta, float4 *ub);
extern int score_block(SiftRanker *r, RankSeg *rs, const sift_grp *g, const SiftFilterSB *fs, const SiftTidBridgeSB *bsb, const SiftDocChunk *dc, int b, float4 theta,
					   const float4 *ub, uint32 live);
extern uint32 block_live(SiftPlan *p, RankSeg *rs, int b, float4 theta);
extern void hot_forget(SiftRanker *r);
extern void rank_spill_end(SiftRanker *r);

/* The cursors of the narrowed leaves rewind when used first after a rewind of the segment */
static inline void
term_sync(RankSeg *rs, SiftPostingCursor *pc, uint32 *epoch)
{
	if (*epoch != rs->epoch)
		sift_post_rewind(pc);
	*epoch = rs->epoch;
}

#endif
