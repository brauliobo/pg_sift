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

int
sift_aggnum_linear(const SiftAggNum *n, sift_i128 *mul, sift_i128 *add)
{
	const SiftAggNx *s = n->step;
	int			l = 0,
				k = 1;

	*mul = 1;
	*add = 0;
	if (n->n == 1)
		return num_leaf(n);
	if (n->n != 4 || s[3].op != SNX_INTCAST || s[3].b != 1)
		return -1;
	if (s[0].op == SNX_CONST && s[1].op == SNX_LEAF)
	{
		l = 1;
		k = 0;
	}
	else if (s[0].op != SNX_LEAF || s[1].op != SNX_CONST)
		return -1;
	switch (s[2].op)
	{
		case SNX_ADD:
			*add = s[k].c;
			break;
		case SNX_SUB:
			if (l == 0)
				*add = -s[k].c;
			else
			{
				*mul = -1;
				*add = s[k].c;
			}
			break;
		case SNX_MUL:
			*mul = s[k].c;
			break;
		default:
			return -1;
	}
	return s[l].leaf;
}

/* the leaf of the histogram `dim` in pa, -1 if the index writes none */
static int
pa_dim(const SiftSrcPreagg *pa, int dim)
{
	for (int d = 0; d < pa->ndims; d++)
		if (pa->dim[d].leaf == dim && !pa->dim[d].cal)
			return d;
	return -1;
}

/* does the index keep the key list or bitmap of the integer leaf in its units (key_columns) */
static bool
pa_pres(const SiftSrcPreagg *pa, int leaf)
{
	for (int p = 0; p < pa->npres; p++)
		if (pa->pres[p] == leaf)
			return true;
	return false;
}

/* the histogram of the calendar cells of the date or timestamp leaf in pa, -1 if the index writes none */
static int
pa_cal(const SiftSrcPreagg *pa, int leaf)
{
	for (int d = 0; d < pa->ndims; d++)
		if (pa->dim[d].leaf == leaf && pa->dim[d].cal)
			return d;
	return -1;
}

/* the text of an EXPLAIN line, and the stable name of sift.col_rollup_misses */
static const struct
{
	const char *name;
	const char *slug;
}			pawhy[] = {
	[SPW_OK] = {"ok", "ok"},
	[SPW_OFF] = {"sift.agg_preagg is off", "preagg_off"},
	[SPW_NONE] = {"the index stores no pre-aggregates", "no_preagg"},
	[SPW_ARMS] = {"union arms", "union_arms"},
	[SPW_SETS] = {"grouping sets", "grouping_sets"},
	[SPW_JOIN] = {"joined table", "joined_table"},
	[SPW_COMBO] = {"combination or lookup", "combination"},
	[SPW_POST] = {"post-aggregation", "post_aggregation"},
	[SPW_VLEAF] = {"virtual leaf", "virtual_leaf"},
	[SPW_NO_AGG] = {"no aggregate", "no_aggregate"},
	[SPW_SEARCH] = {"text search", "text_search"},
	[SPW_TOPN] = {"top-N", "top_n"},
	[SPW_NOT_LIST] = {"unnested leaf is not a list", "not_a_list"},
	[SPW_NO_ROLLUP] = {"no rollup of the list", "no_rollup"},
	[SPW_KEY_EXPR] = {"group key is an expression", "key_expression"},
	[SPW_KEY_LEAF] = {"group key is not a dim leaf", "key_not_a_dim"},
	[SPW_PRED] = {"conjunct is not on the dims", "conjunct_not_on_dims"},
	[SPW_AGG_FILTER] = {"aggregate with FILTER", "aggregate_filter"},
	[SPW_AGG_DISTINCT] = {"aggregate with DISTINCT", "aggregate_distinct"},
	[SPW_AGG_KIND] = {"aggregate not additive over tuples", "aggregate_kind"},
	[SPW_AGG_ARG] = {"aggregate argument is not a plain leaf", "aggregate_argument"},
	[SPW_AGG_TYPE] = {"aggregate type not summed", "aggregate_type"},
	[SPW_DIM_NOT_STORED] = {"dim not in the rollup", "dim_not_stored"},
	[SPW_MEAS_NOT_STORED] = {"measure not in the rollup", "measure_not_stored"},
	[SPW_ROW_KEYS] = {"rows: several group keys", "rows_several_keys"},
	[SPW_ROW_KEY] = {"rows: key is not a column", "rows_key_not_a_column"},
	[SPW_ROW_PRED] = {"rows: conjunct the histogram cannot test", "rows_conjunct"},
	[SPW_ROW_NO_HIST] = {"rows: no histogram of the key", "rows_no_histogram"},
	[SPW_ROW_NO_SUMS] = {"rows: no sums", "rows_no_sums"},
	[SPW_ZONE] = {"row comparison decides no unit", "zone_decides_nothing"},
	[SPW_ROW_CAL_ZONE] = {"rows: calendar key uses the session time zone", "rows_key_time_zone"},
	[SPW_ROW_CAL_FINE] = {"rows: calendar key finer than a day", "rows_key_finer_than_day"},
};

