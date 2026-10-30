/*
 * agg/preagg/fold.c - the stored values of a unit folded into the groups
 * One value into the state of an aggregate, the zones and bounds of a unit, and the stored
 * sums, counts and presence folded into the groups.
 */
#include "postgres.h"

#include "utils/float.h"

#include "agg/agg_int.h"
#include "agg/preagg/int.h"

/* v into the state of aggregate ai in group g, as the rows it stands for would have been added */
static void
preagg_add(AggExec *ex, int ai, uint32 g, const PaVal *v)
{
	const SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];

	switch (a->kind)
	{
		case SAA_COUNT_STAR:
			acc->n[g] += v->rows;
			break;
		case SAA_COUNT_DISTINCT:		/* its values are added by the histogram's fold (preagg_hist) */
		case SAA_GEN:				/* its sketch is merged by preagg_hll */
			break;
		case SAA_COUNT:
			acc->n[g] += v->nn;
			break;
		case SAA_SUM_INT:
		case SAA_AVG_INT:
			if (v->sum != (int64) v->sum || pg_add_s64_overflow(acc->i[g], (int64) v->sum, &acc->i[g]))
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
			acc->n[g] += v->nn;
			break;
		case SAA_SUM_NUM:
		case SAA_AVG_NUM:
			{
				sift_i128	mul,
							add;

				sift_aggnum_linear(&a->num, &mul, &add);
				agg_sum_add(acc, g, mul * v->sum + add * v->nn);
			}
			acc->n[g] += v->nn;
			break;
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			acc->d[g] += v->sumf;
			acc->n[g] += v->nn;
			break;
		default:					/* SAA_MIN, SAA_MAX */
			if (v->has_mm)
			{
				bool		ismin = a->kind == SAA_MIN;
				int64		x = ismin ? v->min : v->max;
				sift_i128	cur = a->num.n ? acc->w[g] : acc->i[g];

				if (ex->lay->leaf[pa_leaf(a)].vtype == SCV_F64)
				{
					double		f = ismin ? v->minf : v->maxf;

					if (acc->n[g] == 0 || (ismin ? float8_cmp_internal(f, acc->d[g]) < 0 : float8_cmp_internal(f, acc->d[g]) > 0))
						acc->d[g] = f;
				}
				else if (acc->n[g] == 0 || (ismin ? x < cur : x > cur))
				{
					if (a->num.n)
						acc->w[g] = x;
					else
						acc->i[g] = x;
				}
				acc->n[g] += v->nn;
			}
	}
}

/* group g takes rows rows, the aggregates their values v[] */
void
preagg_fold(AggExec *ex, uint32 g, int64 rows, const PaVal *v)
{
	ex->seen[g] += rows;
	for (int ai = 0; ai < ex->spec->naggs; ai++)
		preagg_add(ex, ai, g, &v[ai]);
}

/* count(*) and nothing else, over rows of a unit that all pass, one group */
void
preagg_count(AggExec *ex, int64 rows)
{
	PaVal	   *v = MemoryContextAllocZero(ex->ucxt, ex->spec->naggs * sizeof(PaVal));

	for (int ai = 0; ai < ex->spec->naggs; ai++)
		v[ai].rows = v[ai].nn = rows;
	preagg_fold(ex, 0, rows, v);
}

/* a dim of the rollup of the rows: its tests run on the tuples, not on the zone of the unit */
bool
rollup_dim_leaf(const AggExec *ex, int leaf)
{
	if (ex->pa.rows)
		for (int d = 0; d < ex->pa.ndims; d++)
			if (ex->pa.rdim[d] == leaf)
				return true;
	return false;
}

/* the unit's zone of every leaf the conjuncts compare lies inside the comparison, and holds no NULL: they hold for all the rows */
bool
preagg_zones(AggExec *ex)
{
	ex->pa_xleaf = -1;
	for (int k = 0; k < ex->nprune; k++)
	{
		const SiftSrcPrune *z = &ex->prune[k];
		SiftSrcUnitAgg a;

		if (rollup_dim_leaf(ex, z->leaf))
			continue;

		/*
		 * a test of the NULLs holds for all the rows of the unit when its zone counts none (IS NOT NULL) or only NULLs (IS NULL), and no NaN stands in for one. A
		 * unit with some rows that are not NULL answers IS NULL as its histograms minus the exceptions it kept for them (one leaf: pa_xleaf, see hist_net)
		 */
		if (z->nulltest)
		{
			if (ex->pa.nullz)
			{
				bool		known = sift_colsrc_unit_agg(ex->src, &ex->unit, z->leaf, &a);

				if (!known || a.has_nan || (z->nulltest == SSN_NOTNULL ? a.nnull > 0 : a.nnull < a.nvals))
				{
					if (!known || a.has_nan || z->nulltest != SSN_ISNULL || (ex->pa_xleaf >= 0 && ex->pa_xleaf != z->leaf) || a.nnull == a.nvals)
						return false;
					ex->pa_xleaf = z->leaf;
				}
			}
			continue;
		}

		/* the groups of the dim test the comparisons on its leaf, the cells of a calendar dim by their span (cal_keep) */
		if (z->leaf != ex->pa.dim &&
			(!sift_colsrc_unit_agg(ex->src, &ex->unit, z->leaf, &a) || a.nnull > 0 || !a.has_minmax || a.min < z->lo || a.max > z->hi))
			return false;
		/* the bounds of an IN list hold for all the rows only when the unit has one value and the list has it */
		if (z->leaf != ex->pa.dim && z->nvals > 0)
		{
			bool		in = false;

			for (int i = 0; !in && i < z->nvals; i++)
				in = a.min == a.max && z->vals[i] == a.min;
			if (!in)
				return false;
		}
	}
	return true;
}

