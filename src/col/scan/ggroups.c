/*
 * col/scan/ggroups.c - the values of the group column of a scan that reads the groups of a second order one after the other (an index-only scan ordered by the group and the key)
 */
#include "postgres.h"
#include "core/cmp.h"
#include "col/scan_int.h"
#include "col/scan/gmem.h"

/* a stored segment: the groups of its table, read one by one from `at` in the direction of the scan (the table is not held, whatever its size) */
typedef struct GroupSeg
{
	int			slot;
	uint32		ngroups;
	int64		at;
	int64		head;
	bool		ok;
} GroupSeg;

struct SiftColGroups
{
	SiftColSource *src;
	bool		desc;
	bool		single;			/* the one group of g = value */
	int64		only;
	GroupSeg   *seg;
	int64	   *mem;			/* the values of the write buffers, ascending, once each: the buffers are bounded by write_buffer_limit */
	int64		memat;
	uint32		nmem;
};

static void
seg_head(SiftColGroups *c, GroupSeg *s, int j)
{
	uint32		vs,
				ve,
				ns,
				ne;

	s->ok = s->at >= 0 && s->at < (int64) s->ngroups;
	if (s->ok)
		sift_cseg_gorder_group_at(c->src->seg[j].seg, s->slot, (uint32) s->at, &s->head, &vs, &ve, &ns, &ne);
}

/*
 * The cursor over the values of the leaf gcol that the rows of the scan have, once each, ascending (descending: from the largest): the groups of the second order of leaf kcol grouped by gcol in every
 * stored segment (a block of every row, so that the table of groups lists every value) and the rows of the write buffers, merged as they are asked for. NULL when a segment has no such block, or a
 * buffer holds an overflow or no value of gcol: the order of the pair is not there to read. With only, the one group it names is the result (a scan with g = value reads that group alone).
 */
SiftColGroups *
sift_col_gorder_groups_open(SiftColSource *src, int gcol, int kcol, const int64 *only, bool desc, MemoryContext cxt)
{
	SiftColGroups *c;
	uint64		memrows = 0,
				freerows = 0;
	int			nfree = 0;

	for (int j = 0; j < src->nseg; j++)
	{
		int			slot = sift_cseg_gorder_slot(src->seg[j].seg, kcol, true);
		uint32		entries,
					values;

		/* a segment without a block of the leaf (a small overlay) gives its groups from its units, as the write buffers do */
		if (!sift_cseg_gorder_has(src->seg[j].seg, kcol))
		{
			nfree++;
			freerows += src->seg[j].seg->entry.ndocs;
			continue;
		}
		if (slot < 0 || sift_cseg_gorder_group(src->seg[j].seg, slot) != gcol || !sift_cseg_gorder(src->seg[j].seg, slot, &entries, &values) || entries != (uint32) src->seg[j].seg->entry.ndocs)
			return NULL;
	}
	if (nfree == src->nseg || freerows > SIFT_CGORD_FREE_CAP(false))
		return NULL;
	if (!src->mem_loaded)
		scan_load_wbuf(src);
	memrows = freerows;
	for (int i = 0; i < src->nmem; i++)
	{
		const SiftColBatch *b = src->mem[i].b;
		const SiftCVals *gv = &b->col[gcol];

		for (uint32 r = 0; r < b->nrows; r++)
			if (sift_col_bit_test(gv->ovf, r) || sift_col_bit_test(gv->nulls, r))
				return NULL;
		memrows += b->nrows;
	}
	c = MemoryContextAllocZero(cxt, sizeof(SiftColGroups));
	c->src = src;
	c->desc = desc;
	if (only != NULL)
	{
		c->single = true;
		c->only = *only;
		return c;
	}
	c->seg = MemoryContextAllocZero(cxt, Max(src->nseg, 1) * sizeof(GroupSeg));
	for (int j = 0; j < src->nseg; j++)
	{
		GroupSeg   *s = &c->seg[j];

		if (!sift_cseg_gorder_has(src->seg[j].seg, kcol))
			continue;
		s->slot = sift_cseg_gorder_slot(src->seg[j].seg, kcol, true);
		s->ngroups = sift_cseg_gorder_ngroups(src->seg[j].seg, s->slot);
		s->at = desc ? (int64) s->ngroups - 1 : 0;
		seg_head(c, s, j);
	}
	if (memrows > 0)
	{
		uint64		m = 0;

		c->mem = MemoryContextAllocHuge(cxt, memrows * sizeof(int64));
		for (int i = 0; i < src->nmem; i++)
		{
			const SiftColBatch *b = src->mem[i].b;

			for (uint32 r = 0; r < b->nrows; r++)
				c->mem[m++] = b->col[gcol].key[r];
		}
		for (int j = 0; j < src->nseg; j++)
			if (!sift_cseg_gorder_has(src->seg[j].seg, kcol) && !gord_free_group_values(src->seg[j].seg, gcol, c->mem, &m))
				return NULL;
		qsort(c->mem, m, sizeof(int64), sift_cmp_i64);
		for (uint64 i = 0; i < m; i++)
			if (c->nmem == 0 || c->mem[c->nmem - 1] != c->mem[i])
				c->mem[c->nmem++] = c->mem[i];
		c->memat = desc ? (int64) c->nmem - 1 : 0;
	}
	return c;
}

/* The next value of the groups in the direction of the scan, once each; false after the last */
bool
sift_col_gorder_groups_next(SiftColGroups *c, int64 *group)
{
	int			step = c->desc ? -1 : 1;
	bool		have = false;
	int64		best = 0;

	if (c->single)
	{
		*group = c->only;
		c->single = false;
		return true;
	}
	if (c->seg == NULL)
		return false;
	for (int j = 0; j < c->src->nseg; j++)
		if (c->seg[j].ok && (!have || (c->desc ? c->seg[j].head > best : c->seg[j].head < best)))
		{
			best = c->seg[j].head;
			have = true;
		}
	if (c->memat >= 0 && c->memat < (int64) c->nmem && (!have || (c->desc ? c->mem[c->memat] > best : c->mem[c->memat] < best)))
	{
		best = c->mem[c->memat];
		have = true;
	}
	if (!have)
		return false;
	for (int j = 0; j < c->src->nseg; j++)
		if (c->seg[j].ok && c->seg[j].head == best)
		{
			c->seg[j].at += step;
			seg_head(c, &c->seg[j], j);
		}
	if (c->memat >= 0 && c->memat < (int64) c->nmem && c->mem[c->memat] == best)
		c->memat += step;
	*group = best;
	return true;
}