StaticAssertDecl(lengthof(pawhy) == SPW_ROW_CAL_FINE + 1, "pawhy lists every SiftPaWhy");

const char *
sift_pawhy_name(SiftPaWhy why)
{
	return pawhy[why].name;
}

const char *
sift_pawhy_slug(SiftPaWhy why)
{
	return pawhy[why].slug;
}


/* would other options of the index (rollup, preagg, cube) answer the scan: it needs a dim or measure the index does not store */
bool
sift_pawhy_fixable(SiftPaWhy why)
{
	switch (why)
	{
		case SPW_NO_ROLLUP:
		case SPW_KEY_LEAF:
		case SPW_DIM_NOT_STORED:
		case SPW_MEAS_NOT_STORED:
		case SPW_ROW_NO_HIST:
		case SPW_ROW_NO_SUMS:
			return true;
		default:
			return false;
	}
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
			{
				sift_i128	mul,
							add;

				leaf = sift_aggnum_linear(&a->num, &mul, &add);
			}
			/* the arithmetic of an element's value is not checked against the zone of the unit: the rollup's tuples hold sums only */
			if (group >= 0 && a->num.n > 1)
				return SPW_AGG_ARG;
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

/* count(DISTINCT leaf) of a plain 32-bit or text leaf: the groups of its own histogram are its distinct values */
static bool
distinct_leaf(const SiftAggAgg *a, const SiftSrcLayout *lay)
{
	return a->kind == SAA_COUNT_DISTINCT && sift_agg_preagg_distinct && !a->num.n && !a->filter && a->leaf >= 0 &&
		lay->leaf[a->leaf].kind == SCL_SCALAR && (lay->leaf[a->leaf].vtype == SCV_I32 || lay->leaf[a->leaf].vtype == SCV_CODE);
}

bool		sift_agg_preagg_distinct = true;

/* can the unit's histogram d of pa (or the sums of the leaves when d < 0) give aggregate a? */
static SiftPaWhy
cube_agg_ok(const SiftAggAgg *a, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, int d)
{
	int			leaf;
	SiftPaWhy	why;

	if (distinct_leaf(a, lay))
		return (d >= 0 && a->leaf == pa->dim[d].leaf) || (d < 0 && pa_pres(pa, a->leaf)) ? SPW_OK : SPW_AGG_DISTINCT;
	why = preagg_agg_leaf(a, lay, nbase, -1, &leaf);

	if (why != SPW_OK || leaf < 0 || d < 0)
		return why;
	if (leaf == pa->dim[d].leaf)
		return a->kind == SAA_COUNT ? SPW_OK : SPW_AGG_KIND;	/* the rows of the groups that are not NULL */
	for (int m = 0; m < pa->dim[d].nmeas; m++)
		if (pa->dim[d].meas[m] == leaf)
			return (a->kind == SAA_MIN || a->kind == SAA_MAX) && !pa->dim[d].minmax ? SPW_AGG_KIND : SPW_OK;
	return SPW_MEAS_NOT_STORED;
}

/* Can the histogram of leaf answer a test of it or be a group key? The keys of a 32-bit leaf, a dictionary code, an int8 or a numeric(p,s) (its scaled integer) */
static bool
hist_leaf_ok(const SiftSrcLayout *lay, int nbase, int leaf)
{
	SiftSrcVecType vt = plain_leaf(lay, nbase, -1, leaf) ? lay->leaf[leaf].vtype : SCV_F64;

	return vt == SCV_I32 || vt == SCV_CODE || vt == SCV_I64;
}

/* the conjuncts a unit's zone decides: the comparisons and, in a shape that has them (nullz), the NULL tests */
static List *
zone_conjs(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, bool nullz)
{
	List	   *out = sift_aggspec_zone_cmps(s, lay, nbase, false);

	return nullz ? list_concat(out, sift_aggspec_zone_cmps(s, lay, nbase, true)) : out;
}

/*
 * The leaf the conjuncts test besides the zone comparisons (-1: none), or -2 when they cannot be answered from a histogram. The conjuncts must be
 * zone comparisons, or test one scalar leaf. *zdim: the first leaf of a zone comparison that has a histogram (-1: none), which tests it when the
 * zone of a unit is not inside the comparison.
 */
static int
preagg_conj_dim(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, int dim, const SiftSrcPreagg *pa, bool nullz, int *zdim)
{
	List	   *cmps = zone_conjs(s, lay, nbase, nullz);

	*zdim = -1;
	for (int i = 0; i < s->npred; i++)
	{
		int			l = sift_aggspec_pred_leaf(s->pred[i], lay, nbase, -1);

		if (l == -2)
			return -2;
		/* a conjunct on a leaf the zones of a unit can decide whole is no reason for a histogram; every other one is a test of the dimension. The cells of a timestamp stand in for
		 * its histogram, which a unit that spans more than 32 instants does not have */
		if (l >= 0 && *zdim < 0 && s->pred[i]->kind != SAP_NULL && list_member_ptr(cmps, s->pred[i]) && pa_dim(pa, l) >= 0 && hist_leaf_ok(lay, nbase, l) &&
			!(lay->leaf[l].vtype == SCV_I64 && pa_cal(pa, l) >= 0))
			*zdim = l;
		if (l < 0 || list_member_ptr(cmps, s->pred[i]))
			continue;
		if (dim >= 0 && dim != l)
			return -2;
		dim = l;
	}
	return dim;
}

/* The leaf the FILTERs of the aggregates test (-1: none), or -2 when one tests more than one leaf or something a histogram cannot answer */
static int
preagg_filter_dim(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase)
{
	int			dim = -1;

	for (int i = 0; i < s->naggs; i++)
	{
		int			l = s->agg[i].filter ? sift_aggspec_pred_leaf(s->agg[i].filter, lay, nbase, -1) : -1;

		if (l == -2 || (l >= 0 && dim >= 0 && l != dim))
			return -2;
		dim = l >= 0 ? l : dim;
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

/* Adds the leaves a predicate of the elements of group (of the rows: -1) tests to dims; false when it tests more than plain dim leaves (a combo, a late expression) */
static bool
pred_dims(const SiftAggPred *p, const SiftSrcLayout *lay, int nbase, int group, int *dims, int *nd)
{
	if (p->late || p->level != (group >= 0))
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
 * count(leaf) reads; the measures the other aggregates read. Conjuncts of the rows must be zone comparisons. A scan of the rows (group -1) is
 * answered by the rollup of the rows the same way, where every conjunct tests dims.
 */
static SiftPaWhy
preagg_rollup(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, bool nullz, SiftAggPaShape *out)
{
	List	   *cmps = sift_aggspec_zone_cmps(s, lay, nbase, nullz);
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
		if (s->key[k].kind == SAK_SET)
			continue;
		if (s->key[k].kind != SAK_LEAF && s->key[k].kind != SAK_LUT)
			return SPW_KEY_EXPR;
		if (!dim_leaf(lay, nbase, group, s->key[k].leaf) || !add_leaf(dims, &nd, SIFT_SRC_MAX_RDIMS, s->key[k].leaf))
			return SPW_KEY_LEAF;
	}
	for (int i = 0; i < s->npred; i++)
	{
		int			l = group < 0 ? -2 : sift_aggspec_pred_leaf(s->pred[i], lay, nbase, -1);

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
		/* FILTER (WHERE ...) runs on the tuples: it may test dims only */
		if (a->filter && !pred_dims(a->filter, lay, nbase, group, dims, &nd))
			return SPW_AGG_FILTER;
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
	*out = (SiftAggPaShape) {.dim = -1, .hdim = -1, .list = group, .rows = group < 0, .ndims = nd, .nmeas = nm, .nullz = nullz};
	memcpy(out->rdim, dims, nd * sizeof(int));
	memcpy(out->rmeas, meas, nm * sizeof(int));
	return SPW_OK;
}

/* a test of calendar expressions of one leaf or of its NULLs: the grains of the cells that hold one value of it go into *mask */
static bool
cal_pred(const SiftAggPred *p, uint8 *mask)
{
	uint8		m;

	switch (p->kind)
	{
		case SAP_AND:
		case SAP_OR:
			for (int i = 0; i < p->nchild; i++)
				if (!cal_pred(p->child[i], mask))
					return false;
			return true;
		case SAP_NULL:
			return p->op <= 1;
		case SAP_LUT:
			if (sift_aggexpr_cal(p->expr, &m) != SAC_OK)
				return false;
			*mask &= m;
			return true;
		default:
			return false;
	}
}

/*
 * Can the cells of the calendar dim of leaf test the conjuncts on it? Its zone comparisons, and the ANDs and ORs of them, are decided by the span of each cell
 * (a unit with a cell that one cuts is scanned); the others must be tests of calendar expressions of the leaf or of its NULLs, which a cell answers when its grain is in *mask.
 */
static bool
cal_tests(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, int leaf, uint8 *mask)
{
	List	   *cmps = sift_aggspec_zone_cmps(s, lay, nbase, false);

	for (int i = 0; i < s->npred; i++)
		if (sift_aggspec_pred_leaf(s->pred[i], lay, nbase, -1) == leaf && !list_member_ptr(cmps, s->pred[i]) && !sift_aggspec_span_pred(s->pred[i], lay, nbase) &&
			!cal_pred(s->pred[i], mask))
			return false;
	return *mask != 0;
}

/*
 * Does the calendar dim of the date leaf dim answer a unit that has no histogram of the leaf (more days than a histogram holds): no key, and every test of the
 * leaf a comparison tree whose truth a cell's span decides, which the aggregates can all be read from the cells of
 */
static bool
cal_alt(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, int dim)
{
	int			cd = pa_cal(pa, dim);
	bool		any = false;

	if (cd < 0 || s->nkeys != 0)
		return false;
	for (int i = 0; i < s->npred; i++)
		if (sift_aggspec_pred_leaf(s->pred[i], lay, nbase, -1) == dim)
		{
			if (!sift_aggspec_span_pred(s->pred[i], lay, nbase))
				return false;
			any = true;
		}
	for (int i = 0; any && i < s->naggs; i++)
		if (s->agg[i].filter || distinct_leaf(&s->agg[i], lay) || cube_agg_ok(&s->agg[i], lay, nbase, pa, cd) != SPW_OK)
			return false;
	return any;
}

/* the sums and histograms of the rows' leaves answer a scan of the rows: at most one key, a column of a histogram */
static SiftPaWhy
preagg_rows(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, bool nullz, SiftAggPaShape *out)
{
	int			dim = -1,
				d = -1,
				hdim = -1,
				cleaf = -1,
				fdim,
				zdim;
	uint8		cmask = 0;
	bool		hcal = false,
				hcalalt = false,
				calalt = false;
	SiftPaWhy	why = SPW_OK;

	if (s->nkeys > 1)
		return SPW_ROW_KEYS;
	if (s->nkeys == 1)
	{
		/* a group key of the column itself: a 32-bit or text leaf, or the int8 or numeric one that the keys of a program of one leaf stand for */
		dim = s->key[0].kind == SAK_LEAF ? s->key[0].leaf : s->key[0].kind == SAK_NUM ? num_leaf(&s->key[0].num) : -1;
		/* a calendar key (date_trunc, extract, ::date of a date or timestamp): the cells of the calendar dim of its leaf, when it is the same all cell */
		if (dim < 0)
		{
			int			cal = sift_aggkey_cal(&s->key[0], &cmask);

			if (cal == SAC_ZONE)
				return SPW_ROW_CAL_ZONE;
			if (cal == SAC_FINE)
				return SPW_ROW_CAL_FINE;
			if (cal != SAC_OK)
				return SPW_ROW_KEY;
			cleaf = s->key[0].leaf;
		}
	}
	dim = preagg_conj_dim(s, lay, nbase, dim, pa, nullz, &zdim);
	if (dim == -2)
		return SPW_ROW_PRED;
	/* the tests of the key's calendar leaf are made on its cells; those of another leaf are not */
	if (cleaf >= 0 && dim >= 0)
	{
		if (dim != cleaf || !cal_tests(s, lay, nbase, cleaf, &cmask))
			return SPW_ROW_PRED;
		dim = -1;
	}
	/* the FILTERs and the other tests of a dim run on the groups of one histogram: the key's, the conjunct's or theirs */
	fdim = preagg_filter_dim(s, lay, nbase);
	if (fdim == -2 || (fdim >= 0 && dim >= 0 && dim != fdim) || (fdim >= 0 && cleaf >= 0))
		return SPW_AGG_FILTER;
	dim = fdim >= 0 ? fdim : dim;
	/* count(DISTINCT leaf) reads the groups of the histogram of the leaf itself, not of its cells */
	for (int i = 0; i < s->naggs; i++)
		if (distinct_leaf(&s->agg[i], lay))
		{
			/* without a key, the units of a leaf of key_columns give its values as a bitmap each (preagg_presence) */
			if (s->nkeys == 0 && pa_pres(pa, s->agg[i].leaf))
				continue;
			if (cleaf >= 0 || (dim >= 0 && dim != s->agg[i].leaf))
				return SPW_AGG_DISTINCT;
			dim = s->agg[i].leaf;
		}
	/* tests of a date or timestamp that has no histogram of its own: its cells, when they are calendar tests */
	if (cleaf < 0 && dim >= 0 && fdim < 0 && pa_dim(pa, dim) < 0 && pa_cal(pa, dim) >= 0)
	{
		cmask = 0x0E;
		if (cal_tests(s, lay, nbase, dim, &cmask))
		{
			cleaf = dim;
			dim = -1;
		}
	}
	if (cleaf >= 0)
	{
		d = pa_cal(pa, cleaf);
		if (d < 0)
			return SPW_ROW_NO_HIST;
		dim = cleaf;
		zdim = -1;
	}
	else if (dim >= 0)
	{
		d = pa_dim(pa, dim);
		if (d < 0 || !hist_leaf_ok(lay, nbase, dim))
			return SPW_ROW_NO_HIST;
	}
	else if (!pa->aggs)
		why = SPW_ROW_NO_SUMS;
	for (int i = 0; why == SPW_OK && i < s->naggs; i++)
		why = cube_agg_ok(&s->agg[i], lay, nbase, pa, d);
	/* the units the sums cannot answer (their zone is not inside the comparison) answer from the histogram of the dim it tests, when it holds the aggregates */
	if (dim < 0 && zdim >= 0)
	{
		SiftPaWhy	zwhy = SPW_OK;

		for (int i = 0; zwhy == SPW_OK && i < s->naggs; i++)
			zwhy = cube_agg_ok(&s->agg[i], lay, nbase, pa, pa_dim(pa, zdim));
		if (zwhy == SPW_OK)
		{
			if (why == SPW_OK)
			{
				int			cd = pa_cal(pa, zdim);

				hdim = zdim;
				/* a date whose unit has too many days for a histogram of its own still has its cells */
				hcalalt = cd >= 0;
				for (int i = 0; hcalalt && i < s->naggs; i++)
					hcalalt = cube_agg_ok(&s->agg[i], lay, nbase, pa, cd) == SPW_OK;
				if (hcalalt)
					cmask = 0x0E;
			}
			else
				dim = zdim;
			why = SPW_OK;
		}
	}
	/* a unit that keeps no key list or bitmap of a count(DISTINCT) leaf answers from the histogram of the cube when the leaf is a dim of it too */
	for (int i = 0; dim < 0 && hdim < 0 && cleaf < 0 && s->nkeys == 0 && why == SPW_OK && i < s->naggs; i++)
	{
		int			pd = distinct_leaf(&s->agg[i], lay) && pa_pres(pa, s->agg[i].leaf) ? pa_dim(pa, s->agg[i].leaf) : -1;
		SiftPaWhy	pwhy = pd >= 0 && hist_leaf_ok(lay, nbase, s->agg[i].leaf) ? SPW_OK : SPW_AGG_DISTINCT;

		for (int j = 0; pwhy == SPW_OK && j < s->naggs; j++)
			pwhy = cube_agg_ok(&s->agg[j], lay, nbase, pa, pd);
		if (pwhy == SPW_OK)
			hdim = s->agg[i].leaf;
	}
	/* ... or from the cells of a date or timestamp it compares, whose span decides each cell (a unit with a cell the comparison cuts is scanned) */
	if (dim < 0 && hdim < 0 && cleaf < 0 && why == SPW_OK)
	{
		List	   *cmps = sift_aggspec_zone_cmps(s, lay, nbase, false);

		foreach_ptr(SiftAggPred, p, cmps)
		{
			int			l = sift_aggspec_pred_leaf(p, lay, nbase, -1),
						cd = l >= 0 ? pa_cal(pa, l) : -1;
			SiftPaWhy	cwhy = SPW_OK;

			for (int i = 0; cd >= 0 && cwhy == SPW_OK && i < s->naggs; i++)
				cwhy = cube_agg_ok(&s->agg[i], lay, nbase, pa, cd);
			if (cd >= 0 && cwhy == SPW_OK)
			{
				hdim = l;
				hcal = true;
				cmask = 0x0E;
				break;
			}
		}
	}
	if (why != SPW_OK)
		return why;
	if (cleaf < 0 && dim >= 0 && hdim < 0 && fdim < 0 && cal_alt(s, lay, nbase, pa, dim))
	{
		calalt = true;
		cmask = 0x0E;
	}
	*out = (SiftAggPaShape) {.dim = dim, .cal = cleaf >= 0, .calmask = cmask, .hcal = hcal, .hcalalt = hcalalt, .calalt = calalt, .hdim = hdim, .list = -1, .nullz = nullz};
	return SPW_OK;
}

/* does the answer of the sums and histograms depend on the zone of a unit lying inside a comparison with a column other than the histogram's: only the units that do are answered */
static bool
rows_zone_bound(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, const SiftAggPaShape *shape)
{
	foreach_ptr(SiftAggPred, p, zone_conjs(s, lay, nbase, shape->nullz))
		if (sift_aggspec_pred_leaf(p, lay, nbase, -1) != shape->dim)
			return true;
	return false;
}

SiftPaWhy
sift_aggspec_preagg_why(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, SiftAggPaShape *out)
{
	SiftPaWhy	why;

	if (s->narms)
		return SPW_ARMS;
	if (s->nsets && (s->group < 0 || s->setpred || s->setaggs))
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
	{
		if (lay->leaf[s->group].kind != SCL_ARRAY && lay->leaf[s->group].kind != SCL_OBJECT)
			return SPW_NOT_LIST;
		why = preagg_rollup(s, lay, nbase, pa, false, out);
		/* a NULL test of a column of the rows (WHERE deleted_at IS NULL) is decided by the zone of each unit, or by the exceptions the unit kept (unit_preagg.c) */
		if (why == SPW_PRED && sift_aggspec_zone_cmps(s, lay, nbase, true) != NIL)
			why = preagg_rollup(s, lay, nbase, pa, true, out);
		return why;
	}
	why = preagg_rows(s, lay, nbase, pa, false, out);
	/* a NULL test of a column that is no dim of the shape (WHERE deleted_at IS NULL): the zone's NULL count decides it for each unit, as a comparison is decided */
	if (why != SPW_OK && sift_aggspec_zone_cmps(s, lay, nbase, true) != NIL)
	{
		SiftAggPaShape nz;

		SiftPaWhy	nwhy = preagg_rows(s, lay, nbase, pa, true, &nz);

		if (nwhy == SPW_OK)
			*out = nz;
		why = nwhy;
	}
	/*
	 * What the histograms cannot give (two keys, a key and a test of another column), or give only for the units inside a comparison, the rollup of the
	 * rows may: it answers every exact unit.
	 */
	for (int r = 0; r < pa->nrollups; r++)
		if (pa->rollup[r].list < 0 && (why != SPW_OK || rows_zone_bound(s, lay, nbase, out)))
		{
			SiftAggPaShape rows;
			SiftPaWhy	rwhy = preagg_rollup(s, lay, nbase, pa, false, &rows);

			if (rwhy == SPW_OK)
				*out = rows;
			return rwhy == SPW_OK || why == SPW_OK ? SPW_OK : rwhy;
		}
	return why;
}

bool
sift_aggspec_preagg(const SiftAggSpec *s, const SiftSrcLayout *lay, int nbase, const SiftSrcPreagg *pa, SiftAggPaShape *out)
{
	return sift_aggspec_preagg_why(s, lay, nbase, pa, out) == SPW_OK;
}

double		sift_agg_rollup_cost = 1.0;

/* instructions of an aggregate of an element in the scan: a count(*) costs nothing, the sums and counts of a leaf a few, a min or max of a float many */
static double
agg_elem_instr(const SiftAggAgg *a, const SiftSrcLayout *lay)
{
	double		c = a->kind == SAA_COUNT_STAR ? 0.0 : 8.5;

	if (a->kind == SAA_MIN || a->kind == SAA_MAX)
		c = a->leaf >= 0 && lay->leaf[a->leaf].vtype == SCV_F64 ? 57.0 : 25.0;
	return c + (a->filter ? 10.0 : 0.0);
}

void
sift_aggspec_unit_costs(const SiftAggSpec *s, const SiftSrcLayout *lay, double *elem, double *tuple)
{
	double		e = 5.0,
				t = 300.0;
	int			nmeas = 0,
				nmm = 0,
				meas[SIFT_SRC_MAX_MEAS];

	e += s->nkeys >= 1 ? 12.0 : 0.0;
	e += s->nkeys >= 2 ? 5.0 : 0.0;
	e += s->nkeys >= 3 ? 28.0 * (s->nkeys - 2) : 0.0;
	t += 45.0 * s->nkeys;
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->level == (s->group >= 0))
		{
			e += 3.0;
			t += 10.0;
		}
	for (int i = 0; i < s->naggs; i++)
	{
		const SiftAggAgg *a = &s->agg[i];
		bool		mm = a->kind == SAA_MIN || a->kind == SAA_MAX,
					seen = false;

		e += agg_elem_instr(a, lay);
		t += a->filter ? 10.0 : 0.0;
		if (a->kind == SAA_COUNT_STAR || a->leaf < 0)
			continue;
		for (int k = 0; k < nmeas; k++)
			seen |= meas[k] == a->leaf;
		if (!seen && nmeas < SIFT_SRC_MAX_MEAS)
			meas[nmeas++] = a->leaf;
		nmm += mm && !seen;
	}
	*elem = e;
	*tuple = t + 90.0 * nmeas + 120.0 * nmm;
}
