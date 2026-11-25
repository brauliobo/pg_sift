/*
 * col/v1/v1_reader_unit.c - what a stored unit of Format 1 answers without being read (col/v1/v1_reader.h): the zone and the exact sums of a leaf, and the histogram of a dimension, for the units
 * of an index and of a sift_table alike. A unit answers only while it stands for exactly the rows a snapshot sees: stored, no dead row, every heap page it has rows on all-visible to it.
 */
#include "postgres.h"

#include "col/v1/pagg.h"
#include "col/v1/v1_reader.h"

bool
sift_v1_unit_exact(const SiftColUnit *unit)
{
	SiftColSegment *seg;
	uint32		u;

	return sift_col_unit_stored(unit, &seg, &u) && sift_col_unit_exact(unit);
}

bool
sift_v1_unit_agg(const SiftColUnit *unit, int col, const SiftColDef *def, SiftSrcUnitAgg *agg)
{
	SiftCAgg	a;

	if (!sift_col_unit_agg(unit, col, &a))
		return false;
	*agg = (SiftSrcUnitAgg) {.nvals = a.nvals,.nnull = a.nnull,.has_nan = (a.zflags & SIFT_COLZ_NAN) != 0,.is_const = (a.zflags & SIFT_COLZ_CONST) != 0,
		.loose = (a.flags & SIFT_CAGG_LOOSE) != 0};
	agg->has_minmax = a.nnull < a.nvals && def->store != SIFT_COLS_STR && !(a.zflags & SIFT_COLZ_INLINE);
	if (def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8)
	{
		agg->min_f = sift_col_f8_from_key(a.min);
		agg->max_f = sift_col_f8_from_key(a.max);
		agg->sum_f = a.sum_f;
	}
	else
	{
		agg->min = a.min;
		agg->max = a.max;
	}
	if (a.has_sums)
	{
		agg->has_sums = !(a.zflags & SIFT_COLZ_OVERFLOW);
		agg->sum = a.sum;
		agg->sumsq = a.sumsq;
	}
	return true;
}

static void
cube_need(const SiftSrcRollupNeed *need, const int *col_of_leaf, SiftCRollupNeed *cn)
{
	cn->ndims = 0;
	cn->nmeas = need->nmeas;
	for (int m = 0; m < need->nmeas; m++)
	{
		cn->meas[m].leaf = col_of_leaf ? col_of_leaf[need->meas[m].leaf] : need->meas[m].leaf;
		cn->meas[m].sums = need->meas[m].sums;
		cn->meas[m].minmax = need->meas[m].minmax;
	}
}

bool
sift_v1_unit_hist(const SiftColUnit *unit, int col, const SiftColDef *def, MemoryContext mcxt, const int *col_of_leaf, const int *leaf_of_col, const SiftSrcRollupNeed *need,
				  SiftSrcDict *hdict, SiftSrcHist *h)
{
	SiftCHistDim d;
	SiftColDict dict;
	SiftCRollupNeed cn;

	cube_need(need, col_of_leaf, &cn);
	if (!sift_col_unit_hist(unit, col, mcxt, &cn, &d, &dict))
		return false;
	*h = (SiftSrcHist) {.ngroups = d.ngroups,.has_null = d.has_null,.key = d.key,.count = d.count,.nmeas = d.nmeas,.grain = d.leaf / SIFT_PAGG_CAL};
	for (int m = 0; m < d.nmeas; m++)
	{
		h->meas[m] = leaf_of_col ? leaf_of_col[d.meas[m]] : d.meas[m];
		if (h->meas[m] < 0)
			return false;
		h->nn[m] = d.nn[m];
		h->sum[m] = d.sum[m];
		h->sumf[m] = d.sumf[m];
		h->min[m] = d.min[m];
		h->max[m] = d.max[m];
	}
	if (def->store == SIFT_COLS_STR)
	{
		*hdict = (SiftSrcDict) {(const void *) (uintptr_t) dict.id, dict.n, dict.off, dict.bytes};
		h->dict = hdict;
	}
	return true;
}