/* false when the zone of a leaf the conjuncts compare, as bounded with the dead rows in it, cannot lie inside the comparison: preagg_zones fails whatever the dead rows are (the unit's rollup of the rows and the dim's histogram test the comparisons themselves) */
bool
preagg_zone_bounds(AggExec *ex)
{
	if (ex->pa.hdim >= 0)
		return true;
	for (int k = 0; k < ex->nprune; k++)
	{
		const SiftSrcPrune *z = &ex->prune[k];
		SiftSrcUnitAgg a;

		if (!z->nulltest && z->leaf != ex->pa.dim && !rollup_dim_leaf(ex, z->leaf) &&
			sift_colsrc_unit_agg(ex->src, &ex->unit, z->leaf, &a) && (!a.has_minmax || a.min < z->lo || a.max > z->hi))
			return false;
	}
	return true;
}

/*
 * count(DISTINCT leaf) of a leaf of key_columns: the bitmap or list of the unit's values added to the aggregate's set. False when the unit has none; the values
 * added so far do no harm, the scan of the unit adds them again to a set.
 */
static bool
preagg_presence(AggExec *ex, int ai, int leaf)
{
	SiftSrcPresence p;

	if (!sift_colsrc_unit_presence(ex->src, &ex->unit, leaf, &p))
		return false;
	if (p.nkeys > 0)
	{
		for (uint32 i = 0; i < p.nkeys; i++)
			ex->acc[ai].n[0] += dist_add(ex, ex->dist[ai], 0, (uint32) p.keys[i]);
		pfree((int64 *) p.keys);
	}
	else
		ex->acc[ai].n[0] += dist_or_words(ex, ex->dist[ai], 0, p.wbase, p.words, p.nwords);
	return true;
}

/* sift.approx_count_distinct of a leaf: the unit's stored sketch merged into the aggregate's state (the merge is idempotent, like the sets of preagg_presence) */
static bool
preagg_hll(AggExec *ex, int ai, int leaf)
{
	const uint8 *blob;

	if (!sift_colsrc_unit_hll(ex->src, &ex->unit, leaf, &blob))
		return false;
	gen_merge_hll(ex, ai, blob);
	return true;
}

/* no key: the unit's rows are one group, from the sums of the leaves */
bool
preagg_sums(AggExec *ex)
{
	PaVal	   *v = MemoryContextAllocZero(ex->ucxt, Max(ex->spec->naggs, 1) * sizeof(PaVal));
	int			nrow = ex->unit.nrow;

	/* the sums of the leaves are of all the rows of the unit; the exceptions keep histograms and rollups only */
	if (ex->pa_xleaf >= 0)
		return false;
	for (int ai = 0; ai < ex->spec->naggs; ai++)
	{
		const SiftAggAgg *a = &ex->spec->agg[ai];
		int			leaf = pa_leaf(a);
		SiftSrcUnitAgg ua;

		v[ai].rows = v[ai].nn = nrow;
		if (leaf < 0)
			continue;
		if (a->kind == SAA_COUNT_DISTINCT)
		{
			if (!preagg_presence(ex, ai, leaf))
				return false;
			continue;
		}
		if (a->kind == SAA_GEN)
		{
			if (!preagg_hll(ex, ai, leaf))
				return false;
			continue;
		}
		if (!sift_colsrc_unit_agg(ex->src, &ex->unit, leaf, &ua) || (a->kind != SAA_COUNT && a->kind != SAA_MIN && a->kind != SAA_MAX && !ua.has_sums) ||
			(ua.loose && (a->kind == SAA_MIN || a->kind == SAA_MAX)))
			return false;
		v[ai] = (PaVal) {nrow, ua.nvals - ua.nnull, ua.sum, ua.has_minmax, ua.min, ua.max};
	}
	preagg_fold(ex, 0, nrow, v);
	return true;
}
