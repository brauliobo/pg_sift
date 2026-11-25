/*
 * rank_int.h - what rank.c and rank_search.c share: the includes and the functions that cross them.
 */
#ifndef SIFT_BM25_RANK_INT_H
#define SIFT_BM25_RANK_INT_H


#include <math.h>

#include "bm25/eval/eval.h"
#include "bm25/eval/int.h"
#include "bm25/segment/segment.h"


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

/*
 * The factor that makes a double sum of the non-negative leaf bounds an upper bound of their float4 left-to-right sum: each
 * float4 addition rounds by at most 2^-24 relative, so the sum of n terms is off by less than (n + 1) * 2^-23. Pruning with it
 * keeps every term the float4 sum would keep; the scores are computed in leaf order elsewhere.
 */
static inline double
sum_ub_slack(int nleaves)
{
	return 1.0 + (double) (nleaves + 1) / 8388608.0;
}
extern bool block_terms(SiftPlan *p, RankSeg *rs, int b, float4 theta, float4 *ub);
extern int score_block(SiftRanker *r, RankSeg *rs, const sift_grp *g, const SiftFilterSB *fs, const SiftTidBridgeSB *bsb, const SiftDocChunk *dc, int b, float4 theta,
					   const float4 *ub, uint32 live);
extern uint32 block_live(SiftPlan *p, RankSeg *rs, int b, float4 theta);
extern void hot_forget(SiftRanker *r);
extern void rank_spill_end(SiftRanker *r);

/*
 * A binary min-heap of walkers by the superblock they stand on, for the walks that merge many of them: the one at the top has the smallest gid.
 * A key may lag behind its walker (walkers only move forward), so a reader refreshes the top until it is current.
 */
typedef struct GidEnt
{
	uint32		gid;
	int			idx;
} GidEnt;

static inline void
gidheap_down(GidEnt *h, int n, int i)
{
	GidEnt		e = h[i];

	for (int c; (c = 2 * i + 1) < n; i = c)
	{
		if (c + 1 < n && h[c + 1].gid < h[c].gid)
			c++;
		if (h[c].gid >= e.gid)
			break;
		h[i] = h[c];
	}
	h[i] = e;
}

static inline void
gidheap_build(GidEnt *h, int n)
{
	for (int i = n / 2 - 1; i >= 0; i--)
		gidheap_down(h, n, i);
}

static inline void
gidheap_push(GidEnt *h, int *n, GidEnt e)
{
	int			i = (*n)++;

	for (; i > 0 && h[(i - 1) / 2].gid > e.gid; i = (i - 1) / 2)
		h[i] = h[(i - 1) / 2];
	h[i] = e;
}

/* The walker at the top now stands on gid (UINT32_MAX: it is done and leaves the heap) */
static inline void
gidheap_top_to(GidEnt *h, int *n, uint32 gid)
{
	if (gid == UINT32_MAX)
		h[0] = h[--*n];
	else
		h[0].gid = gid;
	if (*n > 0)
		gidheap_down(h, *n, 0);
}

/* The walkers keyed gid, in no order, in out; stk is scratch of n entries */
static inline int
gidheap_at(const GidEnt *h, int n, uint32 gid, int *stk, int *out)
{
	int			nstk = 0,
				nout = 0;

	if (n > 0 && h[0].gid == gid)
		stk[nstk++] = 0;
	while (nstk > 0)
	{
		int			i = stk[--nstk];

		out[nout++] = h[i].idx;
		for (int c = 2 * i + 1; c <= 2 * i + 2 && c < n; c++)
			if (h[c].gid == gid)
				stk[nstk++] = c;
	}
	return nout;
}

/* The cursors of the narrowed leaves rewind when used first after a rewind of the segment */
static inline void
term_sync(RankSeg *rs, SiftPostingCursor *pc, uint32 *epoch)
{
	if (*epoch != rs->epoch)
		sift_post_rewind(pc);
	*epoch = rs->epoch;
}

#endif
