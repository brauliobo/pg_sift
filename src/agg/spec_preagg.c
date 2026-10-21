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

static const char *const pawhy_name[] = {
	[SPW_OK] = "ok",
	[SPW_OFF] = "sift.agg_preagg is off",
	[SPW_NONE] = "the index stores no pre-aggregates",
	[SPW_ARMS] = "union arms",
	[SPW_SETS] = "grouping sets",
	[SPW_JOIN] = "joined table",
	[SPW_COMBO] = "combination or lookup",
	[SPW_POST] = "post-aggregation",
	[SPW_VLEAF] = "virtual leaf",
	[SPW_NO_AGG] = "no aggregate",
	[SPW_SEARCH] = "text search",
	[SPW_TOPN] = "top-N",
	[SPW_NOT_LIST] = "unnested leaf is not a list",
	[SPW_NO_ROLLUP] = "no rollup of the list",
	[SPW_KEY_EXPR] = "group key is an expression",
	[SPW_KEY_LEAF] = "group key is not a dim leaf",
	[SPW_PRED] = "conjunct is not on the dims",
	[SPW_AGG_FILTER] = "aggregate with FILTER",
	[SPW_AGG_DISTINCT] = "aggregate with DISTINCT",
	[SPW_AGG_KIND] = "aggregate not additive over tuples",
	[SPW_AGG_ARG] = "aggregate argument is not a plain leaf",
	[SPW_AGG_TYPE] = "aggregate type not summed",
	[SPW_DIM_NOT_STORED] = "dim not in the rollup",
	[SPW_MEAS_NOT_STORED] = "measure not in the rollup",
	[SPW_ROW_KEYS] = "rows: several group keys",
	[SPW_ROW_KEY] = "rows: key is not a column",
	[SPW_ROW_PRED] = "rows: conjunct the histogram cannot test",
	[SPW_ROW_NO_HIST] = "rows: no histogram of the key",
	[SPW_ROW_NO_SUMS] = "rows: no sums",
	[SPW_ZONE] = "row comparison decides no unit",
};

const char *
sift_pawhy_name(SiftPaWhy why)
{
	return pawhy_name[why];
}

/* The leaf aggregate a reads (-1: count(*)) when pre-aggregates can give it at all; group: the list leaf of an element level scan, else -1 */
static SiftPaWhy
preagg_agg_leaf(const SiftAggAgg *a, const SiftSrcLayout *lay, int nbase, int group, int *leaf_out)
{
	int			leaf = a->leaf;
	bool		sums = false,
				flt = false,
				mm = false;
	SiftSrcVecType vt;

	if (a->filter)
		return SPW_AGG_FILTER;
	if (a->distinct)
		return SPW_AGG_DISTINCT;
	switch (a->kind)
	{
		case SAA_COUNT_STAR:
			*leaf_out = -1;
			return SPW_OK;
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
			if (group < 0 || a->restype != FLOAT8OID)
				return SPW_AGG_TYPE;
			if (a->nvx)
				return SPW_AGG_ARG;
			flt = true;
			break;
		case SAA_MIN:
		case SAA_MAX:
			leaf = a->num.n ? num_leaf(&a->num) : a->leaf;
			mm = true;
			break;
		case SAA_COUNT_ROWS:
		case SAA_COUNT_DISTINCT:
			return SPW_AGG_DISTINCT;
		default:
			return SPW_AGG_KIND;
	}
	if (!plain_leaf(lay, nbase, group, leaf))
		return SPW_AGG_ARG;
	vt = lay->leaf[leaf].vtype;
	if (flt ? vt != SCV_F64 : mm ? (vt != SCV_I32 && vt != SCV_I64 && !(group >= 0 && !a->num.n && vt == SCV_F64)) : sums && vt != SCV_I32 && vt != SCV_I64)
		return SPW_AGG_TYPE;
	*leaf_out = leaf;
	return SPW_OK;
}

