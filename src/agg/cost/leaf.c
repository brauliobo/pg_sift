/*
 * agg/cost/leaf.c - what reading a leaf costs
 * Its bytes and values, its filters, the dictionary entries it maps and the distinct sets
 * it builds.
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_statistic.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"

#include "agg/agg.h"
#include "agg/cost/int.h"
#include "agg/dist_int.h"

/* does a predicate test the leaf? `values`: its values, not only its null flags, and the leaves of a combo slot */
bool
pred_uses_leaf(const SiftAggSpec *s, const SiftAggPred *p, int leaf, bool values)
{
	if (p == NULL)
		return false;
	if (values && p->kind == SAP_COMBO)
	{
		for (int i = 0; i < s->combo[p->leaf].nleaves; i++)
			if (s->combo[p->leaf].leaf[i] == leaf)
				return true;
		return false;
	}
	if (p->kind != SAP_AND && p->kind != SAP_OR && (p->leaf == leaf || p->leaf2 == leaf) && !(values && p->kind == SAP_NULL))
		return true;
	for (int i = 0; i < p->nchild; i++)
		if (pred_uses_leaf(s, p->child[i], leaf, values))
			return true;
	return false;
}

/* is the leaf read by something besides the text tests that run on the encoded strings? */
static bool
leaf_read_whole(AggCtx *c, SiftAggSpec *s, int leaf)
{
	SiftSrcStrTest t;

	/* combos and joins read leaves in ways not followed here */
	if (s->ncombo > 0 || c->joins != NIL)
		return true;
	for (int i = 0; i < s->nkeys; i++)
		if (s->key[i].leaf == leaf && s->key[i].kind != SAK_NUM && s->key[i].kind != SAK_SET)
			return true;
	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].leaf == leaf || pred_uses_leaf(s, s->agg[i].filter, leaf, false))
			return true;
	for (int i = 0; i < s->npred; i++)
		if (pred_uses_leaf(s, s->pred[i], leaf, false) && !(s->pred[i]->leaf == leaf && sift_aggspec_str_test(s->pred[i], c->lay, c->lay->nleaves - c->nvleaf, &t)))
			return true;
	return false;
}

/* The average width of the heap column a plain text leaf is read from (0: unknown, or the leaf is a jsonb path or an expression) */
static int
leaf_heap_width(AggCtx *c, int leaf)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL && lf->parent < 0 ? list_nth(c->attexpr, lf->attno - 1) : NULL;

	return e && IsA(e, Var) ? get_attavgwidth(planner_rt_fetch(c->relid, c->root)->relid, ((Var *) e)->varattno) : 0;
}

/* the share of the rows of a leaf that PostgreSQL's statistics expect to hold a distinct value; 0 when it has none (a path of a document, an expression) */
static double
leaf_stat_frac(AggCtx *c, int leaf, double rows)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL && lf->parent < 0 ? list_nth(c->attexpr, lf->attno - 1) : NULL;
	HeapTuple	tup;
	double		nd = 0;

	if (e == NULL || !IsA(e, Var))
		return 0;
	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid),
						  Int16GetDatum(((Var *) e)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tup))
		return 0;
	nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
	ReleaseSysCache(tup);
	return nd > 0 ? Min(nd / Max(rows, 1.0), 1.0) : Min(-nd, 1.0);
}

/* ... 1 when it has none */
static double
leaf_distinct_frac(AggCtx *c, int leaf, double rows)
{
	double		frac = leaf_stat_frac(c, leaf, rows);

	return frac > 0 ? frac : 1.0;
}

/*
 * Text leaves of long strings: a unit stores the strings inline, and reading the leaf as codes builds a dictionary of the unit
 * (measured 0.5 us a row, which the heap scan beats when few rows pass); a test on the encoded strings costs a few ns a row.
 */
static double
inline_text_cost(AggCtx *c, SiftAggSpec *s)
{
	double		cost = 0;

	for (int i = 0; i < s->nleaves; i++)
	{
		const SiftSrcLeaf *lf = &c->lay->leaf[s->leaves[i]];

		if (lf->vtype == SCV_CODE && lf->kind == SCL_SCALAR && lf->parent < 0 && leaf_heap_width(c, s->leaves[i]) >= AGG_INLINE_WIDTH)
			cost += leaf_read_whole(c, s, s->leaves[i]) ? AGG_INLINE_ROW : AGG_INLINE_TEST;
	}
	return cost;
}

