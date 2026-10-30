/*
 * agg/preagg/rollup.c - the rollups of a unit folded into the groups
 * Which rollup answers the scan, whether it is cheaper than the rows, and its cells folded
 * into the groups.
 */
#include "postgres.h"

#include "agg/agg_int.h"
#include "agg/preagg/int.h"

/*
 * Units in a row that must cost at least a quarter more from the rollup than from the elements before the next ones are scanned without even looking
 * at them (their exactness, their rollup), and how many are skipped before one is looked at again (a clear scan then skips the next ones at once): the units of an index look alike, and
 * looking costs buffer reads that a scan the cost chose anyway does not need.
 */
#define PA_SCAN_RUN		3

#define PA_SCAN_RESAMPLE	16

/* the position of leaf among the dims of the rollup, -1 if it holds none */
static int
rollup_dim(const SiftSrcRollup *r, int leaf)
{
	for (int j = 0; j < r->ndims; j++)
		if (r->dim[j] == leaf)
			return j;
	return -1;
}

/* the dims of the rollup as the vectors of the tuples, which the element tests read as they read the elements' */
static void
rollup_bind(AggExec *ex, const SiftSrcRollup *r)
{
	int			nt = r->ntuples;

	for (int d = 0; d < ex->pa.ndims; d++)
	{
		int			j = rollup_dim(r, ex->pa.rdim[d]);
		uint32	   *data = (uint32 *) TYPEALIGN(SIFT_SIMD_ALIGN, MemoryContextAllocZero(ex->ucxt, nt * sizeof(uint32) + SIFT_AGG_PAD + SIFT_SIMD_ALIGN));

		for (int t = 0; t < nt; t++)
			data[t] = r->isnull[j] && r->isnull[j][t] ? (r->dict[j] ? r->dict[j]->n : 0) : (uint32) r->key[j][t];
		ex->uvec[ex->pa.rdim[d]] = (SiftSrcVec) {data, r->isnull[j], r->dict[j], nt, NULL};
		ex->uvec_ok[ex->pa.rdim[d]] = true;
	}
}

static bool
in_dims(const SiftSrcRollupNeed *n, int leaf)
{
	for (int d = 0; d < n->ndims; d++)
		if (n->dim[d] == leaf)
			return true;
	return false;
}

/* the columns of the rollup the aggregates read: the dims of the shape, the measure of each aggregate with the parts of it the aggregate adds up */
void
rollup_need(AggExec *ex)
{
	SiftSrcRollupNeed *n = &ex->pa_need;
	const SiftAggSpec *s = ex->spec;

	memset(n, 0, sizeof(*n));
	n->ndims = ex->pa.ndims;
	memcpy(n->dim, ex->pa.rdim, ex->pa.ndims * sizeof(int));
	for (int ai = 0; ai < s->naggs; ai++)
	{
		const SiftAggAgg *a = &s->agg[ai];
		int			leaf = pa_leaf(a),
					m = 0;

		if (leaf < 0)
			continue;
		/* count(leaf) of a dim reads the NULLs of the dim, of a measure its non-NULL counts: ask for both, the blob has the one it has */
		if (a->kind == SAA_COUNT && !in_dims(n, leaf))
		{
			if (n->ndims == lengthof(n->dim))
				n->all = true;
			else
				n->dim[n->ndims++] = leaf;
		}
		while (m < n->nmeas && n->meas[m].leaf != leaf)
			m++;
		if (m == n->nmeas)
		{
			if (n->nmeas == lengthof(n->meas))
			{
				n->all = true;
				return;
			}
			n->meas[n->nmeas++].leaf = leaf;
		}
		n->meas[m].sums |= a->kind != SAA_COUNT && a->kind != SAA_MIN && a->kind != SAA_MAX;
		n->meas[m].minmax |= a->kind == SAA_MIN || a->kind == SAA_MAX;
	}
}

static bool
has_filter(const SiftAggSpec *s)
{
	for (int ai = 0; ai < s->naggs; ai++)
		if (s->agg[ai].filter)
			return true;
	return false;
}

/* a leaf of the list's elements, whose count in a unit is the unit's elements */
static int
elem_leaf(AggExec *ex)
{
	for (int l = 0; l < ex->nbase; l++)
		if (ex->lay->leaf[l].parent == ex->pa.list && ex->lay->leaf[l].kind == SCL_SCALAR)
			return l;
	return -1;
}

