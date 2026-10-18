/*
 * sift_aggsel.c - the rows of a column unit that a match set keeps (sift_aggsel.h).
 */
#include "postgres.h"

#include "sift_aggsel.h"

/* Few kept rows are decoded one by one; many are decoded with the whole chunk, and the mask keeps the others out */
#define SEL_SPARSE	8

void
sift_aggsel_init(SiftAggSel *a, MemoryContext mcxt)
{
	memset(a, 0, sizeof(*a));
	a->mcxt = mcxt;
}

bool
sift_aggsel_unit(SiftAggSel *a, const SiftColUnit *cu, uint32 *nrow)
{
	sift_col_sel_all(cu, &a->sel);
	a->nrow = a->sel.n;
	if (!sift_col_filter_tids(cu, a->keep, &a->sel))
	{
		a->units_skipped++;
		return false;
	}
	a->compact = (uint64) a->sel.count * SEL_SPARSE <= a->nrow;
	a->units_compact += a->compact;
	*nrow = a->compact ? a->sel.count : a->nrow;
	if (a->compact)
	{
		if (a->idxcap < a->sel.count + SIFT_COL_IDXPAD)
		{
			if (a->idx)
				pfree(a->idx);
			a->idxcap = Max(a->sel.count + SIFT_COL_IDXPAD, 1024);
			a->idx = MemoryContextAlloc(a->mcxt, a->idxcap * sizeof(uint32));
		}
		sift_col_sel_idx(&a->sel, a->idx);
	}
	return true;
}

void
sift_aggsel_read(SiftAggSel *a, const SiftColUnit *cu, int col, SiftColVec *vec)
{
	if (a->keep && a->compact)
		sift_col_read_idx(cu, col, a->idx, a->sel.count, vec);
	else
		sift_col_read(cu, col, NULL, vec);
}

void
sift_aggsel_live(SiftAggSel *a, const SiftColUnit *cu, uint64 *bits)
{
	SiftColSel	all;

	if (a->keep && a->compact)
	{
		memset(bits, 0, ((a->sel.count + 63) / 64) * sizeof(uint64));
		for (uint32 i = 0; i < a->sel.count; i++)
			bits[i >> 6] |= UINT64CONST(1) << (i & 63);
		return;
	}
	if (a->keep)
	{
		memcpy(bits, a->sel.bits, a->sel.nwords * sizeof(uint64));
		return;
	}
	sift_col_sel_all(cu, &all);
	memcpy(bits, all.bits, all.nwords * sizeof(uint64));
}

/* Runs op on the unit as stored: a compacted unit's rows are expanded to their stored ordinals, op restricts them, and the survivors are compressed back into bits */
void
sift_aggsel_stored(SiftAggSel *a, SiftColSource *src, const SiftColUnit *cu, uint64 *bits, SiftAggSelOp op, void *arg)
{
	SiftColSel	vs;
	const uint32 *idx = a->idx;

	if (a->keep && a->compact)
	{
		sift_col_sel_alloc(src, a->nrow, &vs);
		for (uint32 i = 0; i < a->sel.count; i++)
			if (bits[i >> 6] >> (i & 63) & 1)
				vs.bits[idx[i] >> 6] |= UINT64CONST(1) << (idx[i] & 63);
		sift_col_sel_count(&vs);
		op(cu, &vs, arg);
		for (uint32 i = 0; i < a->sel.count; i++)
			if (!(vs.bits[idx[i] >> 6] >> (idx[i] & 63) & 1))
				bits[i >> 6] &= ~(UINT64CONST(1) << (i & 63));
		return;
	}
	sift_col_sel_alloc(src, a->keep ? a->nrow : cu->nrows, &vs);
	memcpy(vs.bits, bits, vs.nwords * sizeof(uint64));
	sift_col_sel_count(&vs);
	op(cu, &vs, arg);
	memcpy(bits, vs.bits, vs.nwords * sizeof(uint64));
}

static void
vet_op(const SiftColUnit *cu, SiftColSel *vs, void *arg)
{
	sift_col_vet(cu, vs);
}

void
sift_aggsel_vet(SiftAggSel *a, SiftColSource *src, const SiftColUnit *cu, uint64 *bits)
{
	if (!sift_col_unit_allvisible(cu))
		sift_aggsel_stored(a, src, cu, bits, vet_op, NULL);
}