/*
 * The operators one unit costs from the pre-aggregates: the groups of a histogram, or the tuples of the rollup of the list (the product of the distinct
 * values of its dims, at most the elements of the unit and the 4096 a unit holds), which the executor leaves for the scan of the elements when they cost less.
 */
double
preagg_unit_ops(AggCtx *c, SiftAggSpec *s, double units, double rows, double rows_out)
{
	double		tuples = 1.0,
				elems,
				ecost,
				tcost;
	SiftAggPaShape shape;

	if (s->group < 0 && (!sift_aggspec_preagg(s, c->lay, c->lay->nleaves, &c->pa, &shape) || !shape.rows))
	{
		double		sketches = 0;

		for (int i = 0; i < s->naggs; i++)
			sketches += s->agg[i].kind == SAA_GEN && s->agg[i].gen->hll >= 0 ? AGG_PREAGG_HLL : 0.0;
		return Min(rows_out, 256.0) * AGG_PREAGG_GROUP + sketches;
	}
	for (int r = 0; r < c->pa.nrollups; r++)
		if (c->pa.rollup[r].list == s->group)
			for (int d = 0; d < c->pa.rollup[r].ndims; d++)
				tuples *= Max(c->stats.ndistinct[c->pa.rollup[r].dim[d]], 2.0);
	elems = rows / units * (s->group >= 0 ? elems_of(c, s->group) : 1.0);
	sift_aggspec_unit_costs(s, c->lay, &ecost, &tcost);
	return Min(Min(tuples, 4096.0) * tcost * sift_agg_rollup_cost, elems * ecost) / SIFT_AGG_INSTR_PER_OP;
}

/* The share of the units the pre-aggregates answer (a unit is exact when its pages are all-visible, its superblock plain); 0 when the spec cannot use them */
double
preagg_share(AggCtx *c, SiftAggSpec *s)
{
	SiftAggPaShape shape;
	int			dim;
	double		miss = 0.0;
	bool		minmax = false;

	if (!sift_agg_preagg || !sift_aggspec_preagg(s, c->lay, c->lay->nleaves, &c->pa, &shape))
		return 0.0;
	/* a unit written before the options asked for the section it is answered from has none and is scanned: a half refreshed index is priced for the units that have it */
	for (int i = 0; i < s->naggs; i++)
		minmax |= s->agg[i].kind == SAA_MIN || s->agg[i].kind == SAA_MAX;
	dim = shape.dim >= 0 ? shape.dim : shape.hdim;
	if (shape.rows)
		miss = c->stats.rows_miss;
	else if (shape.list >= 0)
		miss = c->stats.roll_miss[shape.list];
	else if (dim >= 0)
		miss = minmax ? c->stats.mm_miss[dim] : c->stats.dim_miss[dim];
	/* a unit whose zone straddles a bound of the query is scanned, not answered: the units are exact only where a range on a leaf holds them whole */
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_CMP && s->pred[i]->leaf >= 0 && s->pred[i]->op != SCMP_NE)
			miss = 1.0 - (1.0 - miss) * (1.0 - Min(c->stats.zone_span[s->pred[i]->leaf], 1.0));
	return Min(Max(c->stats.exact_frac, 0.0), 1.0) * (1.0 - Min(Max(miss, 0.0), 1.0));
}

/* The bytes of a leaf's values in the executor's vectors: the weight of its share of the stored bytes */
static double
leaf_width(const SiftSrcLeaf *lf)
{
	return lf->vtype == SCV_I64 || lf->vtype == SCV_F64 ? 8.0 : 4.0;
}

/*
 * The stored bytes the scan reads: the leaves it reads take their share (by value width) of the compressed bytes of the source; a
 * source that does not know them (a table, a sample) is taken to compress its values by half.
 */
