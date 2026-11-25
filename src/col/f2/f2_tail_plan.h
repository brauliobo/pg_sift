/*
 * col/f2/f2_tail_plan.h - which part holds the end of the file in place and where the rewrite of it may go, from the extents everything in the file holds. The pass of f2_tail.c asks for
 * for a move, makes it, and asks again until the holders of the first plan are moved. Nothing here reads a page.
 */
#ifndef SIFT_F2_TAIL_PLAN_H
#define SIFT_F2_TAIL_PLAN_H

#include "core/host.h"

/* STOR[STOR-F2-04]: the tail is relocated once 10% of the file trails the holder (was 20%: up to 19.9% of trailing bloat survived every VACUUM) */
#define SIFT_TAIL_RELOCATE_PCT	10

typedef enum SiftTailKind
{
	TAIL_PART = 0,				/* a base part with its deletion vector */
	TAIL_SCOPE,					/* the summary of a scope */
	TAIL_PROJ_PART,				/* a projection part */
	TAIL_PROJ_DIR				/* the pages of the directory of one projection, or of the refs, which a commit of the directory rewrites together */
} SiftTailKind;

/* What one holder of blocks spans: the least block and one past the greatest, and the blocks a rewrite of it takes (for a directory, the pages of both chains it rewrites) */
typedef struct SiftTailSpan
{
	SiftTailKind kind;
	BlockNumber lo;
	BlockNumber hi;
	uint64		blocks;
} SiftTailSpan;

typedef struct SiftTailPick
{
	int			top;			/* the index of the span that holds the high-water mark */
	int			run;			/* the holders that reach farthest and must all move for the cut: top is the first of them */
	BlockNumber below_part;		/* its rewrite goes below this block: below the part itself and the others */
	BlockNumber below_cut;		/* when the free space under the part does not hold it: below the block at which a cut saves the share the move already requires */
} SiftTailPick;

typedef struct TailByHi
{
	BlockNumber hi;
	int			idx;
} TailByHi;

static inline int
sift_tail_by_hi_desc(const void *a, const void *b)
{
	const TailByHi *x = a,
			   *y = b;

	return (x->hi < y->hi) - (x->hi > y->hi);
}

/*
 * The holders at the end of the file are rewritten when the ones that reach farthest, k of them, alone pin the high-water mark: what lies between the highest of the others and the mark is at
 * least SIFT_TAIL_RELOCATE_PCT of the file and min_gap blocks, and the free extents hold the k. A directory or a projection part is a holder like any other, and a handful of them near the end
 * (a projection part and its directory pages for each part of a partition) is the usual way a file is pinned, so the pick looks past the first holder. The pick is the highest holder, to be
 * rewritten below below_part or, failing that, below below_cut; the pass asks again after each move and makes as many as the run it was first told (the small parts that a streaming
 * compaction writes at the end of the file are a run of as many moves as it has parts).
 */
static inline bool
sift_tail_pick(const SiftTailSpan *span, int n, BlockNumber hw, uint64 free_blocks, BlockNumber min_gap, SiftTailPick *out)
{
	TailByHi   *ord;
	uint64		blocks = 0;
	BlockNumber lo = PG_UINT32_MAX;
	bool		found = false;

	*out = (SiftTailPick) {.top = -1};
	if (n <= 0)
		return false;
	ord = palloc(n * sizeof(TailByHi));
	for (int i = 0; i < n; i++)
		ord[i] = (TailByHi) {span[i].hi, i};
	qsort(ord, n, sizeof(TailByHi), sift_tail_by_hi_desc);
	for (int k = 1; k <= n && ord[0].hi != 0 && ord[0].hi == hw && blocks <= free_blocks && !found; k++)
	{
		BlockNumber others = k < n ? ord[k].hi : 0;

		blocks += span[ord[k - 1].idx].blocks;
		lo = Min(lo, span[ord[k - 1].idx].lo);
		found = blocks <= free_blocks && (uint64) (hw - others) * 100 >= (uint64) hw * SIFT_TAIL_RELOCATE_PCT && hw - others >= min_gap;
		out->top = ord[0].idx;
		out->run = k;
		out->below_part = Max(others, lo);
		out->below_cut = hw - hw / 100 * SIFT_TAIL_RELOCATE_PCT;
	}
	pfree(ord);
	return found;
}

#endif							/* SIFT_F2_TAIL_PLAN_H */
