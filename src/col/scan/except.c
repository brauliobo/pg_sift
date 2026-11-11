/*
 * col/scan/except.c - the stored rows of a scope source that the scan leaves out: those of the keys whose pairs changed since the scope segments were written (col/scan/scope_delta.c)
 */
#include "postgres.h"

#include "core/cmp.h"
#include "col/seg.h"
#include "col/scan_int.h"

/* is key one of the members the source leaves out: a bit of the bitmap over their range, a bisection when the range is too wide for one */
static bool
sx_has(const SiftColSource *src, int64 key)
{
	uint64		off = (uint64) key - (uint64) src->sx[0];
	uint32		lo = 0,
				hi = src->nsx;

	if (src->sxb != NULL)
		return off < src->sx_span && sift_col_bit_test(src->sxb, off);
	SIFT_BISECT(uint32, m, lo, hi, src->sx[m] < key);
	return lo < src->nsx && src->sx[lo] == key;
}

/*
 * The stored unit of a scope source (sift_col_scope_delta) whose rows include a member the source leaves out: those rows are dead to the scan, as if their liveness bit were clear (cu->live
 * is the selection of the others), and the unit answers from neither its pre-aggregates nor the net of its dead rows. The zone of the member leaf rules most units out.
 */
bool
unit_except(ColUnit *cu)
{
	SiftColSource *src = cu->src;
	SiftCZone	z;
	SiftCChunk	c;
	int64	   *val;
	uint32		n = cu->v.nrows,
				lo = 0,
				hi = src->nsx;
	bool		any = false;

	if (cu->seg == NULL)
		return false;
	sift_cseg_zone(cu->seg, src->sx_leaf, cu->u, &z);
	if (!(z.flags & SIFT_COLZ_OVERFLOW))
	{
		if (z.nvals == z.nnull)
			return false;
		SIFT_BISECT(uint32, m, lo, hi, src->sx[m] < z.min);
		if (lo == src->nsx || src->sx[lo] > z.max)
			return false;
	}
	if (!unit_chunk_raw(cu, src->sx_leaf, &c))
		return false;
	val = palloc((c.n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));
	sift_cchunk_decode(&c, val);
	for (uint32 r = 0; r < n; r++)
	{
		if (!sx_has(src, val[r]) || (c.nulls != NULL && sift_col_bit_test(c.nulls, r)))
			continue;
		if (!any)
		{
			cu->live = MemoryContextAllocZero(src->arena, SIFT_COL_NWORDS(n) * sizeof(uint64));
			sift_cseg_live(cu->seg, cu->u, cu->live);
			any = true;
		}
		cu->live[r / 64] &= ~(UINT64CONST(1) << (r % 64));
	}
	pfree(val);
	cu->excl = any;
	return any;
}