double
scan_bytes(AggCtx *c, SiftAggSpec *s, double rows)
{
	double		all = 0,
				used = 0;

	for (int i = 0; i < c->lay->nleaves; i++)
		all += leaf_width(&c->lay->leaf[i]);
	for (int i = 0; i < s->nleaves; i++)
		used += leaf_width(leaf_of(c, s->leaves[i]));
	return c->stats.bytes > 0 ? c->stats.bytes * used / Max(all, 1.0) : rows * used * 0.5;
}

/*
 * The rows read again from the heap: of the rows of the units a leaf the scan rechecks flags (the share of units the zones say), the share of them that
 * hold an overflow value (a sample of the flagged units says; AGG_RECHECK_FRAC where it could not be taken), one row of each unit at least. A table with
 * half of its documents dirty reads half of them again: past a few percent the normal plan is the cheaper one.
 */
double
recheck_cost(AggCtx *c, SiftAggSpec *s, CostAcc *k)
{
	double		rows = 0,
				flagged = 0;

	for (int i = 0; i < s->nleaves + s->nvleaf; i++)
	{
		int			leaf;
		double		frac;

		if (i < s->nleaves)
		{
			if (s->recheck[i] == SRK_NONE)
				continue;
			leaf = s->leaves[i];
		}
		else
		{
			if (s->vleaf[i - s->nleaves].recheck == SRK_NONE)
				continue;
			leaf = vleaf_source(c, i - s->nleaves);
		}
		frac = c->stats.ovf_frac[leaf] < 0 ? AGG_RECHECK_FRAC : c->stats.ovf_frac[leaf];
		rows = Max(rows, k->rows * c->stats.ovf_units[leaf] * frac);
		flagged = Max(flagged, c->stats.ovf_units[leaf]);
	}
	return Max(rows, k->units * flagged) * AGG_RECHECK_ROW;
}

/* is the leaf one the source tests on its stored values (a comparison, IN list or null test of the query's WHERE): decoded for every row of a unit */
static bool
leaf_filtered(const SiftAggSpec *s, int leaf)
{
	for (int i = 0; i < s->npred; i++)
		if ((s->pred[i]->kind == SAP_CMP || s->pred[i]->kind == SAP_IN || s->pred[i]->kind == SAP_NULL || s->pred[i]->kind == SAP_AND) &&
			pred_uses_leaf(s, s->pred[i], leaf, false))
			return true;
	return false;
}

/* is every reader of the leaf a list (array_agg, string_agg), a count of its values or a group key: its dictionary entries are copied, not computed on? */
static bool
leaf_copy_only(const SiftAggSpec *s, int leaf, bool *sums, bool *keyed)
{
	bool		copied = false;

	*sums = *keyed = false;

	for (int ai = 0; ai < s->naggs; ai++)
	{
		const SiftAggAgg *a = &s->agg[ai];

		bool		reads = a->leaf == leaf || bms_is_member(leaf, a->reads);

		if (a->kind == SAA_GEN)
			for (int j = 0; j < a->gen->nargs; j++)
				for (int l = 0; l < a->gen->arg[j].nleaves; l++)
					reads |= a->gen->arg[j].leaf[l] == leaf;
		if (!reads)
			continue;
		if (a->filter != NULL && pred_uses_leaf(s, a->filter, leaf, false))
			return false;
		if (a->wsum == leaf + 1)
			*sums = copied = true;
		else if (a->kind == SAA_COUNT && a->leaf == leaf)
			copied = true;
		else if (a->kind == SAA_GEN && !a->gen->distinct && !a->gen->ordered && !a->gen->sorted && !a->gen->hyp &&
				 (castNode(Aggref, a->gen->aggref)->aggfnoid == F_ARRAY_AGG_ANYNONARRAY || castNode(Aggref, a->gen->aggref)->aggfnoid == F_STRING_AGG_TEXT_TEXT ||
				  castNode(Aggref, a->gen->aggref)->aggfnoid == F_STRING_AGG_BYTEA_BYTEA))
			copied = true;
		else
			return false;
	}
	for (int i = 0; i < s->npred; i++)
		if (pred_uses_leaf(s, s->pred[i], leaf, false))
			return false;
	for (int k = 0; k < s->nkeys; k++)
		if (s->key[k].kind == SAK_COMBO || s->key[k].leaf == leaf)
			copied |= s->key[k].kind != SAK_COMBO;
	for (int ci = 0; ci < s->ncombo; ci++)
		for (int p = 0; p < s->combo[ci].nleaves; p++)
			if (s->combo[ci].leaf[p] == leaf)
			{
				bool		key = false;

				if (s->combo[ci].nleaves > 1 || s->combo[ci].join >= 0)
					return false;
				for (int k = 0; k < s->nkeys; k++)
					key |= s->key[k].kind == SAK_COMBO && s->key[k].leaf == ci;
				for (int ai = 0; ai < s->naggs; ai++)
					if (s->agg[ai].combo == ci && s->agg[ai].kind >= SAA_X_COUNT && s->agg[ai].kind <= SAA_X_AVG)
					{
						if (s->agg[ai].wsum == 0)
							return false;
						*sums = true;
					}
				if (!key)
					return false;
				copied = *keyed = true;
			}
	return copied;
}

