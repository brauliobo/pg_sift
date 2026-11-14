/*
 * col/maint/view.c - the immutable segments of a directory, open, for the rule of the rewrite and the choice of what it takes
 * the superblocks and key ranges they hold, which of them are overlays, and which hold found flags a rewrite would fill again
 */
#include "postgres.h"
#include "col/maint/maint_int.h"

SbSpans
maint_seg_spans(SiftColSegment *seg)
{
	SbSpans		sp = {0};
	bool		key_cut = sift_col_is_index(seg->index) && sift_col_key_cut(sift_col_rel(seg->index));

	if (!key_cut)
	{
		sp.n = sift_cseg_sb_spans(seg, &sp.r);
		sp.by_tid = sift_cseg_keyed(&seg->entry);
		return sp;
	}
	/* the units of a key-cut index are told by their sequence, as they always were: the rows of a key range lie in any block */
	{
		SiftCSbSpan *r = palloc(Max(seg->nsb, 1) * sizeof(SiftCSbSpan));

		for (uint32 u = 0; u < seg->nsb; u++)
			if (sp.n > 0 && seg->sbdir[u].id <= r[sp.n - 1].hi + 1)
				r[sp.n - 1].hi = Max(r[sp.n - 1].hi, seg->sbdir[u].id);
			else
				r[sp.n++] = (SiftCSbSpan) {seg->sbdir[u].id, seg->sbdir[u].id};
		sp.r = r;
	}
	return sp;
}

/* Do the two segments hold rows of more than `allowed` superblocks in common? */
bool
maint_share_superblock(const SbSpans *a, const SbSpans *b, int allowed)
{
	uint64		shared = 0;

	for (uint32 i = 0, j = 0; i < a->n && j < b->n;)
	{
		uint32		lo = Max(a->r[i].lo, b->r[j].lo),
					hi = Min(a->r[i].hi, b->r[j].hi);

		if (lo <= hi && (shared += (uint64) hi - lo + 1) > allowed)
			return true;
		if (a->r[i].hi <= b->r[j].hi)
			i++;
		else
			j++;
	}
	return false;
}

RewriteView *
maint_view_open(Relation index, const SiftDirSnapshot *snap, MemoryContext cx)
{
	RewriteView *v = MemoryContextAllocZero(cx, sizeof(RewriteView));
	SiftConnStale *conn = sift_conn_stale_begin(index, cx);

	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			v->imm[v->n] = snap->segs[i];
			v->seg[v->n] = sift_cseg_open(index, &v->imm[v->n], cx);
			v->span[v->n] = maint_seg_spans(v->seg[v->n]);
			v->over[v->n] = !maint_is_partition(&v->imm[v->n]);
			v->stale[v->n] = conn != NULL && sift_conn_stale(conn, v->seg[v->n]);
			v->n++;
		}
	for (int i = 0; i < v->n; i++)
		for (int j = 0; j < v->n && !v->over[i]; j++)
		{
			/* rows per unit, compared as products; the older of two alike is the one to keep. A segment of the floor's units is an overlay by its size alone */
			uint64		di = v->imm[i].ndocs * v->seg[j]->nsb,
						dj = v->imm[j].ndocs * v->seg[i]->nsb;

			v->over[i] = j != i && !v->span[j].by_tid && (dj > di || (dj == di && v->imm[j].gen > v->imm[i].gen)) && maint_share_superblock(&v->span[i], &v->span[j], 0);
		}
	if (maint_part_kb > 0 && sift_col_is_index(index) && sift_col_key_cut(sift_col_rel(index)))
		maint_mark_key_overlap(index, v);
	if (conn != NULL)
		sift_conn_stale_end(conn);
	return v;
}
