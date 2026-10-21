/*
 * agg/spec_preagg.c - the pre-aggregate shape of a spec: which unit pre-aggregates answer it
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/date.h"
#include "nodes/value.h"
#include "storage/itemptr.h"

#include "agg/spec.h"

/* ---------- pre-aggregates ---------- */

/* a leaf that a unit's pre-aggregates describe: a scalar, not lossy, of the rows (group -1) or of the elements of the array leaf group */
static bool
plain_leaf(const SiftSrcLayout *lay, int nbase, int group, int leaf)
{
	const SiftSrcLeaf *lf;

	if (leaf < 0 || leaf >= nbase)
		return false;
	lf = &lay->leaf[leaf];
	return lf->kind == SCL_SCALAR && lf->parent == group && !lf->lossy && !lf->wide;
}

int
sift_aggspec_pred_leaf(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase, int group)
{
	int			leaf = -1;

	if (p->late || p->level != (group >= 0))
		return -2;
	switch (p->kind)
	{
		case SAP_TRUE:
			return -1;
		case SAP_AND:
		case SAP_OR:
			for (int i = 0; i < p->nchild; i++)
			{
				int			l = sift_aggspec_pred_leaf(p->child[i], lay, nbase, group);

				if (l == -2 || (l >= 0 && leaf >= 0 && l != leaf))
					return -2;
				leaf = l >= 0 ? l : leaf;
			}
			return leaf;
		case SAP_CMP:
		case SAP_IN:
		case SAP_NULL:
		case SAP_LUT:
			return plain_leaf(lay, nbase, group, p->leaf) ? p->leaf : -2;
		default:
			return -2;
	}
}

/* the leaf of a numeric program that is only that leaf, -1 if it is more */
static int
num_leaf(const SiftAggNum *n)
{
	return n->n == 1 && n->step[0].op == SNX_LEAF ? n->step[0].leaf : -1;
}

/* the leaf of the histogram `dim` in pa, -1 if the index writes none */
static int
pa_dim(const SiftSrcPreagg *pa, int dim)
{
	for (int d = 0; d < pa->ndims; d++)
		if (pa->dim[d].leaf == dim)
			return d;
	return -1;
}

/* The leaf aggregate a reads (-1: count(*)) when pre-aggregates can give it at all; group: the list leaf of an element level scan, else -1 */
static bool
preagg_agg_leaf(const SiftAggAgg *a, const SiftSrcLayout *lay, int nbase, int group, int *leaf_out)
{
	int			leaf = a->leaf;
	bool		sums = false,
				flt = false,
				mm = false;
	SiftSrcVecType vt;

	if (a->filter || a->distinct)
		return false;
	switch (a->kind)
	{
		case SAA_COUNT_STAR:
			*leaf_out = -1;
			return true;
		case SAA_COUNT:
			leaf = a->num.n ? num_leaf(&a->num) : a->leaf;
			break;
		case SAA_SUM_INT:
		case SAA_AVG_INT:
			sums = true;
			break;
		case SAA_SUM_NUM:
		case SAA_AVG_NUM:
			leaf = num_leaf(&a->num);
			sums = true;
			break;
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			/* the sums of the groups are added in another order than the rows': the result of a float8 sum may differ in its last digits */
			if (group < 0 || a->nvx || a->restype != FLOAT8OID)
				return false;
			flt = true;
			break;
		case SAA_MIN:
		case SAA_MAX:
			leaf = a->num.n ? num_leaf(&a->num) : a->leaf;
			mm = true;
			break;
		default:
			return false;
	}
	if (!plain_leaf(lay, nbase, group, leaf))
		return false;
	vt = lay->leaf[leaf].vtype;
	if (flt ? vt != SCV_F64 : mm ? (vt != SCV_I32 && vt != SCV_I64 && !(group >= 0 && !a->num.n && vt == SCV_F64)) : sums && vt != SCV_I32 && vt != SCV_I64)
		return false;
	*leaf_out = leaf;
	return true;
}

/* can the unit's histogram d of pa (or the sums of the leaves when d < 0) give aggregate a? */
static bool
cube_agg_ok(const SiftAggAgg *a, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, int d)
{
	int			leaf;

	if (!preagg_agg_leaf(a, lay, nbase, -1, &leaf))
		return false;
	if (leaf < 0 || d < 0)
		return true;
	if (a->kind == SAA_MIN || a->kind == SAA_MAX)
		return false;
	if (leaf == pa->dim[d].leaf)
		return a->kind == SAA_COUNT;			/* the rows of the groups that are not NULL */
	for (int m = 0; m < pa->dim[d].nmeas; m++)
		if (pa->dim[d].meas[m] == leaf)
			return true;
	return false;
}

/*
 * The leaf the conjuncts test besides the zone comparisons (-1: none), or -2 when they cannot be answered from a histogram. The conjuncts must be
 * zone comparisons, or test one scalar leaf.
 */
static int
preagg_conj_dim(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, int dim)
{
	List	   *cmps = sift_aggspec_zone_cmps(s, lay, nbase);

	for (int i = 0; i < s->npred; i++)
	{
		int			l = sift_aggspec_pred_leaf(s->pred[i], lay, nbase, -1);

		if (l == -2)
			return -2;
		/* a conjunct on a leaf the zones of a unit can decide whole is no reason for a histogram; every other one is a test of the dimension */
		if (l < 0 || list_member_ptr(cmps, s->pred[i]))
			continue;
		if (dim >= 0 && dim != l)
			return -2;
		dim = l;
	}
	return dim;
}

