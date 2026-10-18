/*
 * sift_merge.c - the tiered merge policy shared by the hosts: segments of similar size merge together (DESIGN.md (c)).
 */
#include "sift_host.h"

#include "sift_merge.h"

static int
tier_of(const SiftSegEntry *s, const SiftMergePolicy *pol)
{
	uint64		r = s->bytes / Max(pol->tier_base, 1);
	int			t = 0;

	for (; r >= (uint64) pol->fanin; r /= pol->fanin)
		t++;
	return t;
}

static int
bytes_cmp(const void *a, const void *b)
{
	return pg_cmp_u64(((const SiftSegEntry *) a)->bytes, ((const SiftSegEntry *) b)->bytes);
}

/* A segment of which more than half the documents were deleted */
static bool
mostly_dead(Relation index, const SiftSegEntry *e)
{
	SiftSegment *seg;
	bool		dead;

	if (!(e->flags & SIFT_SEG_DIRTY))
		return false;
	seg = sift_seg_open(index, e, CurrentMemoryContext);
	dead = sift_seg_live_docs(seg) * 2 < e->ndocs;
	sift_seg_close(seg);
	return dead;
}

int
sift_merge_pick(Relation index, const SiftMergePolicy *pol, const SiftDirSnapshot *snap, bool force, SiftSegEntry *sel)
{
	SiftSegEntry imm[SIFT_MAX_SEGMENTS];
	int			n = 0,
				fanin = pol->fanin;

	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			imm[n++] = snap->segs[i];
	qsort(imm, n, sizeof(SiftSegEntry), bytes_cmp);
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
	for (int i = 0; i < n; i++)
		if (mostly_dead(index, &imm[i]))
		{
			sel[0] = imm[i];
			return 1;
		}
	return 0;
}