/* is every reader of the leaf a sum or average that adds its dictionary entries (SiftAggAgg.wsum)? */
bool
leaf_wsum_only(const SiftAggSpec *s, int leaf)
{
	bool		sums = false,
				other = false;

	for (int ai = 0; ai < s->naggs; ai++)
	{
		const SiftAggAgg *a = &s->agg[ai];

		if (a->wsum == leaf + 1)
			sums = true;
		else
			other |= a->leaf == leaf || bms_is_member(leaf, a->reads);
		if (a->kind == SAA_GEN)
			for (int j = 0; j < a->gen->nargs; j++)
				for (int l = 0; l < a->gen->arg[j].nleaves; l++)
					other |= a->gen->arg[j].leaf[l] == leaf;
		other |= a->filter != NULL && pred_uses_leaf(s, a->filter, leaf, false);
	}
	for (int i = 0; i < s->npred; i++)
		other |= pred_uses_leaf(s, s->pred[i], leaf, false);
	for (int k = 0; k < s->nkeys; k++)
		other |= s->key[k].kind != SAK_COMBO && s->key[k].leaf == leaf;
	for (int ci = 0; ci < s->ncombo; ci++)
		for (int p = 0; p < s->combo[ci].nleaves; p++)
			other |= s->combo[ci].leaf[p] == leaf && !combo_wsum_only(s, ci);
	return sums && !other;
}