static bool
add_leaf(int *arr, int *n, int max, int leaf)
{
	for (int i = 0; i < *n; i++)
		if (arr[i] == leaf)
			return true;
	if (*n == max)
		return false;
	arr[(*n)++] = leaf;
	return true;
}

static bool
dim_leaf(const SiftSrcLayout *lay, int nbase, int group, int leaf)
{
	return plain_leaf(lay, nbase, group, leaf) && (lay->leaf[leaf].vtype == SCV_I32 || lay->leaf[leaf].vtype == SCV_CODE);
}

/* Adds the leaves a predicate of the elements of group tests to dims; false when it tests more than plain dim leaves (a combo, a late expression) */
static bool
pred_dims(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase, int group, int *dims, int *nd)
{
	if (p->late || p->level != 1)
		return false;
	switch (p->kind)
	{
		case SAP_TRUE:
			return true;
		case SAP_AND:
		case SAP_OR:
			for (int i = 0; i < p->nchild; i++)
				if (!pred_dims(p->child[i], lay, nbase, group, dims, nd))
					return false;
			return true;
		case SAP_CMP:
		case SAP_IN:
		case SAP_NULL:
		case SAP_LUT:
			return dim_leaf(lay, nbase, group, p->leaf) && add_leaf(dims, nd, SIFT_SRC_MAX_RDIMS, p->leaf);
		default:
			return false;
	}
}

static bool
in_leaves(const int *arr, int n, int leaf)
{
	for (int i = 0; i < n; i++)
		if (arr[i] == leaf)
			return true;
	return false;
}

/*
 * The rollup of the list that answers a scan of its elements: the dims are the group keys, the leaves the element conjuncts test and the leaves
 * count(leaf) reads; the measures the other aggregates read. Conjuncts of the rows must be zone comparisons.
 */
static bool
preagg_rollup(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, SiftAggPaShape *out)
{
	List	   *cmps = sift_aggspec_zone_cmps(s, lay, nbase);
	int			group = s->group,
				nd = 0,
				nm = 0,
				r = 0,
				dims[SIFT_SRC_MAX_RDIMS],
				meas[SIFT_SRC_MAX_MEAS];

	while (r < pa->nrollups && pa->rollup[r].list != group)
		r++;
	if (r == pa->nrollups)
		return false;
	for (int k = 0; k < s->nkeys; k++)
		if (s->key[k].kind != SAK_LEAF || !dim_leaf(lay, nbase, group, s->key[k].leaf) || !add_leaf(dims, &nd, SIFT_SRC_MAX_RDIMS, s->key[k].leaf))
			return false;
	for (int i = 0; i < s->npred; i++)
	{
		int			l = sift_aggspec_pred_leaf(s->pred[i], lay, nbase, -1);

		if (l != -2 ? (l >= 0 && !list_member_ptr(cmps, s->pred[i])) : !pred_dims(s->pred[i], lay, nbase, group, dims, &nd))
			return false;
	}
	for (int i = 0; i < s->naggs; i++)
	{
		const SiftAggAgg *a = &s->agg[i];
		int			leaf;

		if (!preagg_agg_leaf(a, lay, nbase, group, &leaf))
			return false;
		if (leaf < 0)
			continue;
		/* count(leaf) is the count of the dim's tuples that are not NULL, or the count of the measure */
		if (a->kind == SAA_COUNT)
		{
			if (!in_leaves(pa->rollup[r].dim, pa->rollup[r].ndims, leaf) && !in_leaves(pa->rollup[r].meas, pa->rollup[r].nmeas, leaf))
				return false;
		}
		else if (!add_leaf(meas, &nm, SIFT_SRC_MAX_MEAS, leaf))
			return false;
	}
	for (int i = 0; i < nd; i++)
		if (!in_leaves(pa->rollup[r].dim, pa->rollup[r].ndims, dims[i]))
			return false;
	for (int i = 0; i < nm; i++)
		if (!in_leaves(pa->rollup[r].meas, pa->rollup[r].nmeas, meas[i]))
			return false;
	*out = (SiftAggPaShape) {.dim = -1, .list = group, .ndims = nd, .nmeas = nm};
	memcpy(out->rdim, dims, nd * sizeof(int));
	memcpy(out->rmeas, meas, nm * sizeof(int));
	return true;
}

bool
sift_aggspec_preagg(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, SiftAggPaShape *out)
{
	int			dim = -1,
				d = -1;

	if (s->narms || s->nsets || s->njoin || s->ncombo || s->npost || s->nvleaf || s->naggs == 0 || s->search || s->topn)
		return false;
	if (s->group >= 0)
		return (lay->leaf[s->group].kind == SCL_ARRAY || lay->leaf[s->group].kind == SCL_OBJECT) && preagg_rollup(s, lay, nbase, pa, out);
	if (s->nkeys > 1)
		return false;
	if (s->nkeys == 1)
	{
		if (s->key[0].kind != SAK_LEAF)
			return false;
		dim = s->key[0].leaf;
	}
	dim = preagg_conj_dim(s, lay, nbase, dim);
	if (dim == -2)
		return false;
	if (dim >= 0)
	{
		SiftSrcVecType vt = plain_leaf(lay, nbase, -1, dim) ? lay->leaf[dim].vtype : SCV_F64;

		d = pa_dim(pa, dim);
		if (d < 0 || (vt != SCV_I32 && vt != SCV_CODE))
			return false;
	}
	else if (!pa->aggs)
		return false;
	for (int i = 0; i < s->naggs; i++)
		if (!cube_agg_ok(&s->agg[i], lay, nbase, pa, d))
			return false;
	*out = (SiftAggPaShape) {.dim = dim, .list = -1};
	return true;
}
