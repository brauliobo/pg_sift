/*
 * agg/unit_preagg.c - the pre-aggregates of a unit: the fold of its stored sums and histograms and the rollups (docs/COL-PREAGG.md)
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "portability/instr_time.h"
#include "parser/parse_agg.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/expandeddatum.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/wide.h"
#include "agg/back.h"
#include "agg/kern.h"
#include "agg/part.h"
#include "agg/spec.h"
#include "col/kern.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/topn.h"
#include "core/visibility.h"
#include "core/popcount.h"
#include "agg/agg_int.h"

/* ---------- pre-aggregates (docs/COL-PREAGG.md) ---------- */

/* the leaf an aggregate reads: its column, the only leaf of its numeric program; -1: count(*) */
static int
pa_leaf(const SiftAggAgg *a)
{
	sift_i128	mul,
				add;

	return a->kind == SAA_COUNT_STAR ? -1 : a->num.n ? sift_aggnum_linear(&a->num, &mul, &add) : a->leaf;
}

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
				acc->w[g] += mul * v->sum + add * v->nn;
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

/* the unit's zone of every leaf the conjuncts compare lies inside the comparison, and holds no NULL: they hold for all the rows */
static bool
preagg_zones(AggExec *ex)
{
	for (int k = 0; k < ex->nprune; k++)
	{
		const SiftSrcPrune *z = &ex->prune[k];
		SiftSrcUnitAgg a;

		if (z->leaf != ex->pa.dim &&
			(!sift_colsrc_unit_agg(ex->src, &ex->unit, z->leaf, &a) || a.nnull > 0 || !a.has_minmax || a.min < z->lo || a.max > z->hi))
			return false;
	}
	return true;
}

/* no key: the unit's rows are one group, from the sums of the leaves */
static bool
preagg_sums(AggExec *ex)
{
	PaVal	   *v = MemoryContextAllocZero(ex->ucxt, Max(ex->spec->naggs, 1) * sizeof(PaVal));
	int			nrow = ex->unit.nrow;

	for (int ai = 0; ai < ex->spec->naggs; ai++)
	{
		const SiftAggAgg *a = &ex->spec->agg[ai];
		int			leaf = pa_leaf(a);
		SiftSrcUnitAgg ua;

		v[ai].rows = v[ai].nn = nrow;
		if (leaf < 0)
			continue;
		if (!sift_colsrc_unit_agg(ex->src, &ex->unit, leaf, &ua) || (a->kind != SAA_COUNT && a->kind != SAA_MIN && a->kind != SAA_MAX && !ua.has_sums))
			return false;
		v[ai] = (PaVal) {nrow, ua.nvals - ua.nnull, ua.sum, ua.has_minmax, ua.min, ua.max};
	}
	preagg_fold(ex, 0, nrow, v);
	return true;
}