/* is the numeric leaf stored as images (no precision, or past 38 digits) rather than as 128-bit lanes? */
static bool
numeric_image(AggCtx *c, const SiftSrcLeaf *lf)
{
	Node	   *e = lf->path == NIL && lf->parent < 0 ? list_nth(c->attexpr, lf->attno - 1) : NULL;
	int32		typmod = e && IsA(e, Var) ? ((Var *) e)->vartypmod : -1;
	int			precision = ((typmod - VARHDRSZ) >> 16) & 0xffff,
				scale = (((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024;

	return lf->wtypid == NUMERICOID && (typmod < 0 || precision > 38 || scale < 0 || scale > 38);
}

/*
 * The dictionary entries a wide leaf builds for each row read, as a share of the rows: a unit has its own dictionary, so a column of 50,000 values
 * read in units of 10,000 rows builds about 9,500 entries a unit, not a fifth of its rows.
 */
static double
wide_entry_frac(AggCtx *c, int leaf, const CostAcc *k)
{
	double		units = c->stats.nunits > 0 ? c->stats.nunits : k->units,
				nd = Max(leaf_distinct_frac(c, leaf, k->rows) * k->rows, 1.0),
				per_unit = k->rows / units;

	return Min(nd * -expm1(-per_unit / nd) * units / Max(k->rows, 1.0), 1.0);
}

/* the operators a row of an image leaf costs a worker for the dictionary it inflates, cold: more for a column of millions of distinct values (AGG_WIDE_BIG) */
static double
wide_cold_ops(AggCtx *c, int leaf, const CostAcc *k)
{
	double		nd = leaf_distinct_frac(c, leaf, k->rows) * k->rows;

	return AGG_WIDE_COLD * pow(Min(nd / 2000000.0, 1.0), 0.13) + Min(AGG_WIDE_BIG_MAX, AGG_WIDE_BIG * Max(nd - 2000000.0, 0.0) / 1000000.0);
}

/* the bytes of an array of the column an image leaf holds, 0 when the leaf is not the image of an array */
static double
array_image_width(AggCtx *c, int leaf)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->wide && lf->parent < 0 && lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
	int			w;

	if (e == NULL || !OidIsValid(get_element_type(exprType(e))))
		return 0;
	w = leaf_heap_width(c, leaf);
	return w > 0 ? w : AGG_ARR_WIDTH;
}

/* is a value of the leaf read by a key, an aggregate or a combo (an image decoded), not only its null flags or a test? */
static bool
leaf_values_used(const SiftAggSpec *s, int leaf)
{
	for (int i = 0; i < s->nkeys; i++)
		if (s->key[i].leaf == leaf && s->key[i].kind != SAK_COMBO)
			return true;
	for (int i = 0; i < s->naggs; i++)
	{
		const SiftAggAgg *a = &s->agg[i];

		if (a->leaf == leaf || bms_is_member(leaf, a->reads) || pred_uses_leaf(s, a->filter, leaf, true))
			return true;
		if (a->kind == SAA_GEN)
			for (int j = 0; j < a->gen->nargs; j++)
				for (int l = 0; l < a->gen->arg[j].nleaves; l++)
					if (a->gen->arg[j].leaf[l] == leaf)
						return true;
	}
	for (int i = 0; i < s->ncombo; i++)
		for (int p = 0; p < s->combo[i].nleaves; p++)
			if (s->combo[i].leaf[p] == leaf)
				return true;
	return false;
}

/* does a conjunct of the query test a value of the leaf (every row's value is read, whatever few pass)? */
static bool
leaf_tested(const SiftAggSpec *s, int leaf)
{
	for (int i = 0; i < s->npred; i++)
		if (pred_uses_leaf(s, s->pred[i], leaf, true))
			return true;
	return false;
}

/* is a value of the leaf read by a key, an aggregate, a combo or a test (an image decoded), not only its null flags? */
static bool
leaf_values_read(const SiftAggSpec *s, int leaf)
{
	return leaf_values_used(s, leaf) || leaf_tested(s, leaf);
}

/* do only comparisons with constants read the leaf: the kernels test its lanes where they are, no value is decoded (0.3 ns a row measured for 3,000,000 rows of a date column) */
static bool
leaf_compared_only(const SiftAggSpec *s, int leaf)
{
	bool		tested = false;

	/* a top-N orders by columns that no key or aggregate names */
	if (s->topn || leaf_values_used(s, leaf))
		return false;
	for (int i = 0; i < s->npred; i++)
		if (pred_uses_leaf(s, s->pred[i], leaf, true))
		{
			if (s->pred[i]->kind != SAP_CMP)
				return false;
			tested = true;
		}
	return tested;
}

/*
 * The bytes of the arrays whose entries a scan decodes and tests: a leaf a filter tests is read whole, one the query only groups or aggregates at the
 * rows that pass (sparse), and none when all it reads of the leaf are its null flags.
 */
static double
array_bytes_read(AggCtx *c, const SiftAggSpec *s, int leaf, double sparse)
{
	double		bytes = array_image_width(c, leaf);

	if (bytes == 0 || !leaf_values_read(s, leaf))
		return 0;
	return bytes * (leaf_tested(s, leaf) ? 1.0 : sparse);
}

/* the entries of it, in operators */
static double
wide_entry_cost(AggCtx *c, const SiftAggSpec *s, int leaf, const CostAcc *k, double sparse)
{
	bool		sums,
				keyed;
	double		array = AGG_ARR_BIG * Max(array_bytes_read(c, s, leaf, sparse) - AGG_ARR_KNEE, 0.0),
				frac = wide_entry_frac(c, leaf, k) * (leaf_tested(s, leaf) ? 1.0 : sparse);	/* a leaf no conjunct tests: the entries of the rows that pass, a unit that keeps few is read at those rows only */

	if (leaf_copy_only(s, leaf, &sums, &keyed))
		return frac * ((sums ? (keyed ? AGG_WIDE_KEYSUM : AGG_WIDE_NUM) : AGG_WIDE_COPY) + array);
	return frac * ((numeric_image(c, &c->lay->leaf[leaf]) ? (leaf_wsum_only(s, leaf) ? AGG_WIDE_NUM : AGG_WIDE_IMAGE) : AGG_WIDE_ENTRY) + array);
}

/* The nanoseconds a row costs the tests of the IN lists of the predicate p beyond a small table in the cache: a list of spread values is a hash or bit table */
static double
in_tests_ns(AggCtx *c, const SiftAggPred *p)
{
	double		ns = 0;

	if (p->kind == SAP_AND || p->kind == SAP_OR)
		for (int i = 0; i < p->nchild; i++)
			ns += in_tests_ns(c, p->child[i]);
	else
		ns = Max(in_list_test_ns(c, p) - AGG_IN_TEST_BASE, 0);
	return ns;
}

/*
 * A key read as dictionary codes gives an id to every entry of the dictionary of each unit read, whether a row that passes the filters holds it
 * or not: a near-unique text column mapped over the units the zones leave costs its rows in entries, and the table of ids outgrows the groups
 * the query has. Returns the entries mapped; *ids: the ids they make (the most of one key); *ops: what mapping them costs.
 */
double
key_entries(AggCtx *c, const SiftAggSpec *s, const CostAcc *k, double *ids, double *ops)
{
	double		units = Max(k->units, 1.0),
				read = Max(k->zone_kept, 1.0 / units) * units,
				total = 0;

	*ids = 0;
	*ops = 0;
	for (int i = 0; i < s->nkeys; i++)
	{
		const SiftSrcLeaf *lf;
		int			leaf = -1;
		double		frac,
					nd,
					entries;

		/* a wide column is a key through the combo of its leaf */
		if (s->key[i].kind == SAK_COMBO && s->combo[s->key[i].leaf].nleaves == 1 && s->combo[s->key[i].leaf].join < 0)
			leaf = s->combo[s->key[i].leaf].leaf[0];
		else if (s->key[i].kind == SAK_LEAF || s->key[i].kind == SAK_LUT)
			leaf = s->key[i].leaf;
		if (leaf < 0)
			continue;
		lf = &c->lay->leaf[leaf];
		/* without statistics the values are not known to be many; the combo of an integer column gives each value an id of the output table, as an entry of a dictionary does */
		if (s->key[i].kind == SAK_COMBO ? !(lf->wide ? lf->vtype == SCV_CODE : lf->vtype == SCV_I32) : (lf->vtype != SCV_CODE || lf->wide))
			continue;
		frac = leaf_stat_frac(c, leaf, k->rows);
		if (frac <= 0)
			continue;
		nd = Max(frac * k->rows, 1.0);
		if (lf->wide)
		{
			/* the units the column has, not a guess of 2,000 rows each */
			double		wunits = c->stats.nunits > 0 ? c->stats.nunits : units;

			entries = nd * -expm1(-k->rows / wunits * k->sparse / nd) * Max(k->zone_kept, 1.0 / wunits) * wunits;
		}
		else
			entries = nd * -expm1(-k->rows / units * k->sparse / nd) * read;
		total += entries;
		if (lf->wide)
			*ops += entries * AGG_KEY_WIDE * pow(Min(entries, nd) / 30000.0, 0.42);
		else
			*ops += entries * AGG_KEY_ENTRY * (pow(Max(Min(entries, nd) / AGG_KEY_IDS, 1.0), 0.35) - 1.0);
		*ids = Max(*ids, Min(entries, nd));
	}
	return total;
}

/* the tests a row goes through: the bounds of a range on one leaf are one pass over its lanes (0.65 ms for 1,000,000 rows with one bound or two, 1.04 with a second column) */
static int
filter_tests(const SiftAggSpec *s)
{
	int			tests = 0;

	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];
		bool		bound = p->kind == SAP_CMP && p->op != SCMP_EQ && p->op != SCMP_NE,
					seen = false;

		for (int j = 0; bound && j < i; j++)
			seen |= s->pred[j]->kind == SAP_CMP && s->pred[j]->leaf == p->leaf && s->pred[j]->op != SCMP_EQ && s->pred[j]->op != SCMP_NE;
		tests += !seen;
	}
	return tests;
}

