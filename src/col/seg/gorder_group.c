/*
 * col/seg/gorder_group.c - the rows of one group of a second order (a global order grouped by another leaf) between two keys: the group's run
 * in the table of groups, the two bounds as seeks among its entries
 */
#include "core/cmp.h"
#include "col/seg_int.h"

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

/*
 * The heap addresses of every row of the groups of set (all keys, NULL keys too) from the second order of any leaf grouped by gcol, ascending: a group is its run in the table of groups,
 * so the leaf that is the group needs no order of its own to be looked up. False when no leaf has such a block or the groups hold more than limit entries.
 */
bool
sift_cseg_gorder_tids_groups(SiftColSegment *seg, int gcol, const int64 *set, uint32 nset, uint32 limit, SiftKey **tids, uint32 *ntids)
{
	SiftCGOrdBuf memo;
	uint32		total = 0,
				n = 0;
	int			slot = -1;
	SiftKey    *t;

	for (int col = 0; col < seg->schema->ncols && slot < 0; col++)
		slot = gorder_group_view(seg, col, gcol);
	if (slot < 0)
		return false;
	for (uint32 k = 0; k < nset; k++)
	{
		uint32		vs,
					ve,
					ns,
					ne;

		sift_cseg_gorder_range(seg, slot, set[k], &vs, &ve, &ns, &ne);
		total += (ve - vs) + (ne - ns);
		if (total > limit)
			return false;
	}
	memo.at = UINT32_MAX;
	t = palloc(Max(total, 1) * sizeof(SiftKey));
	for (uint32 k = 0; k < nset; k++)
	{
		uint32		vs,
					ve,
					ns,
					ne;

		sift_cseg_gorder_range(seg, slot, set[k], &vs, &ve, &ns, &ne);
		seg_gord_addrs(seg, slot, vs, ve, &memo, t, &n);
		seg_gord_addrs(seg, slot, ns, ne, &memo, t, &n);
	}
	if (n > 1)
		sift_sort_u64(t, n);
	*tids = t;
	*ntids = n;
	return true;
}

/* The group at place gi of the table of groups of block slot */
static int64
group_value(SiftColSegment *seg, int slot, uint32 gi)
{
	int64		g;
	uint32		vs,
				ve,
				ns,
				ne;

	sift_cseg_gorder_group_at(seg, slot, gi, &g, &vs, &ve, &ns, &ne);
	return g;
}

/*
 * The heap addresses of every row of the groups in [lo, hi] (all keys, NULL keys too) as sift_cseg_gorder_tids_groups: the table of groups is sorted by group and the value runs, like the
 * NULL-key runs, lie in the order of the groups, so the groups of a range are one span of each.
 */
bool
sift_cseg_gorder_tids_group_range(SiftColSegment *seg, int gcol, int64 lo, int64 hi, uint32 limit, SiftKey **tids, uint32 *ntids)
{
	SiftCGOrdBuf memo;
	int64		g;
	uint32		a = 0,
				b,
				end,
				vs = 0,
				ve = 0,
				ns = 0,
				ne = 0,
				n = 0;
	int			slot = -1;
	SiftKey    *t;

	for (int col = 0; col < seg->schema->ncols && slot < 0; col++)
		slot = gorder_group_view(seg, col, gcol);
	if (slot < 0)
		return false;
	end = sift_cseg_gorder_ngroups(seg, slot);
	b = end;
	SIFT_BISECT(uint32, mid, a, b, group_value(seg, slot, mid) < lo);
	b = a;
	SIFT_BISECT(uint32, mid, b, end, group_value(seg, slot, mid) <= hi);
	if (a < b)
	{
		uint32		x,
					y;

		sift_cseg_gorder_group_at(seg, slot, a, &g, &vs, &x, &ns, &y);
		sift_cseg_gorder_group_at(seg, slot, b - 1, &g, &x, &ve, &y, &ne);
	}
	if ((ve - vs) + (ne - ns) > limit)
		return false;
	memo.at = UINT32_MAX;
	t = palloc(Max((ve - vs) + (ne - ns), 1) * sizeof(SiftKey));
	seg_gord_addrs(seg, slot, vs, ve, &memo, t, &n);
	seg_gord_addrs(seg, slot, ns, ne, &memo, t, &n);
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