bool
rollup_skip(AggExec *ex)
{
	if (ex->pa_scan_run < PA_SCAN_RUN)
		return false;
	if (++ex->pa_scan_run >= PA_SCAN_RUN + PA_SCAN_RESAMPLE)
		ex->pa_scan_run = PA_SCAN_RUN - 1;		/* one more clear scan skips again */
	ex->n_rollup_cost++;
	return true;
}

/* does the unit's rollup cost less than its elements (the unit scans when not)? Unknown sizes: the rollup */
static bool
rollup_cheaper(AggExec *ex)
{
	uint32		nt = sift_colsrc_unit_rollup_tuples(ex->src, &ex->unit, ex->pa.list);
	int			leaf = elem_leaf(ex);
	SiftSrcUnitAgg ua;
	double		tuples,
				elems;

	if (nt == 0 || leaf < 0 || !sift_colsrc_unit_agg(ex->src, &ex->unit, leaf, &ua))
		return true;
	tuples = nt * ex->pa_tuple_cost * sift_agg_rollup_cost;
	elems = ua.nvals * ex->pa_elem_cost;
	ex->pa_scan_run = tuples >= 1.25 * elems && sift_agg_rollup_cost > 0 ? ex->pa_scan_run + 1 : 0;
	return tuples <= elems;
}

/* the ids of key k for the kept tuples of the rollup */
static void
rollup_key_ids(AggExec *ex, const SiftSrcRollup *r, int k, const uint32 *kept, int nkept, uint32 *ids)
{
	int			j = rollup_dim(r, ex->spec->key[k].leaf);

	preagg_key_ids(ex, k, r->key[j], r->isnull[j], UINT32_MAX, r->dict[j], kept, nkept, ids);
}

/* what tuple t of the rollup adds to each aggregate: slot[ai] is the measure (or the dim, -2 - dim) the aggregate reads, fkeep its FILTER */
static void
rollup_vals(AggExec *ex, const SiftSrcRollup *r, uint32 t, const int *slot, uint64 **fkeep, PaVal *v)
{
	for (int ai = 0; ai < ex->spec->naggs; ai++)
	{
		int			m = slot[ai];

		v[ai] = (PaVal) {.rows = r->count[t], .nn = r->count[t]};
		if (fkeep[ai] && !sift_colsrc_bit(fkeep[ai], t))
		{
			v[ai].rows = v[ai].nn = 0;				/* the elements of the tuple fail the FILTER: they add nothing to this aggregate */
			continue;
		}
		if (m <= -2)
			v[ai].nn = r->isnull[-2 - m] && r->isnull[-2 - m][t] ? 0 : r->count[t];
		else if (m >= 0)
		{
			v[ai].nn = r->nn[m][t];
			v[ai].has_mm = r->nn[m][t] > 0;
			if (r->sum[m])
				v[ai].sum = r->sum[m][t];
			if (r->sumf[m])
				v[ai].sumf = r->sumf[m][t];
			if (r->min[m])
			{
				v[ai].min = r->min[m][t];
				v[ai].max = r->max[m][t];
			}
			if (r->minf[m])
			{
				v[ai].minf = r->minf[m][t];
				v[ai].maxf = r->maxf[m][t];
			}
		}
	}
}

/* the key ids of grouping set `set`: the keys outside the set are NULL (id 0, as key_prepare gives them), the last key is the number of the set */
static const uint32 *
set_ids(AggExec *ex, int set, int n, const uint32 *ids)
{
	int			nk = ex->spec->nkeys;
	uint32	   *out = MemoryContextAlloc(ex->ucxt, Max((Size) n * nk, 1) * sizeof(uint32));

	for (int k = 0; k < nk; k++)
		for (int i = 0; i < n; i++)
			out[(Size) k * n + i] = ex->spec->key[k].kind == SAK_SET ? set + 1 : (ex->spec->setmask[set] >> k & 1) ? ids[(Size) k * n + i] : 0;
	return out;
}