void
cost_read(AggCtx *c, SiftAggSpec *s, CostAcc *k)
{
	int			nkeyfilters;
	double		sparse,
				tested;

	k->zone_kept = zone_kept(c, s, &nkeyfilters, &k->key_share);
	/*
	 * A unit that few rows pass the filters is read at those rows only (docs/COL-SEARCH.md: a unit with a kept row in eight or fewer is
	 * compacted, its leaves decoded for the kept rows): the leaves the filters test are read whole, the others for the share that passes.
	 */
	sparse = Min(k->sel_rows / (k->rows * Max(k->zone_kept, 1.0 / k->units)), 1.0);
	sparse = sparse <= AGG_SPARSE_MAX ? Min(sparse * AGG_SPARSE_LEAF, 1.0) : 1.0;
	/* the rows the point directory lists are the only ones the filters read and test */
	tested = point_narrows(c, s) ? sparse : 1.0;
	k->sparse = sparse;
	for (int i = 0; i < s->nleaves; i++)
		if (!leaf_compared_only(s, s->leaves[i]))
			k->scan_cost += k->rows * (leaf_level(c, s->leaves[i]) ? elems_of(c, leaf_of(c, s->leaves[i])->parent) : 1.0) * AGG_SCAN_LEAF *
				(leaf_filtered(s, s->leaves[i]) ? tested : sparse);
	k->scan_cost += k->rows * (1 + (s->group >= 0 ? k->elems : 0)) * AGG_FILTER_VALUE * Max(filter_tests(s) + nkeyfilters, 1) * tested;
	k->scan_cost += k->rows * c->elem_work * AGG_FILTER_VALUE;
	for (int i = 0; i < s->npred; i++)
		k->scan_cost += k->rows * in_tests_ns(c, s->pred[i]) / (AGG_OP_NS * AGG_SCAN_SPEED) * tested;
	for (int i = 0; i < s->nvleaf; i++)
		k->scan_cost += sift_aggvleaf_member(&s->vleaf[i]) ? k->rows * elems_of(c, s->vleaf[i].obj) * AGG_VLEAF_ELEM : 0;
	k->scan_cost += k->rows * inline_text_cost(c, s);
	for (int i = 0; i < s->nleaves; i++)
		if (leaf_of(c, s->leaves[i])->wide)
		{
			k->scan_cost += k->rows * (AGG_WIDE_ROW + wide_entry_cost(c, s, s->leaves[i], k, sparse));
			if (numeric_image(c, leaf_of(c, s->leaves[i])))
				k->cold_ops += k->rows * wide_entry_frac(c, s->leaves[i], k) * wide_cold_ops(c, s->leaves[i], k);
		}
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_WIDE && leaf_of(c, s->pred[i]->leaf)->vtype == SCV_CODE)
			k->scan_cost += k->rows * leaf_distinct_frac(c, s->pred[i]->leaf, k->rows) * AGG_WIDE_TEST;
	k->scan_cost = (k->scan_cost + k->units * AGG_UNIT_COST) * Max(k->zone_kept, 1.0 / k->units);
	k->cold_ops *= Max(k->zone_kept, 1.0 / k->units);
}

