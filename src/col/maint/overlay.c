/* col/maint/overlay.c - the small segments (overlays) of a key-cut index that one merge takes together. */
#include "postgres.h"
#include "access/xlog.h"
#include "col/maint/maint_int.h"

typedef struct KeySpan
{
	int64		lo,
				hi;
} KeySpan;

static int
span_cmp(const void *a, const void *b)
{
	const KeySpan *x = a,
			   *y = b;

	return x->lo < y->lo ? -1 : x->lo > y->lo;
}

bool		maint_overlay_asked = false;

/*
 * The overlays of a key-cut index in partitions: the segments under half a partition, that promotions leave over the keys of the partitions (a partition holds a key in one
 * unit; each overlay holds it in another). The smallest of them whose bytes stay under half a partition, so that the merged one is an overlay too and not a partition whose
 * key range overlaps the others': a merge of the small tail into one. take marks them; false where fewer than two. ev: what the merge costs and what the reads would spare.
 * A unit holds a range of keys, so what a read of a key opens is told by the widths of the ranges of the units: the width of those of each overlay added is the units a random
 * key opens in them, the width of all of them together (overlaps once) the units it would open in the merged one.
 */
bool
maint_overlay_set(Relation index, RewriteView *v, bool *take, OverlayEv *ev)
{
	int			idx[SIFT_MAX_SEGMENTS],
				ncand = 0,
				cut;
	uint64		bytes = 0,
				limit = (uint64) maint_part_kb * 512,
				nspan = 0,
				total_units = 0;
	KeySpan    *sp;

	/* a merge writes about the bytes of its segments: an eighth of max_wal_size at most, so that it does not force a checkpoint of its own; the next pass takes the rest */
	limit = Min(limit, ((uint64) max_wal_size_mb << 20) / 8);
	memset(ev, 0, sizeof(*ev));
	if (maint_part_kb == 0 || v->nkover > 0 || !sift_col_is_index(index) || !sift_col_key_cut(sift_col_rel(index)))
		return false;
	cut = sift_col_rel(index)->cut[0];
	for (int i = 0; i < v->n; i++)
	{
		total_units += v->seg[i]->nsb;
		if (!maint_is_partition(&v->imm[i]) && !(v->imm[i].flags & SIFT_SEG_UNIT))
			idx[ncand++] = i;
	}
	if (ncand < 2)
		return false;
	/* by size, the smallest first */
	for (int a = 1; a < ncand; a++)
		for (int b = a; b > 0 && v->imm[idx[b - 1]].bytes > v->imm[idx[b]].bytes; b--)
		{
			int			t = idx[b];

			idx[b] = idx[b - 1];
			idx[b - 1] = t;
		}
	sp = palloc(Max(total_units, 1) * sizeof(KeySpan));
	for (int a = 0; a < ncand && bytes + v->imm[idx[a]].bytes < limit; a++)
	{
		SiftColSegment *seg = v->seg[idx[a]];

		bytes += v->imm[idx[a]].bytes;
		take[idx[a]] = true;
		ev->n++;
		ev->rows += v->imm[idx[a]].ndocs;
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			SiftCZone	z;

			sift_cseg_zone(seg, cut, u, &z);
			if (z.nvals == 0)
				continue;
			sp[nspan++] = (KeySpan) {z.min, z.max};
			ev->sum += (double) z.max - (double) z.min + 1;
		}
	}
	if (ev->n < 2 || nspan == 0)
	{
		memset(take, false, v->n);
		pfree(sp);
		return false;
	}
	qsort(sp, nspan, sizeof(KeySpan), span_cmp);
	for (uint64 k = 0, end = 0; k < nspan; k++)
	{
		int64		lo = sp[k].lo;

		if (k == 0 || lo > sp[end].hi)
		{
			end = k;
			ev->cover += (double) sp[k].hi - (double) lo + 1;
		}
		else if (sp[k].hi > sp[end].hi)
		{
			ev->cover += (double) sp[k].hi - (double) sp[end].hi;
			sp[end].hi = sp[k].hi;
		}
	}
	pfree(sp);
	return true;
}