/* The unit's rollup of the list: its tuples that pass the conjuncts, folded into the groups of the keys. false: the unit lacks a dim or a measure the query reads */
bool
preagg_rollup(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcRollup r;
	int		   *slot = MemoryContextAlloc(ex->ucxt, Max(s->naggs, 1) * sizeof(int));
	PaVal	   *vs;
	uint32	   *kept,
			   *ids,
			   *gid;
	uint64	   *keep,
			  **fkeep = MemoryContextAllocZero(ex->ucxt, Max(s->naggs, 1) * sizeof(uint64 *));
	int			nkept = 0,
				qlevel = s->group >= 0;

	ex->pa_miss = true;
	if (!rollup_cheaper(ex))
	{
		ex->pa_miss = false;
		ex->n_rollup_cost++;
		return false;
	}
	if (!rollup_net(ex, &r))
		return false;
	for (int d = 0; d < ex->pa.ndims; d++)
		if (rollup_dim(&r, ex->pa.rdim[d]) < 0)
			return false;
	for (int ai = 0; ai < s->naggs; ai++)
	{
		const SiftAggAgg *a = &s->agg[ai];
		int			leaf = pa_leaf(a),
					m = 0;
		bool		mm = a->kind == SAA_MIN || a->kind == SAA_MAX;

		if (leaf < 0)
		{
			slot[ai] = -1;
			continue;
		}
		if (a->kind == SAA_COUNT && rollup_dim(&r, leaf) >= 0)
		{
			slot[ai] = -2 - rollup_dim(&r, leaf);
			continue;
		}
		while (m < r.nmeas && r.meas[m] != leaf)
			m++;
		if (m == r.nmeas)
			return false;
		if (ex->lay->leaf[leaf].vtype == SCV_F64 && a->kind != SAA_COUNT && !preagg_floats(ex, leaf, !mm))
		{
			ex->pa_miss = false;
			return false;
		}
		/* a unit net of its exceptions has no minimum or maximum: they are not the remaining rows' */
		if (mm && (ex->lay->leaf[leaf].vtype == SCV_F64 ? r.minf[m] == NULL : r.min[m] == NULL))
		{
			ex->pa_miss = false;
			return false;
		}
		slot[ai] = m;
	}
	if (s->npred > 0 || has_filter(s))
		rollup_bind(ex, &r);
	keep = NULL;
	/* the conjuncts of a scan of the elements that are the rows' are zone comparisons the unit lies inside (preagg_zones), and so is a range of the rows' rollup on a leaf that is not its dim */
	for (int i = 0; i < s->npred; i++)
	{
		int			l = ex->pa.rows ? sift_aggspec_pred_leaf(s->pred[i], ex->lay, ex->nbase, -1) : -2;

		if (s->pred[i]->level == qlevel && (l < 0 || rollup_dim_leaf(ex, l)))
			keep = pred_pass(ex, s->pred[i], qlevel, r.ntuples, keep);
	}
	for (int ai = 0; ai < s->naggs; ai++)
		fkeep[ai] = s->agg[ai].filter ? pred_pass(ex, s->agg[ai].filter, qlevel, r.ntuples, NULL) : NULL;
	for (int d = 0; d < ex->pa.ndims; d++)
		ex->uvec_ok[ex->pa.rdim[d]] = false;
	kept = MemoryContextAlloc(ex->ucxt, Max(r.ntuples, 1) * sizeof(uint32));
	for (uint32 t = 0; t < r.ntuples; t++)
		if (keep == NULL || sift_colsrc_bit(keep, t))
			kept[nkept++] = t;
	ids = MemoryContextAllocZero(ex->ucxt, Max((Size) nkept * s->nkeys, 1) * sizeof(uint32));
	gid = MemoryContextAllocZero(ex->ucxt, Max(nkept, 1) * sizeof(uint32));
	vs = MemoryContextAllocZero(ex->ucxt, Max((Size) nkept * s->naggs, 1) * sizeof(PaVal));
	for (int k = 0; k < s->nkeys; k++)
		if (s->key[k].kind != SAK_SET)
			rollup_key_ids(ex, &r, k, kept, nkept, ids + (Size) k * nkept);
	for (int i = 0; i < nkept; i++)
		rollup_vals(ex, &r, kept[i], slot, fkeep, vs + (Size) i * s->naggs);
	/* a grouping set folds the tuples again, under the ids of its own columns; a plain query is the one set */
	for (int set = 0; set < Max(s->nsets, 1); set++)
	{
		if (ex->fold && set != ex->fold - 1)	/* sets_fold sums the other sets from this one */
			continue;
		ids_to_gids(ex, nkept, s->nsets ? set_ids(ex, set, nkept, ids) : ids, gid);
		for (int i = 0; i < nkept; i++)
			preagg_fold(ex, gid[i], r.count[kept[i]], vs + (Size) i * s->naggs);
	}
	return true;
}