/* can the unit's histogram d of pa (or the sums of the leaves when d < 0) give aggregate a? */
static SiftPaWhy
cube_agg_ok(const SiftAggAgg *a, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, int d)
{
	int			leaf;
	SiftPaWhy	why = preagg_agg_leaf(a, lay, nbase, -1, &leaf);

	if (why != SPW_OK || leaf < 0 || d < 0)
		return why;
	if (a->kind == SAA_MIN || a->kind == SAA_MAX)
		return SPW_AGG_KIND;
	if (leaf == pa->dim[d].leaf)
		return a->kind == SAA_COUNT ? SPW_OK : SPW_AGG_KIND;	/* the rows of the groups that are not NULL */
	for (int m = 0; m < pa->dim[d].nmeas; m++)
		if (pa->dim[d].meas[m] == leaf)
			return SPW_OK;
	return SPW_MEAS_NOT_STORED;
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
static SiftPaWhy
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
		return SPW_NO_ROLLUP;
	for (int k = 0; k < s->nkeys; k++)
	{
		if (s->key[k].kind != SAK_LEAF)
			return SPW_KEY_EXPR;
		if (!dim_leaf(lay, nbase, group, s->key[k].leaf) || !add_leaf(dims, &nd, SIFT_SRC_MAX_RDIMS, s->key[k].leaf))
			return SPW_KEY_LEAF;
	}
	for (int i = 0; i < s->npred; i++)
	{
		int			l = sift_aggspec_pred_leaf(s->pred[i], lay, nbase, -1);

		if (l != -2 ? (l >= 0 && !list_member_ptr(cmps, s->pred[i])) : !pred_dims(s->pred[i], lay, nbase, group, dims, &nd))
			return SPW_PRED;
	}
	for (int i = 0; i < s->naggs; i++)
	{
		const SiftAggAgg *a = &s->agg[i];
		int			leaf;
		SiftPaWhy	why = preagg_agg_leaf(a, lay, nbase, group, &leaf);

		if (why != SPW_OK)
			return why;
		if (leaf < 0)
			continue;
		/* count(leaf) is the count of the dim's tuples that are not NULL, or the count of the measure */
		if (a->kind == SAA_COUNT)
		{
			if (!in_leaves(pa->rollup[r].dim, pa->rollup[r].ndims, leaf) && !in_leaves(pa->rollup[r].meas, pa->rollup[r].nmeas, leaf))
				return SPW_DIM_NOT_STORED;
		}
		else if (!add_leaf(meas, &nm, SIFT_SRC_MAX_MEAS, leaf))
			return SPW_MEAS_NOT_STORED;
	}
	for (int i = 0; i < nd; i++)
		if (!in_leaves(pa->rollup[r].dim, pa->rollup[r].ndims, dims[i]))
			return SPW_DIM_NOT_STORED;
	for (int i = 0; i < nm; i++)
		if (!in_leaves(pa->rollup[r].meas, pa->rollup[r].nmeas, meas[i]))
			return SPW_MEAS_NOT_STORED;
	*out = (SiftAggPaShape) {.dim = -1, .list = group, .ndims = nd, .nmeas = nm};
	memcpy(out->rdim, dims, nd * sizeof(int));
	memcpy(out->rmeas, meas, nm * sizeof(int));
	return SPW_OK;
}

SiftPaWhy
sift_aggspec_preagg_why(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, SiftAggPaShape *out)
{
	int			dim = -1,
				d = -1;

	if (s->narms)
		return SPW_ARMS;
	if (s->nsets)
		return SPW_SETS;
	if (s->njoin)
		return SPW_JOIN;
	if (s->ncombo)
		return SPW_COMBO;
	if (s->npost)
		return SPW_POST;
	if (s->nvleaf)
		return SPW_VLEAF;
	if (s->naggs == 0)
		return SPW_NO_AGG;
	if (s->search)
		return SPW_SEARCH;
	if (s->topn)
		return SPW_TOPN;
	if (s->group >= 0)
		return lay->leaf[s->group].kind == SCL_ARRAY || lay->leaf[s->group].kind == SCL_OBJECT ? preagg_rollup(s, lay, nbase, pa, out) : SPW_NOT_LIST;
	if (s->nkeys > 1)
		return SPW_ROW_KEYS;
	if (s->nkeys == 1)
	{
		if (s->key[0].kind != SAK_LEAF)
			return SPW_ROW_KEY;
		dim = s->key[0].leaf;
	}
	dim = preagg_conj_dim(s, lay, nbase, dim);
	if (dim == -2)
		return SPW_ROW_PRED;
	if (dim >= 0)
	{
		SiftSrcVecType vt = plain_leaf(lay, nbase, -1, dim) ? lay->leaf[dim].vtype : SCV_F64;

		d = pa_dim(pa, dim);
		if (d < 0 || (vt != SCV_I32 && vt != SCV_CODE))
			return SPW_ROW_NO_HIST;
	}
	else if (!pa->aggs)
		return SPW_ROW_NO_SUMS;
	for (int i = 0; i < s->naggs; i++)
	{
		SiftPaWhy	why = cube_agg_ok(&s->agg[i], lay, nbase, pa, d);

		if (why != SPW_OK)
			return why;
	}
	*out = (SiftAggPaShape) {.dim = dim, .list = -1};
	return SPW_OK;
}

bool
sift_aggspec_preagg(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, SiftAggPaShape *out)
{
	return sift_aggspec_preagg_why(s, lay, nbase, pa, out) == SPW_OK;
}
