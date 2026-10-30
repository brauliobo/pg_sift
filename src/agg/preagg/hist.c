/*
 * agg/preagg/hist.c - the histograms of a unit folded into the groups
 * The floats a histogram stores, the net of a histogram and of a rollup, and the fold of a
 * histogram by the key of its dimension.
 */
#include "postgres.h"

#include "utils/float.h"

#include "agg/agg_int.h"
#include "agg/preagg/int.h"

/* the values of a float8 leaf of the unit are finite and so small that the sum of their squares, which avg(float8) is owed to check, cannot overflow */
#define PA_FLOAT_MAX	1e100

bool
preagg_floats(AggExec *ex, int leaf, bool bounded)
{
	SiftSrcUnitAgg a;

	return sift_colsrc_unit_agg(ex->src, &ex->unit, leaf, &a) && !a.has_nan &&
		(!bounded || a.nnull == a.nvals || (a.has_minmax && fabs(a.min_f) <= PA_FLOAT_MAX && fabs(a.max_f) <= PA_FLOAT_MAX));
}

/*
 * The unit's histogram of dim, and its rollup of a list, as the rows the conjuncts of the scan keep: with an IS NULL that the zone could not decide
 * (pa_xleaf), net of the rows the unit kept the exceptions of. False for a unit that has none of that leaf, lacks the dim or whose float sums would cancel: it is
 * scanned (counted in exc_scan).
 */
static bool
hist_net(AggExec *ex, int dim, SiftSrcHist *h)
{
	bool		ok;

	if (ex->pa_xleaf >= 0 && !sift_colsrc_unit_except(ex->src, &ex->unit, ex->pa_xleaf))
		return false;
	ok = sift_colsrc_unit_hist(ex->src, &ex->unit, dim, h);
	if (ex->pa_xleaf >= 0)
		sift_colsrc_unit_except(ex->src, &ex->unit, -1);
	return ok;
}

bool
rollup_net(AggExec *ex, SiftSrcRollup *r)
{
	bool		ok;

	if (ex->pa_xleaf >= 0 && !sift_colsrc_unit_except(ex->src, &ex->unit, ex->pa_xleaf))
		return false;
	ok = sift_colsrc_unit_rollup(ex->src, &ex->unit, ex->pa.list, &ex->pa_need, r);
	if (ex->pa_xleaf >= 0)
		sift_colsrc_unit_except(ex->src, &ex->unit, -1);
	return ok;
}

/* the groups of the unit's histogram of the key's column, or of the column the conjuncts test (no key: one group) */
bool
preagg_hist(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcHist h;
	int		   *slot = MemoryContextAlloc(ex->ucxt, Max(s->naggs, 1) * sizeof(int));
	PaVal	   *v = MemoryContextAllocZero(ex->ucxt, Max(s->naggs, 1) * sizeof(PaVal));
	uint32	   *kept,
			   *ids,
			   *gid;
	uint64	   *keep,
			  **fkeep = MemoryContextAllocZero(ex->ucxt, Max(s->naggs, 1) * sizeof(uint64 *));
	int			nkept = 0;

	if (!hist_net(ex, ex->pa.dim + (ex->pa.cal ? SIFT_SRC_CAL : 0), &h))
		return false;
	/* the cells of a grain the key or the tests are not constant over, or no cells (the unit had too many, or an infinity): the unit is scanned */
	if (ex->pa.cal && (h.grain <= 0 || !(ex->pa.calmask >> h.grain & 1)))
		return false;
	for (int ai = 0; ai < s->naggs; ai++)
	{
		int			leaf = pa_leaf(&s->agg[ai]);
		int			m = 0;
		bool		mm = s->agg[ai].kind == SAA_MIN || s->agg[ai].kind == SAA_MAX;

		while (leaf >= 0 && leaf != ex->pa.dim && m < h.nmeas && h.meas[m] != leaf)
			m++;
		/* a unit written without min and max (an older segment, an index that does not ask for them) is scanned */
		if (leaf >= 0 && leaf != ex->pa.dim &&
			(m == h.nmeas || (mm ? h.min[m] == NULL : s->agg[ai].kind != SAA_COUNT && h.sum[m] == NULL && h.sumf[m] == NULL)))
			return false;
		if (leaf >= 0 && (s->agg[ai].kind == SAA_SUM_F64 || s->agg[ai].kind == SAA_AVG_F64) && !preagg_floats(ex, leaf, true))
			return false;
		slot[ai] = leaf < 0 ? -1 : s->agg[ai].kind == SAA_COUNT_DISTINCT ? -3 : leaf == ex->pa.dim ? -2 : m;
	}
	keep = preagg_keep(ex, &h, fkeep);
	if (ex->pa.cal && !cal_keep(ex, &h, &keep))
		return false;
	kept = MemoryContextAlloc(ex->ucxt, Max(h.ngroups, 1) * sizeof(uint32));
	for (uint32 k = 0; k < h.ngroups; k++)
		if (keep == NULL || sift_colsrc_bit(keep, k))
			kept[nkept++] = k;
	ids = MemoryContextAllocZero(ex->ucxt, Max(nkept, 1) * sizeof(uint32));
	gid = MemoryContextAllocZero(ex->ucxt, Max(nkept, 1) * sizeof(uint32));
	if (s->nkeys == 1)
		preagg_key_ids(ex, 0, h.key, NULL, h.ngroups - h.has_null, h.dict, kept, nkept, ids);
	ids_to_gids(ex, nkept, ids, gid);
	for (int j = 0; j < nkept; j++)
	{
		for (int ai = 0; ai < s->naggs; ai++)
		{
			int			m = slot[ai];

			v[ai].rows = v[ai].nn = h.count[kept[j]];
			if (m == -3)
			{
				/* the group is one value: a NULL group adds none, any other adds its id (the value for a 32-bit leaf, the id over all dictionaries for text) */
				if (kept[j] < h.ngroups - h.has_null && h.count[kept[j]] > 0)
				{
					uint32		id = h.dict ? amap_id(ex, dict_amap(ex, dict_slot(ex, h.dict), h.dict, ai), h.dict, ai, (uint32) h.key[kept[j]]) : (uint32) h.key[kept[j]];

					if (dist_add(ex, ex->dist[ai], gid[j], id))
						ex->acc[ai].n[gid[j]]++;
				}
				v[ai] = (PaVal) {0};
				continue;
			}
			if (m == -2)
				v[ai].nn = kept[j] < h.ngroups - h.has_null ? h.count[kept[j]] : 0;
			else if (m >= 0)
			{
				v[ai].nn = h.nn[m][kept[j]];
				v[ai].sum = h.sum[m] ? h.sum[m][kept[j]] : 0;
				v[ai].sumf = h.sumf[m] ? h.sumf[m][kept[j]] : 0;
				if (h.min[m])
				{
					v[ai].has_mm = v[ai].nn > 0;
					v[ai].min = h.min[m][kept[j]];
					v[ai].max = h.max[m][kept[j]];
				}
			}
			if (fkeep[ai] && !sift_colsrc_bit(fkeep[ai], kept[j]))
				v[ai] = (PaVal) {0};				/* the rows of the group fail the FILTER: they add nothing to this aggregate */
		}
		preagg_fold(ex, gid[j], h.count[kept[j]], v);
	}
	return true;
}
