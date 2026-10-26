/*
 * agg/pre.c - the pre-aggregates of a column unit as the records of the seam (agg/pre.h).
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/pre.h"
#include "col/pagg.h"

void
sift_aggpre_spec(const SiftCPaggSpec *spec, SiftSrcPreagg *pa)
{
	pa->aggs = true;
	if (spec->off)
		return;
	for (int c = 0; c < SIFT_COL_MAX_COLS && pa->npres < SIFT_SRC_MAX_DIMS; c++)
		if (spec->pres[c])
			pa->pres[pa->npres++] = c;
	memcpy(pa->hll, spec->hll, sizeof(pa->hll));
	for (int d = 0; d < spec->ndims; d++)
	{
		pa->dim[pa->ndims].leaf = spec->dim[d].leaf;
		pa->dim[pa->ndims].nmeas = spec->dim[d].nmeas;
		pa->dim[pa->ndims].minmax = spec->minmax;
		pa->dim[pa->ndims].cal = spec->dim[d].cal;
		for (int m = 0; m < spec->dim[d].nmeas; m++)
			pa->dim[pa->ndims].meas[m] = spec->dim[d].meas[m];
		pa->ndims++;
	}
	for (int r = 0; r < spec->nrollups; r++)
	{
		const SiftCPaggRollup *ru = &spec->rollup[r];

		pa->rollup[pa->nrollups].list = ru->list;
		pa->rollup[pa->nrollups].ndims = ru->ndims;
		pa->rollup[pa->nrollups].nmeas = ru->nmeas;
		for (int j = 0; j < ru->ndims; j++)
			pa->rollup[pa->nrollups].dim[j] = ru->dim[j];
		for (int m = 0; m < ru->nmeas; m++)
			pa->rollup[pa->nrollups].meas[m] = ru->meas[m];
		pa->nrollups++;
	}
}

void
sift_aggpre_init(SiftAggPre *pre, MemoryContext mcxt)
{
	pre->cxt = mcxt;
	pre->pcx = NULL;
}

bool
sift_aggpre_unit_agg(const SiftColUnit *unit, int col, const SiftColDef *def, SiftSrcUnitAgg *agg)
{
	SiftCAgg	a;

	if (!sift_col_unit_agg(unit, col, &a))
		return false;
	memset(agg, 0, sizeof(*agg));
	agg->nvals = a.nvals;
	agg->nnull = a.nnull;
	agg->has_nan = (a.zflags & SIFT_COLZ_NAN) != 0;
	agg->is_const = (a.zflags & SIFT_COLZ_CONST) != 0;
	agg->loose = (a.flags & SIFT_CAGG_LOOSE) != 0;
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
		agg->has_sumsq = !(a.flags & SIFT_CAGG_SUMSQ_OVF);
		agg->sum = a.sum;
		agg->sumsq = a.sumsq;
	}
	return true;
}

bool
sift_aggpre_unit_hist(SiftAggPre *pre, const SiftColUnit *unit, int col, const SiftColDef *def, const int *leaf_of_col, SiftSrcHist *h)
{
	SiftCHistDim d;
	SiftColDict dict;

	if (pre->pcx == NULL)
		pre->pcx = AllocSetContextCreate(pre->cxt, "sift_aggpre histogram", ALLOCSET_DEFAULT_SIZES);
	MemoryContextReset(pre->pcx);
	if (!sift_col_unit_hist(unit, col, pre->pcx, &d, &dict))
		return false;
	memset(h, 0, sizeof(*h));
	h->ngroups = d.ngroups;
	h->has_null = d.has_null;
	h->key = d.key;
	h->count = d.count;
	h->nmeas = d.nmeas;
	h->grain = d.leaf / SIFT_PAGG_CAL;
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
		pre->hdict = (SiftSrcDict) {(const void *) (uintptr_t) dict.id, dict.n, dict.off, dict.bytes};
		h->dict = &pre->hdict;
	}
	return true;
}

bool
sift_aggpre_unit_presence(const SiftColUnit *unit, int col, SiftSrcPresence *p)
{
	SiftCPresence c;

	if (!sift_col_unit_presence(unit, col, &c))
		return false;
	*p = (SiftSrcPresence) {c.wbase, c.nwords, c.words, c.nkeys, c.keys};
	return true;
}

bool
sift_aggpre_run_info(const SiftAggSel *sel, const SiftSrcUnit *unit, const SiftColUnit *cu, int col, uint32 *nvals, uint32 *nnull, uint64 *id)
{
	return !sel->compact && !sel->keep && unit->nrow == cu->nrows && sift_col_run_info(cu, col, nvals, nnull, id);
}

int
sift_aggpre_run_heads(const SiftColUnit *unit, int col, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi)
{
	SiftCHeadEnt e[2 * SIFT_CRUN_HEAD_MAX];
	int			h = sift_col_run_heads(unit, col, e, nlo, nhi);

	StaticAssertStmt(sizeof(SiftSrcHead) == sizeof(SiftCHeadEnt) && SIFT_SRC_HEAD_MAX == SIFT_CRUN_HEAD_MAX, "the run head entries of the engine and the source differ");
	memcpy(lo, e, *nlo * sizeof(SiftSrcHead));
	memcpy(hi, e + *nlo, *nhi * sizeof(SiftSrcHead));
	return h;
}