/* the entries (groups of a histogram, tuples of a rollup) that pass the test p of qlevel, ANDed into keep (NULL: all pass so far): a bitmap, the vectors bound for the leaves p reads */
static uint64 *
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
static uint64 *
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

		if (p == NULL || sift_aggspec_pred_leaf(p, ex->lay, ex->nbase, s->group) != ex->pa.dim)
			continue;
		if (!bound)
		{
			uint8	   *isnull = h->has_null ? MemoryContextAllocZero(ex->ucxt, ng + SIFT_AGG_PAD) : NULL;
			void	   *data = (void *) TYPEALIGN(SIFT_SIMD_ALIGN, MemoryContextAllocZero(ex->ucxt, ng * sizeof(int64) + SIFT_AGG_PAD + SIFT_SIMD_ALIGN));

			for (int k = 0; k < ng - h->has_null; k++)
			{
				if (wide)
					((int64 *) data)[k] = h->key[k];
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

/*
 * The ids of key ki for the kept groups or tuples: the unit's dictionary entries, or the integers. A group is NULL when isnull[g] (NULL: the
 * groups from nonnull on are, the NULL group of a histogram being its last)
 */
void
preagg_key_ids(AggExec *ex, int ki, const int64 *key, const uint8 *isnull, uint32 nonnull, const SiftSrcDict *dict, const uint32 *kept, int nkept,
			   uint32 *ids)
{
	KeyState   *k = &ex->ks[ki];
	int32		mn = PG_INT32_MAX,
				mx = PG_INT32_MIN;
	const uint32 *map = NULL;
	bool		cover = false;

#define PA_NULL(g) (isnull ? isnull[g] : (g) >= nonnull)
	if (k->numk)
	{
		/* the program of one leaf: the ids of its values, as the scan gives them */
		for (int j = 0; j < nkept; j++)
			ids[j] = PA_NULL(kept[j]) ? 0 : idmap_id(&k->im, key[kept[j]]);
		return;
	}
	if (dict)
		map = dict_kmap(ex, dict_slot(ex, dict), dict, ki);
	for (int j = 0; j < nkept && !dict; j++)
		if (!PA_NULL(kept[j]))
		{
			mn = Min(mn, (int32) key[kept[j]]);
			mx = Max(mx, (int32) key[kept[j]]);
		}
	if (!dict)
		cover = key_cover(ex, ki, mn <= mx, mn, mx, nkept);
	for (int j = 0; j < nkept; j++)
	{
		uint32		g = kept[j];

		if (dict)
			ids[j] = map[PA_NULL(g) ? dict->n : (uint32) key[g]];
		else if (PA_NULL(g))
			ids[j] = key_null_id(ex, ki);
		else
			ids[j] = cover ? ((const uint32 *) k->rt.tab)[(int32) key[g] - k->rt.lo] : ident_wide_id(ex, k, (int32) key[g]);
	}
#undef PA_NULL
}

/* the groups of the table for n tuples that have the key ids ids[key * n + i]: compute_gids for ids that are known */
void
ids_to_gids(AggExec *ex, int n, const uint32 *ids, uint32 *gid)
{
	int			nk = ex->spec->nkeys;

	if (nk == 0)
	{
		memset(gid, 0, n * sizeof(uint32));
		return;
	}
	for (int j = 0; j < nk && !ex->hash; j++)
		if (key_used(&ex->ks[j]) > ex->ks[j].cap)
		{
			direct_grow(ex);
			break;
		}
	if (!ex->hash)
	{
		for (int i = 0; i < n; i++)
		{
			uint32		g = 0;

			for (int j = 0; j < nk; j++)
				g += ids[(Size) j * n + i] * ex->ks[j].mult;
			gid[i] = g;
		}
		return;
	}
	for (int i = 0; i < n; i++)
	{
		uint32		k[SIFT_AGG_MAX_KEYS] = {0};

		for (int j = 0; j < nk; j++)
			k[j] = ids[(Size) j * n + i];
		gid[i] = hash_group(ex, k);
	}
	if (ex->ngroups > ex->gcap)
		groups_resize(ex, Max(ex->gcap * 2, ex->ngroups), NULL, ex->gcap);
}

/* the values of a float8 leaf of the unit are finite and so small that the sum of their squares, which avg(float8) is owed to check, cannot overflow */
#define PA_FLOAT_MAX	1e100

static bool
preagg_floats(AggExec *ex, int leaf, bool bounded)
{
	SiftSrcUnitAgg a;

	return sift_colsrc_unit_agg(ex->src, &ex->unit, leaf, &a) && !a.has_nan &&
		(!bounded || a.nnull == a.nvals || (a.has_minmax && fabs(a.min_f) <= PA_FLOAT_MAX && fabs(a.max_f) <= PA_FLOAT_MAX));
}

/* the groups of the unit's histogram of the key's column, or of the column the conjuncts test (no key: one group) */
static bool
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

	if (!sift_colsrc_unit_hist(ex->src, &ex->unit, ex->pa.dim, &h))
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
				if (kept[j] < h.ngroups - h.has_null)
				{
					uint32		id = h.dict ? dict_amap(ex, dict_slot(ex, h.dict), h.dict, ai)[(uint32) h.key[kept[j]]] : (uint32) h.key[kept[j]];

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
static void
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

/*
 * Units in a row that must cost at least a quarter more from the rollup than from the elements before the next ones are scanned without even looking
 * at them (their exactness, their rollup), and how many are skipped before one is looked at again (a clear scan then skips the next ones at once): the units of an index look alike, and
 * looking costs buffer reads that a scan the cost chose anyway does not need.
 */
#define PA_SCAN_RUN		3
#define PA_SCAN_RESAMPLE	16

static bool
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
static bool
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
	int			nkept = 0;

	ex->pa_miss = true;
	if (!rollup_cheaper(ex))
	{
		ex->pa_miss = false;
		ex->n_rollup_cost++;
		return false;
	}
	if (!sift_colsrc_unit_rollup(ex->src, &ex->unit, ex->pa.list, &ex->pa_need, &r))
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
		slot[ai] = m;
	}
	if (s->npred > 0 || has_filter(s))
		rollup_bind(ex, &r);
	keep = NULL;
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->level == 1)
			keep = pred_pass(ex, s->pred[i], 1, r.ntuples, keep);
	for (int ai = 0; ai < s->naggs; ai++)
		fkeep[ai] = s->agg[ai].filter ? pred_pass(ex, s->agg[ai].filter, 1, r.ntuples, NULL) : NULL;
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
		ids_to_gids(ex, nkept, s->nsets ? set_ids(ex, set, nkept, ids) : ids, gid);
		for (int i = 0; i < nkept; i++)
			preagg_fold(ex, gid[i], r.count[kept[i]], vs + (Size) i * s->naggs);
	}
	return true;
}

