/*
 * col/seg/gorder_group.c - the rows of one group of a second order (a global order grouped by another leaf) between two keys: the group's run
 * in the table of groups, the two bounds as seeks among its entries
 */
#include "core/host.h"
#include "core/cmp.h"
#include "col/seg_int.h"
#include "col/gquant.h"

/*
 * Is the second order of leaf col (a complete block grouped by leaf gcol) there to read: the slot to read it through, -1 when the segment has none
 */
static int
gorder_group_view(SiftColSegment *seg, int col, int gcol)
{
	int			slot = sift_cseg_gorder_slot(seg, col, true);
	GOrdLeaf   *g = slot < 0 ? NULL : seg_gord_leaf(seg, slot);

	return g != NULL && g->hdr.gcol == gcol && g->hdr.vtotal == 0 && seg_gord_probe_ok(seg, g, slot) ? slot : -1;
}

/* Does leaf col have a second order grouped by leaf gcol (sift_cseg_gorder_tids_group) */
bool
sift_cseg_gorder_grouped_by(SiftColSegment *seg, int col, int gcol)
{
	return gorder_group_view(seg, col, gcol) >= 0;
}

/*
 * The entries [*a, *b) of the second order of leaf col (slot *slot) that belong to `group` of leaf gcol and whose key lies in [lo, hi]: the group is found in the table of groups, the two bounds among
 * the entries of its run. False when the segment has no such block.
 */
static bool
gorder_group_span(SiftColSegment *seg, int col, int gcol, int64 group, int64 lo, int64 hi, int *slot, uint32 *a, uint32 *b, SiftCGOrdBuf *memo)
{
	uint32		vs,
				ve,
				ns,
				ne;

	*slot = gorder_group_view(seg, col, gcol);
	if (*slot < 0)
		return false;
	*a = *b = 0;
	sift_cseg_gorder_range(seg, *slot, group, &vs, &ve, &ns, &ne);
	if (lo <= hi && vs < ve)
	{
		*a = seg_gord_seek(seg, *slot, vs, ve, lo, false, memo);
		*b = seg_gord_seek(seg, *slot, *a, ve, hi, true, memo);
	}
	return true;
}

/* How many rows of one group have a key in [lo, hi]; UINT32_MAX when the segment has no second order of the leaf by that group */
uint32
sift_cseg_gorder_count_group(SiftColSegment *seg, int col, int gcol, int64 group, int64 lo, int64 hi)
{
	SiftCGOrdBuf memo;
	uint32		a,
				b;
	int			slot;

	memo.at = UINT32_MAX;
	return gorder_group_span(seg, col, gcol, group, lo, hi, &slot, &a, &b, &memo) ? b - a : UINT32_MAX;
}

/* The heap addresses of the rows of one group whose key lies in [lo, hi], ascending; false when the segment has no such block or the range holds more than limit entries */
bool
sift_cseg_gorder_tids_group(SiftColSegment *seg, int col, int gcol, int64 group, int64 lo, int64 hi, uint32 limit, SiftKey **tids, uint32 *ntids)
{
	SiftCGOrdBuf memo;
	uint32		a,
				b,
				n = 0;
	int			slot;
	SiftKey    *t;

	memo.at = UINT32_MAX;
	if (!gorder_group_span(seg, col, gcol, group, lo, hi, &slot, &a, &b, &memo) || b - a > limit)
		return false;
	t = palloc(Max(b - a, 1) * sizeof(SiftKey));
	seg_gord_addrs(seg, slot, a, b, &memo, t, &n);
	if (n > 1)
		sift_sort_u64(t, n);
	*tids = t;
	*ntids = n;
	return true;
}

/* fn(arg, address) for each row of the group in the order of the keys, a chunk at a time (sift_cseg_gorder_each of a group); false when there is no such block or more than limit entries */
bool
sift_cseg_gorder_each_group(SiftColSegment *seg, int col, int gcol, int64 group, int64 lo, int64 hi, uint32 limit, void (*fn) (void *arg, SiftKey tid), void *arg)
{
	SiftCGOrdBuf memo;
	uint32		a,
				b,
				crows;
	int			slot;

	memo.at = UINT32_MAX;
	if (!gorder_group_span(seg, col, gcol, group, lo, hi, &slot, &a, &b, &memo) || b - a > limit)
		return false;
	crows = seg_gord_leaf(seg, slot)->hdr.crows;
	for (uint32 c = a / crows; a < b && c <= (b - 1) / crows; c++)
	{
		uint32		from = Max(a, c * crows),
					to = Min(b, (c + 1) * crows);

		(void) seg_gord_chunk_memo(seg, slot, c, &memo);
		for (uint32 i = from; i < to; i++)
			fn(arg, memo.addr[i - c * crows]);
	}
	return true;
}
