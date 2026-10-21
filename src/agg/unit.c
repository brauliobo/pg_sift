/*
 * agg/unit.c - answering one unit: the fold of its pre-aggregates, late conjuncts, overflow checks, filters, process_unit
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

static void
overflow_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sift: a column of the aggregate scan holds values its index cannot store"),
			 errhint("Set sift.enable_agg to off for this query.")));
}

/* bits [lo, hi) of a bit vector cleared */
static void
clear_bit_range(uint64 *bits, uint32 lo, uint32 hi)
{
	for (; lo < hi && (lo & 63); lo++)
		bits[lo >> 6] &= ~(UINT64CONST(1) << (lo & 63));
	for (; lo + 64 <= hi; lo += 64)
		bits[lo >> 6] = 0;
	for (; lo < hi; lo++)
		bits[lo >> 6] &= ~(UINT64CONST(1) << (lo & 63));
}

/* ---------- visibility ---------- */

/* ---------- a unit ---------- */

/* the element count of the unit and the offsets of its rows; the owner of every element is made when something reads it (unit_parent) */
static void
build_parents(AggExec *ex)
{
	ex->offs = elem_offs(ex, ex->spec->group);
	ex->nelem = ex->offs[ex->unit.nrow];
	ex->parent = NULL;
}

/* the row that owns each element of the array or object leaf `group` (the unnested one is unit_parent's) */
static const uint32 *
group_parent(AggExec *ex, int group)
{
	const uint32 *offs;
	uint32	   *par;

	if (group == ex->spec->group)
	{
		build_parents(ex);
		return unit_parent(ex);
	}
	offs = elem_offs(ex, group);
	par = MemoryContextAlloc(ex->ucxt, (Size) offs[ex->unit.nrow] * sizeof(uint32) + SIFT_AGG_PAD);
	sift_colk->expand_parents(offs + 1, ex->unit.nrow, par);
	return par;
}

/*
 * The conjuncts that evaluate an expression PostgreSQL may fail on (SiftAggPred.late) are applied to the items that passed every other
 * conjunct and the snapshot check, in the order of the query, each to the survivors of the ones before it: the normal plan never
 * evaluates an expression for a row that an earlier condition (a guard against a zero divisor) or its snapshot leaves out.
 */
static void
apply_late(AggExec *ex, uint64 *mask, int total)
{
	for (int i = 0; i < ex->spec->npred; i++)
	{
		SiftAggPred *p = ex->spec->pred[i];

		if (!p->late)
			continue;
		for (int st = 0; st < total; st += VEC)
		{
			int			n = Min(VEC, total - st);
			uint64		t[VECW + PADW];

			ex->alive = mask + st / 64;
			eval_pred(ex, p, ex->qlevel, st, n, t);
			ex->alive = NULL;
			sift_colk->bits_and(mask + st / 64, t, (n + 63) / 64);
		}
	}
}

/* ---------- pre-aggregates (docs/COL-PREAGG.md) ---------- */

