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

/* A move of the pieces above a cut copies about as many blocks as the file gets shorter by, so it is made at half the share the rewrite of a whole part waits for */
#define SIFT_TAIL_PIECES_PCT	(SIFT_TAIL_RELOCATE_PCT / 2)

/* The most extents the free space below a cut may be cut into for a move that copies the blocks above it: a free space in more pieces would give the part more extents than the file gains */
#define SIFT_TAIL_MAX_PIECES	4096

/* The blocks of the extents at or above cut */
static inline uint64
sift_tail_blocks_from(const SiftExtent *ext, int n, BlockNumber cut)
{
	uint64		blocks = 0;

	for (int i = 0; i < n; i++)
		if ((uint64) ext[i].start + ext[i].nblocks > cut)
			blocks += (uint64) ext[i].start + ext[i].nblocks - Max(ext[i].start, cut);
	return blocks;
}

static inline int
sift_tail_ext_desc(const void *a, const void *b)
{
	const SiftExtent *x = a,
			   *y = b;

	return (x->start < y->start) - (x->start > y->start);
}

/*
 * The lowest block, not below floor, at or above which at most want blocks of the extents lie: the parts keep what is under it and move the rest, so that the file can be cut there. A
 * part whose top extent is larger than the free space is not moved whole: the extent is cut at the block, the piece above it is copied below.
 */
static inline BlockNumber
sift_tail_cut_for(const SiftExtent *ext, int n, uint64 want, BlockNumber floor)
{
	SiftExtent *ord = memcpy(palloc(Max(n, 1) * sizeof(SiftExtent)), ext, n * sizeof(SiftExtent));
	BlockNumber cut = 0;

	if (n > 1)
		qsort(ord, n, sizeof(SiftExtent), sift_tail_ext_desc);
	for (int i = 0; i < n; i++)
	{
		cut = ord[i].start;
		if (ord[i].nblocks > want)
		{
			cut = ord[i].start + ord[i].nblocks - (BlockNumber) want;
			break;
		}
		want -= ord[i].nblocks;
	}
	pfree(ord);
	return Max(cut, floor);
}

/* Whether the file ends the share SIFT_TAIL_PIECES_PCT shorter (and by min_gap blocks) when nothing of the parts stays at or above cut */
static inline bool
sift_tail_cut_saves(BlockNumber hw, BlockNumber cut, BlockNumber min_gap)
{
	return cut < hw && hw - cut >= min_gap && (uint64) (hw - cut) * 100 >= (uint64) hw * SIFT_TAIL_PIECES_PCT;
}

/* What a holder is for the pieces of its stream to be moved: the extents of the stream (none: the holder is not moved piece by piece), the end of its header and extent-list pages and of its deletion vector */
typedef struct SiftTailPart
{
	const SiftExtent *stream;
	int			nstream;
	BlockNumber meta_hi;
	BlockNumber dv_hi;
} SiftTailPart;

static inline BlockNumber
sift_tail_stream_hi(const SiftTailPart *p)
{
	BlockNumber hi = 0;

	for (int i = 0; i < p->nstream; i++)
		hi = Max(hi, p->stream[i].start + p->stream[i].nblocks);
	return hi;
}

/*
 * The block the file is cut at when the pieces of the streams that lie at or above it are copied into free blocks below it, from the lowest that the free space (free_blocks) can take them to
 * and that the others allow: whatever is not moved piece by piece holds the file to its end, a deletion vector is never moved, and the header of a part none of whose stream is moved stays
 * where it is. A cut under `from` is not asked for (the free space under it was found too scattered). No cut when no holder reaches the end of the file: what holds it is not a part (a
 * metadata slab), and moving pieces would not shorten the file. Whether the file ends enough shorter at the cut.
 */
static inline bool
sift_tail_pick_cut(const SiftTailSpan *span, const SiftTailPart *part, int n, BlockNumber hw, uint64 free_blocks, BlockNumber min_gap, BlockNumber from, BlockNumber *cut)
{
	BlockNumber floor = from,
				c;
	int			nall = 0,
				k = 0;
	bool		holds_end = false;
	SiftExtent *all;

	for (int i = 0; i < n; i++)
	{
		floor = Max(floor, part[i].nstream > 0 ? part[i].dv_hi : span[i].hi);
		nall += part[i].nstream;
		holds_end |= span[i].hi == hw;
	}
	if (!holds_end)
		return false;
	all = palloc(Max(nall, 1) * sizeof(SiftExtent));
	for (int i = 0; i < n; i++)
		for (int j = 0; j < part[i].nstream; j++)
			all[k++] = part[i].stream[j];
	c = sift_tail_cut_for(all, nall, free_blocks, floor);
	pfree(all);
	for (bool raised = true; raised;)
	{
		raised = false;
		for (int i = 0; i < n; i++)
			if (part[i].nstream > 0 && sift_tail_stream_hi(&part[i]) <= c && part[i].meta_hi > c)
			{
				c = part[i].meta_hi;
				raised = true;
			}
	}
	*cut = c;
	return sift_tail_cut_saves(hw, c, min_gap);
}

#endif							/* SIFT_F2_TAIL_PLAN_H */