/*
 * Does the set of a count(DISTINCT) of a leaf stay in the words of a group (agg/dist.c DD_WORDS)? The values of the leaf fall in a window of
 * dist_narrow_words words aligned to 64: from the range of its zones, or, for a text leaf, the codes of its dictionary (0 to nd - 1). A
 * leaf with no range known is not: the executor decides at its first batch, and the plan keeps the bitmap price.
 */
static bool
distinct_narrow(AggCtx *c, const SiftAggAgg *a)
{
	double		nd = c->stats.ndistinct[a->leaf],
				lo = c->stats.min[a->leaf],
				hi = c->stats.max[a->leaf],
				room = 64.0 * sift_limit(dist_narrow_words, DIST_NARROW_WORDS);

	if (lo != 0 || hi != 0)
		return floor(hi / 64.0) * 64.0 + 64.0 - floor(lo / 64.0) * 64.0 <= room;
	return nd > 0 && nd <= room && leaf_of(c, a->leaf)->vtype == SCV_CODE;
}

/*
 * The chunks of the bitmap sets of a count(DISTINCT) of a leaf: a group's values fall in chunks of AGG_DIST_CHUNK_BITS bits of the value
 * range (nd values wide), and a chunk is allocated and zeroed when a value first touches it. Only while the chunks fit the set's memory:
 * past it the sets move to a hash set, whose pair costs between 0.1 and 2 us with the column and the size of the set (not priced: 0).
 * A set of a narrow leaf has none.
 */