/* the leaf an aggregate reads: its column, the only leaf of its numeric program; -1: count(*) */
static int
pa_leaf(const SiftAggAgg *a)
{
	return a->kind == SAA_COUNT_STAR ? -1 : a->num.n ? a->num.step[0].leaf : a->leaf;
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
			acc->w[g] += v->sum;
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

/*
 * The groups of the histogram that pass the conjuncts on the key's column (a bitmap; NULL: all): the tests run on the groups as
 * on the rows of a unit that has one per group, the column's vector being the histogram's keys.
 */
static uint64 *
preagg_keep(AggExec *ex, const SiftSrcHist *h)
{
	SiftAggSpec *s = ex->spec;
	int			ng = h->ngroups,
				nw = (ng + 63) / 64;
	uint64	   *keep = NULL;
	uint32	   *data = NULL;

	for (int i = 0; i < s->npred; i++)
	{
		if (sift_aggspec_pred_leaf(s->pred[i], ex->lay, ex->nbase, s->group) != ex->pa.dim)
			continue;
		if (keep == NULL)
		{
			uint8	   *isnull = h->has_null ? MemoryContextAllocZero(ex->ucxt, ng + SIFT_AGG_PAD) : NULL;

			keep = MemoryContextAlloc(ex->ucxt, (nw + PADW) * sizeof(uint64));
			fill_ones(keep, ng);
			data = (uint32 *) TYPEALIGN(SIFT_SIMD_ALIGN, MemoryContextAllocZero(ex->ucxt, ng * sizeof(uint32) + SIFT_AGG_PAD + SIFT_SIMD_ALIGN));
			for (int k = 0; k < ng - h->has_null; k++)
				data[k] = (uint32) h->key[k];
			if (h->has_null)
			{
				isnull[ng - 1] = 1;
				data[ng - 1] = h->dict ? h->dict->n : 0;
			}
			ex->uvec[ex->pa.dim] = (SiftSrcVec) {data, isnull, h->dict, ng, NULL};
			ex->uvec_ok[ex->pa.dim] = true;
		}
		for (int st = 0; st < ng; st += VEC)
		{
			int			n = Min(VEC, ng - st);
			uint64		t[VECW + PADW];

			eval_pred(ex, s->pred[i], s->group >= 0, st, n, t);
			sift_colk->bits_and(keep + st / 64, t, (n + 63) / 64);
		}
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
	uint64	   *keep;
	int			nkept = 0;

	if (!sift_colsrc_unit_hist(ex->src, &ex->unit, ex->pa.dim, &h))
		return false;
	for (int ai = 0; ai < s->naggs; ai++)
	{
		int			leaf = pa_leaf(&s->agg[ai]);
		int			m = 0;

		while (leaf >= 0 && leaf != ex->pa.dim && m < h.nmeas && h.meas[m] != leaf)
			m++;
		if (leaf >= 0 && leaf != ex->pa.dim && (m == h.nmeas || (s->agg[ai].kind != SAA_COUNT && h.sum[m] == NULL && h.sumf[m] == NULL)))
			return false;
		if (leaf >= 0 && (s->agg[ai].kind == SAA_SUM_F64 || s->agg[ai].kind == SAA_AVG_F64) && !preagg_floats(ex, leaf, true))
			return false;
		slot[ai] = leaf < 0 ? -1 : leaf == ex->pa.dim ? -2 : m;
	}
	keep = preagg_keep(ex, &h);
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
			if (m == -2)
				v[ai].nn = kept[j] < h.ngroups - h.has_null ? h.count[kept[j]] : 0;
			else if (m >= 0)
			{
				v[ai].nn = h.nn[m][kept[j]];
				v[ai].sum = h.sum[m] ? h.sum[m][kept[j]] : 0;
				v[ai].sumf = h.sumf[m] ? h.sumf[m][kept[j]] : 0;
			}
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

/* the tuples that pass the conjunct p (an element test): a bitmap, the vectors of rollup_bind read */
static uint64 *
rollup_pass(AggExec *ex, const SiftAggPred *p, int nt, uint64 *keep)
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

		eval_pred(ex, (SiftAggPred *) p, 1, st, n, t);
		sift_colk->bits_and(keep + st / 64, t, (n + 63) / 64);
	}
	return keep;
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

/* does the unit's rollup cost less than its elements (the unit scans when not)? Unknown sizes: the rollup */
static bool
rollup_cheaper(AggExec *ex)
{
	uint32		nt = sift_colsrc_unit_rollup_tuples(ex->src, &ex->unit, ex->pa.list);
	int			leaf = elem_leaf(ex);
	SiftSrcUnitAgg ua;

	if (nt == 0 || leaf < 0 || !sift_colsrc_unit_agg(ex->src, &ex->unit, leaf, &ua))
		return true;
	return nt * ex->pa_tuple_cost * sift_agg_rollup_cost <= ua.nvals * ex->pa_elem_cost;
}

/* The unit's rollup of the list: its tuples that pass the conjuncts, folded into the groups of the keys. false: the unit lacks a dim or a measure the query reads */
static bool
preagg_rollup(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcRollup r;
	int		   *slot = MemoryContextAlloc(ex->ucxt, Max(s->naggs, 1) * sizeof(int));
	PaVal	   *v = MemoryContextAllocZero(ex->ucxt, Max(s->naggs, 1) * sizeof(PaVal));
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
			keep = rollup_pass(ex, s->pred[i], r.ntuples, keep);
	for (int ai = 0; ai < s->naggs; ai++)
		fkeep[ai] = s->agg[ai].filter ? rollup_pass(ex, s->agg[ai].filter, r.ntuples, NULL) : NULL;
	for (int d = 0; d < ex->pa.ndims; d++)
		ex->uvec_ok[ex->pa.rdim[d]] = false;
	kept = MemoryContextAlloc(ex->ucxt, Max(r.ntuples, 1) * sizeof(uint32));
	for (uint32 t = 0; t < r.ntuples; t++)
		if (keep == NULL || sift_colsrc_bit(keep, t))
			kept[nkept++] = t;
	ids = MemoryContextAllocZero(ex->ucxt, Max((Size) nkept * s->nkeys, 1) * sizeof(uint32));
	gid = MemoryContextAllocZero(ex->ucxt, Max(nkept, 1) * sizeof(uint32));
	for (int k = 0; k < s->nkeys; k++)
	{
		int			j = rollup_dim(&r, s->key[k].leaf);

		preagg_key_ids(ex, k, r.key[j], r.isnull[j], UINT32_MAX, r.dict[j], kept, nkept, ids + (Size) k * nkept);
	}
	ids_to_gids(ex, nkept, ids, gid);
	for (int i = 0; i < nkept; i++)
	{
		uint32		t = kept[i];

		for (int ai = 0; ai < s->naggs; ai++)
		{
			int			m = slot[ai];

			v[ai] = (PaVal) {.rows = r.count[t], .nn = r.count[t]};
			if (fkeep[ai] && !sift_colsrc_bit(fkeep[ai], t))
			{
				v[ai].rows = v[ai].nn = 0;				/* the elements of the tuple fail the FILTER: they add nothing to this aggregate */
				continue;
			}
			if (m <= -2)
				v[ai].nn = r.isnull[-2 - m] && r.isnull[-2 - m][t] ? 0 : r.count[t];
			else if (m >= 0)
			{
				v[ai].nn = r.nn[m][t];
				v[ai].has_mm = r.nn[m][t] > 0;
				if (r.sum[m])
					v[ai].sum = r.sum[m][t];
				if (r.sumf[m])
					v[ai].sumf = r.sumf[m][t];
				if (r.min[m])
				{
					v[ai].min = r.min[m][t];
					v[ai].max = r.max[m][t];
				}
				if (r.minf[m])
				{
					v[ai].minf = r.minf[m][t];
					v[ai].maxf = r.maxf[m][t];
				}
			}
		}
		preagg_fold(ex, gid[i], r.count[t], v);
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
	return false;
}

static bool
preagg_unit(AggExec *ex)
{
	if (!sift_colsrc_unit_exact(ex->src, &ex->unit) || preagg_rechecked(ex) || !preagg_zones(ex))
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
	}
	/* a comparison with NULL keeps no row, and leaves no interval for the zone to lie inside; nor does an IN list with gaps (or a list that prune_init gave no zone) */
	foreach_ptr(SiftAggPred, p, sift_aggspec_zone_cmps(ex->spec, ex->lay, ex->nbase))
	{
		PredState  *st = &ex->ps[p->id];

		if (!st->ready)
			eval_operand(ex, p, st);
		if (ex->pa_on && (st->cnull || (p->kind == SAP_IN && !st->in_dense)))
		{
			ex->pa_on = false;
			ex->pa_why = SPW_ZONE;
		}
	}
}
/* A unit whose zone of the first sort leaf has nothing that sorts before the n-th best so far is not read */
static bool
topn_viable(AggExec *ex)
{
	SiftSrcZone z;

	if (!sift_topn_wants_zones(ex->topn))
		return true;
	sift_colsrc_zone(ex->src, &ex->unit, sift_topn_leaf(ex->topn, 0), &z);
	if (sift_topn_zone_viable(ex->topn, &z))
		return true;
	sift_topn_skip(ex->topn);
	return false;
}

/* the rows that pass go to the heap of the best n, by the sort leaves (agg/topn.h) */
static void
topn_unit(AggExec *ex, const uint64 *rmask, int nrow)
{
	sift_topn_unit(ex->topn, ex->src, &ex->unit, get_vec(ex, sift_topn_leaf(ex->topn, 0)), rmask, nrow);
}

/* *rows gets the rows of the unit that hold a value the index cannot store in leaf (a numeric NaN aside: the readers evaluate it). An element of a child leaf counts for its row. */
static void
leaf_overflow_rows(AggExec *ex, int leaf, uint64 **rows)
{
	const SiftSrcLeaf *l = &ex->lay->leaf[leaf];
	const SiftSrcVec *v;
	const uint32 *par;

	if (l->typid == NUMERICOID || !sift_colsrc_overflow(ex->src, &ex->unit, leaf) || !(v = get_vec(ex, leaf))->ovf)
		return;
	if (!*rows)
		*rows = MemoryContextAllocZero(ex->ucxt, ((ex->unit.nrow + 63) / 64 + PADW) * sizeof(uint64));
	par = l->parent >= 0 ? group_parent(ex, l->parent) : NULL;
	for (int64 e = 0; e < v->n; e++)
		if (sift_colsrc_bit(v->ovf, e))
		{
			uint32		r = par ? par[e] : e;

			(*rows)[r >> 6] |= UINT64CONST(1) << (r & 63);
		}
}

/*
 * The rows of the unit that hold a value the index cannot store in a leaf the scan reads, NULL when there is none. The leaves after the source's
 * read their values from one of its leaves (the value column of a member of an object, the array of a length): theirs count too.
 */
static uint64 *
overflow_rows(AggExec *ex, bool rechecked)
{
	SiftAggSpec *s = ex->spec;
	uint64	   *rows = NULL;

	for (int i = 0; i < s->nleaves; i++)
		if (s->leaves[i] < ex->nbase && (s->recheck[i] != SRK_NONE) == rechecked)
			leaf_overflow_rows(ex, s->leaves[i], &rows);
	for (int j = 0; j < s->nvleaf && !rechecked; j++)
		if (!s->vleaf[j].types)
			leaf_overflow_rows(ex, s->vleaf[j].count ? s->vleaf[j].obj : ex->vl[j].val, &rows);
	return rows;
}

/*
 * A unit may hold values the index cannot store (the writer's committed after the plan was made, in flight or rolled back, as the plan could
 * not know); the rows of another snapshot or none are not the scan's, a row it sees is: the normal plan reads it from the heap.
 * rechecked: the leaves the source reads again (a row of the stored unit that has such a value is not in its live rows: the unit that follows
 * has it, its values as SQL gives them) and what is left is the value SQL raises for. The source has the error of the row; it is raised when the
 * row reaches the expression, after the conjuncts of the rows, as SQL raises it.
 */
static pg_noinline void
check_overflow(AggExec *ex, const uint64 *live, bool rechecked)
{
	int			nrw = (ex->unit.nrow + 63) / 64;
	uint64	   *rows = overflow_rows(ex, rechecked);

	if (!rows)
		return;
	sift_colk->bits_and(rows, live, nrw);
	sift_colsrc_vet(ex->src, &ex->unit, rows);
	if (sift_colk->bits_count(rows, nrw) == 0)
		return;
	if (rechecked)
		sift_colsrc_overflow_raise(ex->src, &ex->unit, rows);
	overflow_error();
}

/* check_overflow for the rows that keep an element of the unit, at the point the aggregates read them (SRK_CAST) */
static pg_noinline void
check_overflow_elems(AggExec *ex, const uint64 *em)
{
	int			nrw = (ex->unit.nrow + 63) / 64;
	uint64	   *rv = MemoryContextAllocZero(ex->ucxt, (nrw + PADW) * sizeof(uint64));

	rows_of_elems(ex->offs, ex->unit.nrow, em, rv);
	check_overflow(ex, rv, true);
}

/* the leaves are read for the rows that still count when they are fewer than a SPARSE_RATIOth of the unit */
#define SPARSE_RATIO	8

/* the rows whose value has one of the JSON types of each SAP_TYPE conjunct; false when none is left */
static bool
type_filters(AggExec *ex, uint64 *rmask)
{
	for (int i = 0; i < ex->spec->npred; i++)
	{
		SiftAggPred *p = ex->spec->pred[i];

		if (p->kind == SAP_TYPE && !sift_colsrc_filter_types(ex->src, &ex->unit, p->leaf, p->op, rmask))
			return false;
	}
	return true;
}

/* the rows that pass each SAP_WIDE conjunct, in the order wide_init gave them to the source; false when none is left */
static bool
wide_filters(AggExec *ex, uint64 *rmask)
{
	int			k = 0;

	for (int i = 0; i < ex->spec->npred; i++)
		if (ex->spec->pred[i]->kind == SAP_WIDE && !sift_colsrc_filter_wide(ex->src, &ex->unit, k++, rmask))
			return false;
	return true;
}

void
process_unit(AggExec *ex)
{
	SiftSrcUnit *u = &ex->unit;
	SiftAggSpec *s = ex->spec;
	int			nrow = u->nrow,
				nrw = (nrow + 63) / 64;
	uint64	   *rmask;
	bool		elem = s->group >= 0;

	MemoryContextReset(ex->ucxt);
	memset(ex->uvec_ok, 0, ex->lay->nleaves * sizeof(bool));
	memset(ex->gofs, 0, ex->lay->nleaves * sizeof(uint32 *));
	memset(ex->gpar, 0, ex->lay->nleaves * sizeof(uint32 *));
	for (int i = 0; i < s->npreds_total; i++)
		ex->ps[i].rowmask_ok = false;
	ex->n_units++;
	if (nrow > 0 && ex->pa_on && preagg_unit(ex))
	{
		ex->n_pre_units++;
		ex->row_base += nrow;
		CHECK_FOR_INTERRUPTS();
		return;
	}
	ex->n_rows += nrow;
	if (nrow == 0)
		return;
	if (sift_aggbe_run_unit(ex->be, ex->be_state, &ex->unit))
	{
		ex->row_base += nrow;
		CHECK_FOR_INTERRUPTS();
		return;
	}
	if (ex->topn && !topn_viable(ex))
		return;
	rmask = MemoryContextAlloc(ex->ucxt, (nrw + PADW) * sizeof(uint64));
	ex->rsel = NULL;
	sift_colsrc_live(ex->src, u, rmask);
	check_overflow(ex, rmask, false);
	if ((ex->ntype > 0 && !type_filters(ex, rmask)) || (ex->nwide > 0 && !wide_filters(ex, rmask)) ||
		(ex->nfilter > 0 && !sift_colsrc_filter(ex->src, u, rmask)))
	{
		ex->row_base += nrow;
		return;
	}
	/* a few rows left: the leaves are decoded for them only */
	if ((ex->nfilter > 0 || ex->ntype > 0 || ex->nwide > 0) && sift_colsrc_can_read_sel(ex->src) && sift_colk->bits_count(rmask, nrw) * SPARSE_RATIO < nrow)
		ex->rsel = rmask;
	/* row level conjuncts */
	for (int i = 0; i < s->npred; i++)
	{
		SiftSrcStrTest *str = ex->ps[s->pred[i]->id].str;

		if (s->pred[i]->level == 0 && !s->pred[i]->late && !(ex->pushed && ex->pushed[i]) && !(str && sift_colsrc_filter_str(ex->src, u, str, rmask)))
			sift_colk->bits_and(rmask, row_bitmap(ex, s->pred[i]), nrw);
	}
	ex->qlevel = elem;
	if (ex->recheck && !ex->recheck_late)
		check_overflow(ex, rmask, true);
	if (!elem)
	{
		sift_colsrc_vet(ex->src, u, rmask);
		apply_late(ex, rmask, nrow);
		if (ex->recheck_late)
			check_overflow(ex, rmask, true);
		if (ex->topn)
			topn_unit(ex, rmask, nrow);
		else
			for (int st = 0; st < nrow; st += VEC)
				agg_chunk(ex, st, Min(VEC, nrow - st), rmask + st / 64);
	}
	else
	{
		uint64	   *em;
		int			m;
		uint64		cm[VECW + PADW];
		bool		allrows;

		build_parents(ex);
		m = ex->nelem;
		/* no row is out: so is no element, and the owners of the elements are not needed to tell */
		allrows = sift_colk->bits_count(rmask, nrw) == nrow;
		em = MemoryContextAlloc(ex->ucxt, ((m + 63) / 64 + PADW) * sizeof(uint64));
		for (int st = 0; st < m; st += VEC)
		{
			int			n = Min(VEC, m - st),
						nw = (n + 63) / 64;

			if (allrows)
				fill_ones(cm, n);
			else
				sift_aggk->gather_bits(cm, rmask, unit_parent(ex) + st, n);
			for (int i = 0; i < s->npred; i++)
				if (s->pred[i]->level == 1 && !s->pred[i]->late)
				{
					uint64		t[VECW + PADW];

					eval_pred(ex, s->pred[i], 1, st, n, t);
					sift_colk->bits_and(cm, t, nw);
				}
			memcpy(em + st / 64, cm, nw * sizeof(uint64));
		}
		/* the rows that keep an element, checked against the snapshot: nothing to check in a unit whose pages are all visible */
		if (sift_colsrc_vet_needed(ex->src, u))
		{
			uint64	   *rv = MemoryContextAllocZero(ex->ucxt, (nrw + PADW) * sizeof(uint64)),
					   *seen = MemoryContextAlloc(ex->ucxt, (nrw + PADW) * sizeof(uint64));

			rows_of_elems(ex->offs, nrow, em, rv);
			memcpy(seen, rv, nrw * sizeof(uint64));
			sift_colsrc_vet(ex->src, u, rv);
			/* the elements of a row the snapshot does not see go */
			for (int w = 0; w < nrw; w++)
				for (uint64 b = seen[w] & ~rv[w]; b; b &= b - 1)
				{
					int			r = w * 64 + pg_rightmost_one_pos64(b);

					clear_bit_range(em, ex->offs[r], ex->offs[r + 1]);
				}
		}
		apply_late(ex, em, m);
		if (ex->recheck_late)
			check_overflow_elems(ex, em);
		for (int st = 0; st < m; st += VEC)
			agg_chunk(ex, st, Min(VEC, m - st), em + st / 64);
	}
	ex->row_base += nrow;
	CHECK_FOR_INTERRUPTS();
}
