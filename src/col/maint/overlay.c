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

/*
 * A merge of overlays is a size-tiered step: the merged one is at least twice the largest it takes, so that a row is rewritten once for each doubling and not at every promotion.
 * The reads of a key open a unit in each overlay: when more than OVERLAYS_ALIVE_MAX of them are alive (a read index only: overlay_worth) the smallest are merged whatever their
 * sizes, as far as a pass allows.
 */
#define OVERLAY_GROWTH_PCT	200
#define OVERLAYS_ALIVE_MAX	4

bool		maint_overlay_asked = false;
int			maint_overlay_steps = 0;	/* the steps of the rewrite in hand left for the overlays: the automatic one takes a pass and the rule asks again for the next, an explicit col_maintain goes on until they are one */

/*
 * The overlays of a key-cut index: the segments that promotions leave over the keys of the large ones (a large segment holds a key in one unit; each overlay holds it in another).
 * With partitions (sift.col_auto_merge_partition_kb) the large ones are the partitions; else the segments of at least half the largest. The merge takes the smallest overlays that
 * come to at least one and a half times the largest of them (a size-tiered step: a row is rewritten once for each growth by half and not at every promotion), their bytes under half a large segment, so that
 * the merged one is an overlay too, and under a pass of the WAL (an eighth of max_wal_size, and sift.col_auto_merge_wal_pct percent of it). take marks them; false where fewer than
 * two. ev: what the merge costs and what the reads would spare.
 * A unit holds a range of keys, so what a read of a key opens is told by the widths of the ranges of the units: the width of those of each overlay added is the units a random
 * key opens in them, the width of all of them together (overlaps once) the units it would open in the merged one.
 */
bool
maint_overlay_set(Relation index, RewriteView *v, bool *take, OverlayEv *ev)
{
	int			idx[SIFT_MAX_SEGMENTS],
				ncand = 0,
				ntake = 0,
				least = 2,
				cut;
	uint64		bytes = 0,
				large = 0,
				limit,
				nspan = 0,
				total_units = 0;
	KeySpan    *sp;

	memset(ev, 0, sizeof(*ev));
	if (v->nkover > 0 || !sift_col_is_index(index) || !sift_col_key_cut(sift_col_rel(index)))
		return false;
	cut = sift_col_rel(index)->cut[0];
	for (int i = 0; i < v->n; i++)
		large = Max(large, v->imm[i].bytes);
	for (int i = 0; i < v->n; i++)
	{
		total_units += v->seg[i]->nsb;
		if (!(v->imm[i].flags & SIFT_SEG_UNIT) && (maint_part_kb > 0 ? !maint_is_partition(&v->imm[i]) : v->imm[i].bytes * 2 < large))
			idx[ncand++] = i;
	}
	if (ncand < 2)
		return false;
	limit = maint_part_kb > 0 ? (uint64) maint_part_kb * 512 : large / 2;
	/* a merge writes about the bytes of its segments: a pass takes an eighth of max_wal_size at most, and sift.col_auto_merge_wal_pct percent of it, so that it forces no checkpoint of its own; the next pass takes the rest */
	limit = Min(limit, ((uint64) max_wal_size_mb << 20) / 8);
	if (sift_col_auto_merge_wal_pct > 0)
		limit = Min(limit, (uint64) ((double) ((uint64) max_wal_size_mb << 20) * sift_col_auto_merge_wal_pct / 100.0));
	/* by size, the smallest first */
	for (int a = 1; a < ncand; a++)
		for (int b = a; b > 0 && v->imm[idx[b - 1]].bytes > v->imm[idx[b]].bytes; b--)
		{
			int			t = idx[b];

			idx[b] = idx[b - 1];
			idx[b - 1] = t;
		}
	for (int a = 0; a < ncand && bytes + v->imm[idx[a]].bytes < limit; a++)
	{
		bytes += v->imm[idx[a]].bytes;
		ntake++;
	}
	ev->forced = ncand > OVERLAYS_ALIVE_MAX;
	if (ev->forced)
		least = Min(ntake, ncand - OVERLAYS_ALIVE_MAX + 1);
	while (ntake > least && bytes * 100 < OVERLAY_GROWTH_PCT * v->imm[idx[ntake - 1]].bytes)
		bytes -= v->imm[idx[--ntake]].bytes;
	if (ntake < 2 || (!ev->forced && bytes * 100 < OVERLAY_GROWTH_PCT * v->imm[idx[ntake - 1]].bytes))
		return false;
	sp = palloc(Max(total_units, 1) * sizeof(KeySpan));
	for (int a = 0; a < ntake; a++)
	{
		SiftColSegment *seg = v->seg[idx[a]];

		take[idx[a]] = true;
		ev->n++;
		ev->rows += v->imm[idx[a]].ndocs;
		ev->units += seg->nsb;
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

/* The range of the key leaf over the units of a key-cut segment */
void
maint_seg_key_range(SiftColSegment *seg, int leaf, int64 *lo, int64 *hi)
{
	*lo = PG_INT64_MAX;
	*hi = PG_INT64_MIN;
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		SiftCZone	z;

		sift_cseg_zone(seg, leaf, u, &z);
		if (z.nvals == 0)
			continue;
		*lo = Min(*lo, z.min);
		*hi = Max(*hi, z.max);
	}
}

/* Does the key range of segment i overlap another one's */
bool
maint_key_meets(const RewriteView *v, int i)
{
	for (int j = 0; j < v->n; j++)
		if (j != i && v->klo[i] < v->khi[j] && v->klo[j] < v->khi[i])
			return true;
	return false;
}

/*
 * The segments of a key-cut index that the partitions of a rewrite are not: a rewrite cuts the key range into partitions that share at most a boundary key, so a key
 * is in one of them; the runs of a build that outgrew the memory of a segment writer are as large as partitions but each holds rows of every key (a lookup of a key
 * reads a unit in each). They are overlays, which the rewrite merges.
 */
void
maint_mark_key_overlap(Relation index, RewriteView *v)
{
	int			leaf = sift_col_rel(index)->cut[0];

	for (int i = 0; i < v->n; i++)
		maint_seg_key_range(v->seg[i], leaf, &v->klo[i], &v->khi[i]);
	/* a segment whose key range no other one meets is a partition of the key whatever its size (the short end of a cut at the memory of a writer): it is no overlay for a rewrite to take */
	for (int i = 0; i < v->n; i++)
		v->over[i] &= maint_key_meets(v, i);
	/* the rewrite right after a build is the build's own (its choices stand): the runs are for the next one */
	for (int i = 0; i < v->n && !maint_after_build; i++)
		for (int j = i + 1; j < v->n; j++)
			if (maint_is_partition(&v->imm[i]) && maint_is_partition(&v->imm[j]) && v->klo[i] < v->khi[j] && v->klo[j] < v->khi[i])
				v->kover[i] = v->kover[j] = true;
	for (int i = 0; i < v->n; i++)
		v->nkover += v->kover[i];
}
