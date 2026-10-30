/*
 * agg/preagg/keep.c - the rows of a histogram the filters keep
 * Which cells of a histogram pass the conjuncts of the scan, by the zones of the span and by
 * the calendar.
 */
#include "postgres.h"

#include "col/preagg.h"
#include "agg/agg_int.h"
#include "agg/preagg/int.h"

/* the entries (groups of a histogram, tuples of a rollup) that pass the test p of qlevel, ANDed into keep (NULL: all pass so far): a bitmap, the vectors bound for the leaves p reads */
uint64 *
pred_pass(AggExec *ex, const SiftAggPred *p, int qlevel, int nt, uint64 *keep)
{
	if (keep == NULL)
	{
		keep = MemoryContextAlloc(ex->ucxt, ((nt + 63) / 64 + PADW) * sizeof(uint64));
		fill_ones(keep, nt);
	}
	for (int st = 0; st < nt; st += VEC)
	{
		int			n = Min(VEC, nt - st);
		uint64		t[VECW + PADW];

		eval_pred(ex, (SiftAggPred *) p, qlevel, st, n, t);
		sift_colk->bits_and(keep + st / 64, t, (n + 63) / 64);
	}
	return keep;
}

/*
 * The groups of the histogram that pass the conjuncts on the key's column (a bitmap; NULL: all), and for each aggregate the groups that pass its FILTER
 * when that tests the column (fkeep[ai]; NULL: all): the tests run on the groups as on the rows of a unit that has one per group, the column's vector
 * being the histogram's keys.
 */
uint64 *
preagg_keep(AggExec *ex, const SiftSrcHist *h, uint64 **fkeep)
{
	SiftAggSpec *s = ex->spec;
	int			ng = h->ngroups;
	uint64	   *keep = NULL;
	bool		wide = ex->lay->leaf[ex->pa.dim].vtype == SCV_I64,
				bound = false;

	for (int i = 0; i < s->npred + s->naggs; i++)
	{
		int			ai = i - s->npred;
		const SiftAggPred *p = ai < 0 ? s->pred[i] : s->agg[ai].filter;

		/* the comparisons on the leaf of a calendar dim are decided by the span of each cell, not by its first day (cal_keep) */
		if (p == NULL || sift_aggspec_pred_leaf(p, ex->lay, ex->nbase, s->group) != ex->pa.dim || (ex->pa.cal && sift_aggspec_span_pred(p, ex->lay, ex->nbase)))
			continue;
		if (!bound)
		{
			uint8	   *isnull = h->has_null ? MemoryContextAllocZero(ex->ucxt, ng + SIFT_AGG_PAD) : NULL;
			void	   *data = (void *) TYPEALIGN(SIFT_SIMD_ALIGN, MemoryContextAllocZero(ex->ucxt, ng * sizeof(int64) + SIFT_AGG_PAD + SIFT_SIMD_ALIGN));

			for (int k = 0; k < ng - h->has_null; k++)
			{
				/* a cell of a timestamp is tested at its first microsecond */
				if (wide)
					((int64 *) data)[k] = ex->pa.cal ? h->key[k] * USECS_PER_DAY : h->key[k];
				else
					((uint32 *) data)[k] = (uint32) h->key[k];
			}
			if (h->has_null)
			{
				isnull[ng - 1] = 1;
				if (!wide)
					((uint32 *) data)[ng - 1] = h->dict ? h->dict->n : 0;
			}
			ex->uvec[ex->pa.dim] = (SiftSrcVec) {data, isnull, h->dict, ng, NULL};
			ex->uvec_ok[ex->pa.dim] = true;
			bound = true;
		}
		if (ai < 0)
			keep = pred_pass(ex, p, s->group >= 0, ng, keep);
		else
			fkeep[ai] = pred_pass(ex, p, s->group >= 0, ng, NULL);
	}
	ex->uvec_ok[ex->pa.dim] = false;
	return keep;
}

/* what the span [first, last] of a cell does to the test z of the cell's leaf: 0 no row passes, 1 every row does, 2 some may (a bound cuts the cell) */
static int
span_test(const SiftSrcPrune *z, int leaf, int64 first, int64 last, bool top)
{
	if (z->nkids > 0)
	{
		int			res = z->any ? 0 : 1;

		for (int i = 0; i < z->nkids; i++)
		{
			int			r = span_test(&z->kids[i], leaf, first, last, false);

			if (r == (z->any ? 1 : 0))
				return r;
			if (r == 2)
				res = 2;
		}
		return res;
	}
	/* the NULL tests and the tests of another leaf say nothing of this one: a conjunct of its own is decided elsewhere, a disjunct of it leaves the cell open */
	if (z->nulltest || z->leaf != leaf)
		return top ? 1 : 2;
	if (last < z->lo || first > z->hi)
		return 0;
	if (z->nvals > 0)
	{
		bool		in = false;

		for (int i = 0; !in && i < z->nvals; i++)
			in = z->vals[i] >= first && z->vals[i] <= last;
		return !in ? 0 : first == last ? 1 : 2;
	}
	return first >= z->lo && last <= z->hi ? 1 : 2;
}

/*
 * The cells of the histogram of a calendar dim that the comparisons on its leaf keep, ANDed into *keep (NULL: all pass so far). A cell is decided by its
 * whole span, first to last instant: inside the tests (comparisons, and the ANDs and ORs of them) it passes, outside it does not, and the NULL group never
 * does. False: a bound cuts a cell, the cell's rows differ, and the unit is scanned.
 */
bool
cal_keep(AggExec *ex, const SiftSrcHist *h, uint64 **keep)
{
	bool		date = ex->lay->leaf[ex->pa.dim].vtype == SCV_I32;
	int			ng = h->ngroups,
				nk = ng - h->has_null;

	/* the entries of the leaves the conjuncts compare, then those of the ORs (prune_init) */
	for (int z = 0; z < ex->nzone; z++)
	{
		const SiftSrcPrune *pz = &ex->prune[z];

		if (pz->nulltest || (pz->nkids == 0 && pz->leaf != ex->pa.dim))
			continue;
		if (*keep == NULL)
		{
			*keep = MemoryContextAlloc(ex->ucxt, ((ng + 63) / 64 + PADW) * sizeof(uint64));
			fill_ones(*keep, ng);
		}
		for (int g = 0; g < ng; g++)
		{
			int64		first = h->key[g],
						next = sift_cpagg_cal_next(h->grain, first),
						last;
			int			r;

			if (g >= nk)
			{
				(*keep)[g / 64] &= ~(UINT64CONST(1) << (g % 64));
				continue;
			}
			if (date)
				last = next - 1;
			else
			{
				first *= USECS_PER_DAY;
				last = next > PG_INT64_MAX / USECS_PER_DAY ? PG_INT64_MAX : next * USECS_PER_DAY - 1;
			}
			r = span_test(pz, ex->pa.dim, first, last, true);
			if (r == 0)
				(*keep)[g / 64] &= ~(UINT64CONST(1) << (g % 64));
			else if (r == 2)
				return false;
		}
	}
	return true;
}