double
distinct_chunks(AggCtx *c, SiftAggAgg *a, CostAcc *k)
{
	double		groups = Max(Min(k->rows_out, k->agg_rows), 1.0),
				nd = a->leaf >= 0 ? c->stats.ndistinct[a->leaf] : 0,
				chunks,
				touched;

	if (nd <= 0 || distinct_narrow(c, a))
		return 0;
	chunks = Max(ceil(nd / AGG_DIST_CHUNK_BITS), 1.0);
	/* the integer set of a group that holds few rows starts as a hash set (dist_wide_mode in agg/dist.c): no chunk is allocated */
	if (leaf_of(c, a->leaf)->vtype != SCV_CODE && k->agg_rows / groups < 16.0 * (2.0 + floor((c->stats.max[a->leaf] - c->stats.min[a->leaf]) / AGG_DIST_CHUNK_BITS)))
		return 0;
	touched = Min(groups * chunks * -expm1(-k->agg_rows / groups / chunks), k->agg_rows);
	return touched * AGG_DIST_CHUNK_BYTES > (double) get_hash_memory_limit() ? 0 : touched;
}

/* the distinct values of the text column a count(DISTINCT) reads by the statistics; 0 for any other argument or without them */
double
distinct_text_values(AggCtx *c, const SiftAggAgg *a, const CostAcc *k)
{
	double		frac;

	if (a->kind != SAA_COUNT_DISTINCT || a->leaf < 0 || c->lay->leaf[a->leaf].vtype != SCV_CODE || c->lay->leaf[a->leaf].wide ||
		(frac = leaf_stat_frac(c, a->leaf, k->rows)) <= 0)
		return 0;
	return Max(frac * k->rows, 1.0);
}

/*
 * The ids the dictionary entries of a text column get for its count(DISTINCT): each unit hashes its entries into the table of the ids of the
 * query, which grows with the values the column has (a column of a few thousand values costs nothing; one of 700,000 costs 140 ns a row).
 */
double
distinct_id_ops(AggCtx *c, const SiftAggSpec *s, const CostAcc *k)
{
	double		units = Max(k->units, 1.0),
				read = Max(k->zone_kept, 1.0 / units) * units,
				ops = 0;

	for (int ai = 0; ai < s->naggs; ai++)
	{
		double		nd = distinct_text_values(c, &s->agg[ai], k),
					entries,
					x;
		bool		counted = false;

		/* a leaf the keys or an earlier aggregate gave ids to has them */
		for (int j = 0; j < ai; j++)
			counted |= s->agg[j].kind == SAA_COUNT_DISTINCT && s->agg[j].leaf == s->agg[ai].leaf;
		for (int j = 0; j < s->nkeys; j++)
			counted |= (s->key[j].kind == SAK_LEAF || s->key[j].kind == SAK_LUT) && s->key[j].leaf == s->agg[ai].leaf;
		if (nd <= 0 || counted)
			continue;
		entries = nd * -expm1(-k->rows / units / nd) * read;
		x = Min(entries, nd) / 100000.0;
		ops += entries * AGG_DIST_ID * pow(x, x <= 1.0 ? 0.9 : 0.6);
	}
	return ops;
}
