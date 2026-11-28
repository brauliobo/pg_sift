/*
 * col/v1/reader_unit.c - what a stored unit of Format 1 answers without being read (col/v1/reader.h): the zone and the exact sums of a leaf, and the histogram of a dimension, for the units
 * of an index and of a sift_table alike. A unit answers only while it stands for exactly the rows a snapshot sees: stored, every heap page it has rows on all-visible to it, and no dead
 * row or the pre-aggregates VACUUM wrote net of exactly its dead rows (col/v1/seg/net.c).
 */
#include "postgres.h"

#include "col/v1/pagg.h"
#include "col/v1/reader.h"

int
sift_v1_unit_rows(SiftColUnit *unit)
{
	SiftColSegment *seg;
	uint32		u;

	if (unit->nsu > 0)
		return (int) unit->nrows;
	if (!sift_col_unit_stored(unit, &seg, &u))
		return -1;
	if (sift_col_unit_exact(unit))
		return (int) unit->nrows;
	if (sift_col_unit_dead(unit, 0, false) >= 0 && sift_col_unit_net(unit))
		return (int) (unit->nrows - sift_col_unit_net_dead(unit));
	sift_col_unit_dead(unit, -1, false);
	return -1;
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

/* The columns of need: the measures, and the dims of a rollup (with_dims; a histogram has its one) */
static void
cube_need(const SiftSrcRollupNeed *need, const int *col_of_leaf, bool with_dims, SiftCRollupNeed *cn)
{
	cn->ndims = with_dims ? need->ndims : 0;
	for (int j = 0; j < cn->ndims; j++)
		cn->dim[j] = col_of_leaf ? col_of_leaf[need->dim[j]] : need->dim[j];
	cn->nmeas = need->nmeas;
	for (int m = 0; m < need->nmeas; m++)
	{
		cn->meas[m].leaf = col_of_leaf ? col_of_leaf[need->meas[m].leaf] : need->meas[m].leaf;
		cn->meas[m].sums = need->meas[m].sums;
		cn->meas[m].minmax = need->meas[m].minmax;
	}
}

static void
src_dict(const SiftColDict *d, SiftSrcDict *out)
{
	*out = (SiftSrcDict) {(const void *) (uintptr_t) d->id, d->n, d->off, d->bytes};
}

/* The histogram d of the columns of a schema as one of the leaves of a source; false when a measure is no leaf of it */
static bool
hist_of(const SiftCHistDim *d, const int *leaf_of_col, SiftSrcHist *h)
{
	*h = (SiftSrcHist) {.ngroups = d->ngroups,.has_null = d->has_null,.key = d->key,.count = d->count,.nmeas = d->nmeas,.grain = d->leaf / SIFT_PAGG_CAL};
	for (int m = 0; m < d->nmeas; m++)
	{
		h->meas[m] = leaf_of_col ? leaf_of_col[d->meas[m]] : d->meas[m];
		if (h->meas[m] < 0)
			return false;
		h->nn[m] = d->nn[m];
		h->sum[m] = d->sum[m];
		h->sumf[m] = d->sumf[m];
		h->min[m] = d->min[m];
		h->max[m] = d->max[m];
	}
	return true;
}

bool
sift_v1_unit_hist(const SiftColUnit *unit, int col, const SiftColDef *def, MemoryContext mcxt, const int *col_of_leaf, const int *leaf_of_col, const SiftSrcRollupNeed *need,
				  SiftSrcDict *hdict, SiftSrcHist *h)
{
	SiftCHistDim d;
	SiftColDict dict;
	SiftCRollupNeed cn;

	cube_need(need, col_of_leaf, false, &cn);
	if (!sift_col_unit_hist(unit, col, mcxt, &cn, &d, &dict) || !hist_of(&d, leaf_of_col, h))
		return false;
	if (def->store == SIFT_COLS_STR)
	{
		src_dict(&dict, hdict);
		h->dict = hdict;
	}
	return true;
}

bool
sift_v1_unit_rollup(const SiftColUnit *unit, MemoryContext mcxt, const int *col_of_leaf, const int *leaf_of_col, const SiftSrcRollupNeed *need, SiftSrcDict *rdict, SiftSrcRollup *r)
{
	SiftCRollup c;
	SiftColDict dict[SIFT_PAGG_MAX_RDIMS];
	SiftCRollupNeed cn;

	cube_need(need, col_of_leaf, true, &cn);
	if (!sift_col_unit_rollup(unit, mcxt, &cn, &c, dict))
		return false;
	*r = (SiftSrcRollup) {.ntuples = c.ntuples,.ndims = c.ndims,.count = c.count,.nmeas = c.nmeas};
	for (int j = 0; j < c.ndims; j++)
	{
		if ((r->dim[j] = leaf_of_col ? leaf_of_col[c.dim[j]] : c.dim[j]) < 0)
			return false;
		r->key[j] = c.key[j];
		r->isnull[j] = c.isnull[j];
		if (dict[j].off != NULL)
		{
			src_dict(&dict[j], &rdict[j]);
			r->dict[j] = &rdict[j];
		}
	}
	for (int m = 0; m < c.nmeas; m++)
	{
		if ((r->meas[m] = leaf_of_col ? leaf_of_col[c.meas[m]] : c.meas[m]) < 0)
			return false;
		r->nn[m] = c.nn[m];
		r->sum[m] = c.sum[m];
		r->sumf[m] = c.sumf[m];
		r->min[m] = c.min[m];
		r->max[m] = c.max[m];
		r->minf[m] = c.minf[m];
		r->maxf[m] = c.maxf[m];
	}
	return true;
}

bool
sift_v1_unit_presence(const SiftColUnit *unit, int col, MemoryContext mcxt, SiftSrcPresence *p)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftCPresence c;
	const bool	has = sift_col_unit_presence(unit, col, &c);

	MemoryContextSwitchTo(old);
	if (has)
		*p = (SiftSrcPresence) {c.wbase, c.nwords, c.words, c.nkeys, c.keys};
	return has;
}

bool
sift_v1_seg_hist(const SiftColUnit *unit, int col, SiftSrcHist *h)
{
	const SiftCHistDim *d = sift_col_unit_seg_ktab(unit, col);

	return d != NULL && hist_of(d, NULL, h);
}
