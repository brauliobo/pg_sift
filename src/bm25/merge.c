/*
 * bm25/merge.c - the tiered merge policy shared by the hosts: segments of similar size merge together (DESIGN.md (c)).
 */
#include "sift_host.h"

#include "bm25/merge.h"

static int
tier_of(const SiftSegEntry *s, const SiftMergePolicy *pol)
{
	uint64		r = s->bytes / Max(pol->tier_base, 1);
	int			t = 0;

	for (; r >= (uint64) pol->fanin; r /= pol->fanin)
		t++;
	return t;
}

int
sift_seg_bytes_cmp(const void *a, const void *b)
{
	return pg_cmp_u64(((const SiftSegEntry *) a)->bytes, ((const SiftSegEntry *) b)->bytes);
}

/* A segment of which more than dead_pct percent of the documents were deleted */
static bool
mostly_dead(Relation index, const SiftSegEntry *e, int dead_pct)
{
	SiftSegment *seg;
	bool		dead;

	if (!(e->flags & SIFT_SEG_DIRTY))
		return false;
	seg = sift_seg_open(index, e, CurrentMemoryContext);
	dead = sift_seg_live_docs(seg) * 100 < (uint64) e->ndocs * (100 - dead_pct);
	sift_seg_close(seg);
	return dead;
}

/* The immutable segments of snap, smallest first */
static int
immutables(const SiftDirSnapshot *snap, SiftSegEntry *imm)
{
	int			n = 0;

	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			imm[n++] = snap->segs[i];
	qsort(imm, n, sizeof(SiftSegEntry), sift_seg_bytes_cmp);
	return n;
}

static int
pick_dead(Relation index, const SiftMergePolicy *pol, const SiftSegEntry *imm, int n, SiftSegEntry *sel)
{
	for (int i = 0; i < n; i++)
		if (pol->mostly_dead ? pol->mostly_dead(index, &imm[i]) : mostly_dead(index, &imm[i], pol->dead_pct ? pol->dead_pct : SIFT_MERGE_DEAD_PCT))
		{
			sel[0] = imm[i];
			return 1;
		}
	return 0;
}

int
sift_merge_pick_dead(Relation index, const SiftMergePolicy *pol, const SiftDirSnapshot *snap, SiftSegEntry *sel)
{
	SiftSegEntry imm[SIFT_MAX_SEGMENTS];

	return pick_dead(index, pol, imm, immutables(snap, imm), sel);
}

int
sift_merge_pick(Relation index, const SiftMergePolicy *pol, const SiftDirSnapshot *snap, bool force, SiftSegEntry *sel)
{
	SiftSegEntry imm[SIFT_MAX_SEGMENTS];
	int			n = immutables(snap, imm),
				fanin = pol->fanin;

	if (force)
	{
		memcpy(sel, imm, n * sizeof(SiftSegEntry));
		return n > 1 || (n == 1 && (imm[0].flags & SIFT_SEG_DIRTY)) ? n : 0;
	}
	for (int i = 0; i + fanin <= n; i++)
		if (tier_of(&imm[i], pol) == tier_of(&imm[i + fanin - 1], pol))
		{
			int			k = 0;

			for (int j = i; j < n && tier_of(&imm[j], pol) == tier_of(&imm[i], pol) && k < 2 * fanin; j++)
				sel[k++] = imm[j];
			return k;
		}
	if (snap->nsegs > SIFT_MERGE_ENTRIES(fanin) && n >= 2)
	{
		memcpy(sel, imm, Min(n, fanin) * sizeof(SiftSegEntry));
		return Min(n, fanin);
	}
	return pick_dead(index, pol, imm, n, sel);
}