/* a unit that holds a value a leaf the source reads again cannot store has the row SQL raises for, also when the query reads other columns: it is scanned */
static bool
preagg_rechecked(AggExec *ex)
{
	const SiftAggSpec *s = ex->spec;

	for (int i = 0; ex->recheck && i < s->nleaves; i++)
		if (s->recheck[i] != SRK_NONE && sift_colsrc_overflow(ex->src, &ex->unit, s->leaves[i]))
			return true;
	for (int j = 0; ex->recheck && j < s->nvleaf; j++)
		if (s->vleaf[j].recheck != SRK_NONE && sift_colsrc_overflow(ex->src, &ex->unit, ex->vl[j].val))
			return true;
	return false;
}

/*
 * The aggregates that add `leaf * mul + add` of an int8 leaf (PostgreSQL raises for a result outside int8) can take the unit's sums only when its zone
 * puts every result inside int8: a unit that may hold a value that raises is scanned, and the scan raises.
 */
static bool
preagg_arith(AggExec *ex)
{
	for (int ai = 0; ai < ex->spec->naggs; ai++)
	{
		const SiftAggAgg *a = &ex->spec->agg[ai];
		sift_i128	mul,
					add,
					lo,
					hi;
		SiftSrcUnitAgg ua;

		if (a->num.n < 2)
			continue;
		if (!sift_colsrc_unit_agg(ex->src, &ex->unit, sift_aggnum_linear(&a->num, &mul, &add), &ua))
			return false;
		if (ua.nnull == ua.nvals)
			continue;
		if (!ua.has_minmax)
			return false;
		lo = Min(mul * ua.min, mul * ua.max) + add;
		hi = Max(mul * ua.min, mul * ua.max) + add;
		if (lo < PG_INT64_MIN || hi > PG_INT64_MAX)
			return false;
	}
	return true;
}

bool
preagg_unit(AggExec *ex)
{
	if (ex->pa.list >= 0 && rollup_skip(ex))
		return false;
	if (!sift_colsrc_unit_exact(ex->src, &ex->unit) || preagg_rechecked(ex) || !preagg_arith(ex))
		return false;
	if (ex->pa.hdim >= 0 && !preagg_zones(ex))
	{
		bool		ok;

		/* the zone is not inside a comparison on the dim: its groups that pass it, the other comparisons still hold for the zone */
		ex->pa.dim = ex->pa.hdim;
		ok = preagg_zones(ex) && preagg_hist(ex);
		ex->pa.dim = -1;
		return ok;
	}
	if (!preagg_zones(ex))
		return false;
	if (ex->pa.list < 0)
		return ex->pa.dim < 0 ? preagg_sums(ex) : preagg_hist(ex);
	if (preagg_rollup(ex))
		return true;
	/* the unit is clean but its rollup lacks something: scan it now, and ask for the rollup it should have */
	if (ex->pa_miss)
	{
		ex->n_rollup_miss++;
		ex->n_rollup_queued += sift_colsrc_unit_rollup_miss(ex->src, &ex->unit);
	}
	return false;
}

void
preagg_init(AggExec *ex)
{
	SiftSrcPreagg pa;

	ex->pa_on = false;
	ex->pa_why = SPW_OFF;
	if (!sift_agg_preagg)
		return;
	sift_colsrc_preagg(ex->index ? ex->index : ex->heap, &pa);
	ex->pa_why = !pa.aggs && pa.ndims == 0 && pa.nrollups == 0 ? SPW_NONE : sift_aggspec_preagg_why(ex->spec, ex->lay, ex->nbase, &pa, &ex->pa);
	ex->pa_on = ex->pa_why == SPW_OK;
	if (ex->pa_on && ex->pa.list >= 0)
	{
		rollup_need(ex);
		sift_aggspec_unit_costs(ex->spec, ex->lay, &ex->pa_elem_cost, &ex->pa_tuple_cost);
		ex->pa_scan_run = 0;
	}
	/* a comparison with NULL keeps no row, and leaves no interval for the zone to lie inside; nor does an IN list with gaps (or a list that prune_init gave no zone) */
	foreach_ptr(SiftAggPred, p, sift_aggspec_zone_cmps(ex->spec, ex->lay, ex->nbase))
	{
		PredState  *st = &ex->ps[p->id];

		if (!st->ready)
			eval_operand(ex, p, st);
		if (ex->pa_on && (st->cnull || (p->kind == SAP_IN && !st->in_dense)))
		{
			/* the groups of the dim's histogram test an IN list with gaps, which gives the zones no interval: the sums cannot answer */
			if (!st->cnull && p->leaf == ex->pa.hdim)
			{
				ex->pa.dim = ex->pa.hdim;
				ex->pa.hdim = -1;
			}
			else
			{
				ex->pa_on = false;
				ex->pa_why = SPW_ZONE;
			}
		}
	}
}
