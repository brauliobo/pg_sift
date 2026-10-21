/*
 * agg/plan_cost.c - the cost model of the scan and its constants
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/pathnode.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/topn.h"
#include "agg/spec.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/plan_int.h"

/* ---------- cost ---------- */

/*
 * The executor's cost per value in units of cpu_operator_cost (about 3 ns: docs/COL-PLAN.md, "Cost", fitted on whole table runs of
 * one to three keys and aggregates), and the elements per row of an array whose statistics are missing.
 */
#define AGG_SCAN_LEAF		0.7		/* a leaf read (decode, null flags) */
#define AGG_FILTER_VALUE	0.5		/* a predicate on a value */
#define AGG_TUPLE			2.5		/* a tuple that passes the filters: selection, ordinals, group ids */
#define AGG_AGG_ROW			1.2		/* an aggregate update */
#define AGG_KEY_ROW			1.0		/* a group key id */
#define AGG_COMBO_ROW		2.5		/* a tuple's entry in a combo: gathers and a hash probe */
#define AGG_SEMI_ROW		1.0		/* a tuple tested against a semi or anti join: one byte of the lookup table, or a hash probe */
#define AGG_ORDERED_ROW		6.0		/* a value kept for an ordered-set aggregate (histogram probe) */
#define AGG_NUM_STEP		0.5		/* an int128 add, subtract, multiply on a value */
#define AGG_NUM_DIV			8.0		/* round, trunc, floor, ceil: an int128 division */
#define AGG_VLEAF_ELEM		9.0		/* an element of an object looked up by key (a metric of a free key object), per row and key */
#define AGG_EVAL_SLOT		60.0	/* PostgreSQL evaluating an expression for a value combination it has not seen: the datums, a few operators, the id or the copy of the result */
#define AGG_WIDE_LEAF		3.0		/* a 64-bit leaf of a combo, per tuple: the hash probe for the id of its value */
#define AGG_DIRECT_NUM		2.0		/* the transition of a PostgreSQL-evaluated value (numeric_add): the normal plan pays it per row too, at the price PostgreSQL gives a transition function */
#define AGG_DIRECT_CMP		1.0		/* the type's comparison of one (min, max): the same in both plans */
#define AGG_REAL_CALL		5.0		/* what the executor really spends on a function or operator node (the fmgr call and its arguments), in operators */
#define AGG_REAL_NUMERIC	40.0	/* the same for a numeric function: the allocation and the digit arithmetic */
#define AGG_PLAIN_CREDIT	0.5		/* the share of the normal plan's under-priced evaluation the scan is credited with */
#define AGG_BYTE			0.1		/* a stored byte read and decompressed: about 0.3 ns, 3 GB/s */
#define AGG_UNIT_COST		200.0
#define AGG_PREAGG_GROUP	25.0	/* a group of a unit's histogram folded into the table: its key id and the aggregates (about 80 ns) */
#define AGG_START_COST		140000.0	/* opening the source, the lookup tables and the first units: about 0.4 ms, which a selective btree plan does not pay */
#define AGG_MERGE_GROUP		8.0		/* a group of a partial table of a worker: dumped, sent, merged */
#define AGG_ORDERED_MERGE	1.5		/* a value kept for an ordered-set aggregate, as the leader merges the histograms of the workers (the distinct pairs, at most one per value) */
#define AGG_SETUP_FACTOR	110.0	/* times parallel_setup_cost: starting the workers and opening the source in each takes about 8 ms, which in this path's estimates (a unit is worth about 0.1 us there, the unnest excess below included) is 110 times the default 1000 */
#define AGG_DEFAULT_ELEMS	3.0
#define AGG_RECHECK_FRAC	0.02	/* share of the rows the source reads again from the heap when a leaf the scan rechecks is flagged (the index does not count them) */
#define AGG_RECHECK_ROW		60.0	/* a row read again: the heap fetch by TID, the document walked again and the unit it makes */
#define AGG_WIDE_ROW		3.0		/* a row of a wide leaf: its lanes decoded, the hash probe for its entry in the unit's dictionary */
#define AGG_WIDE_ENTRY		12.0	/* a new entry of it (a numeric built, about 100 ns), per distinct value */
#define AGG_WIDE_TEST		1.0		/* a conjunct on a wide leaf of images, per dictionary entry: the engine makes its LUT once per segment (about 3 ns an entry) */
#define AGG_INLINE_WIDTH	200		/* average bytes of a text column from which its units store the strings themselves (docs/COL.md 2.9) */
#define AGG_INLINE_ROW		56.0	/* a row of such a leaf read: the unit's strings are interned into a dictionary of its own */
#define AGG_INLINE_TEST		3.0		/* a row of it tested on the encoded strings (the prefixes decide most rows) */

double
elems_of(AggCtx *c, int grp)
{
	return c->stats.elems_per_row[grp] > 0 ? c->stats.elems_per_row[grp] : AGG_DEFAULT_ELEMS;
}

static double
num_cost(const SiftAggNum *o)
{
	double		cost = 0;

	for (int k = 0; k < o->n; k++)
		cost += o->step[k].op >= SNX_ROUND ? AGG_NUM_DIV : o->step[k].op > SNX_CONST ? AGG_NUM_STEP : 0;
	return cost;
}

/* the numeric programs of a predicate tree, per row that reaches them */
static double
pred_num_cost(const SiftAggPred *p)
{
	double		cost = p == NULL ? 0 : num_cost(&p->num);

	for (int i = 0; p && i < p->nchild; i++)
		cost += pred_num_cost(p->child[i]);
	return cost;
}

/* the numeric operations of a program, and of the predicate tree (one more for the comparison of each): PostgreSQL's numeric functions in the normal plan */
static int
num_ops(const SiftAggNum *o)
{
	int			n = 0;

	for (int k = 0; k < o->n; k++)
		n += o->step[k].op > SNX_CONST;
	return n;
}

static int
pred_num_ops(const SiftAggPred *p)
{
	int			n = p == NULL ? 0 : p->num.n > 0 ? num_ops(&p->num) + 1 : 0;

	for (int i = 0; p && i < p->nchild; i++)
		n += pred_num_ops(p->child[i]);
	return n;
}

/*
 * The distinct values of a column under the table's own conditions on it: the values of the table (PostgreSQL's statistics) times the
 * share the conditions keep (a range of dates keeps the dates of the range). 0 when there are no statistics.
 */
static double
column_distinct(AggCtx *c, Var *v)
{
	List	   *clauses = NIL;
	HeapTuple	tup;
	double		nd;

	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid), Int16GetDatum(v->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tup))
		return 0.0;
	nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
	ReleaseSysCache(tup);
	nd = nd > 0 ? nd : -nd * c->rel->tuples;
	foreach_node(RestrictInfo, ri, c->rel->baserestrictinfo)
	{
		Bitmapset  *atts = NULL;

		pull_varattnos((Node *) ri->clause, c->relid, &atts);
		if (bms_membership(atts) == BMS_SINGLETON && bms_is_member(v->varattno - FirstLowInvalidHeapAttributeNumber, atts))
			clauses = lappend(clauses, ri);
	}
	return nd * (clauses == NIL ? 1.0 : clauselist_selectivity(c->root, clauses, 0, JOIN_INNER, NULL));
}

/*
 * distinct value combinations of the inputs of a combo among `tuples`: what the statistics say of the columns, the tuples when an input is
 * not one; a column of an earlier lookup has at most as many values as the lookup has rows.
 */
static double
combo_entries(AggCtx *c, const SiftAggCombo *cb, double tuples)
{
	List	   *exprs = NIL;
	double		entries = 1.0;

	for (int p = 0; p < cb->nleaves; p++)
	{
		SiftSrcLeaf *lf;
		Node	   *e;
		double		nd;

		if (cb->leaf[p] < 0)
		{
			foreach_ptr(JoinCtx, jc, c->joins)
				if (jc->combo == cb->refcombo[p])
					entries *= Max(c->root->simple_rel_array[jc->rti]->rows, 1.0);
			continue;
		}
		lf = leaf_of(c, cb->leaf[p]);
		/* the values of a member or an element have no statistics: PostgreSQL's default number of distinct values, as for a column it knows nothing of */
		if (lf->parent >= 0 || lf->path != NIL)
		{
			entries *= DEFAULT_NUM_DISTINCT;
			continue;
		}
		if (lf->attno > list_length(c->attexpr) || (e = list_nth(c->attexpr, lf->attno - 1)) == NULL)
			return tuples;
		if (IsA(e, Var) && (nd = column_distinct(c, (Var *) e)) > 0)
			entries *= nd;
		else
			exprs = lappend(exprs, e);
	}
	if (exprs != NIL)
		entries *= estimate_num_groups(c->root, exprs, tuples, NULL, NULL);
	return Max(Min(entries, tuples), 1.0);
}

/*
 * The rows of the path of a joined plan. PostgreSQL assumes 1000 rows a call of a set-returning function in the select list (a date
 * spine), a ceiling nobody generates up to: the rows a call makes are taken as the geometric mean of one and that estimate.
 */
static double
child_rows(Path *p)
{
	for (;;)
	{
		if (IsA(p, SubqueryScanPath))
			p = ((SubqueryScanPath *) p)->subpath;
		else if (IsA(p, ProjectionPath))
			p = ((ProjectionPath *) p)->subpath;
		else if (IsA(p, ProjectSetPath))
		{
			Path	   *sub = ((ProjectSetPath *) p)->subpath;

			return child_rows(sub) * sqrt(Max(p->rows / Max(sub->rows, 1.0), 1.0));
		}
		else
			return p->rows;
	}
}

/*
 * The evaluation PostgreSQL does for the combos with a 64-bit leaf or a direct slot: a hash probe per tuple for the id of each wide
 * leaf, and the expressions once per distinct combination of the leaves (every tuple when the column has as many values). Combos of
 * dictionary and small integer leaves have few combinations and keep the price they had.
 */
static double
combo_eval_cost(AggCtx *c, const SiftAggSpec *s, double rows, double agg_rows, double scan_rows)
{
	double		cost = 0;

	for (int ci = 0; ci < s->ncombo; ci++)
	{
		const SiftAggCombo *cb = &s->combo[ci];
		int			wide = 0;
		bool		direct = false,
					filter = false;
		double		tuples;

		if (cb->join >= 0)
		{
			const SiftAggJoin *j = &s->join[cb->join];
			JoinCtx    *jc = list_nth(c->joins, cb->join);

			/* Without equality keys, every cached input tests every child row. */
			if (j->nkey == 0 && j->params == NIL)
			{
				double		candidates = jc->cpath ? child_rows(jc->cpath) : c->root->simple_rel_array[jc->rti]->tuples;

				cost += combo_entries(c, cb, scan_rows) * Max(candidates, 1.0) * AGG_EVAL_SLOT;
			}
			continue;
		}
		for (int p = 0; p < cb->nleaves; p++)
			if (cb->leaf[p] >= 0)
				wide += leaf_of(c, cb->leaf[p])->vtype == SCV_I64 || leaf_of(c, cb->leaf[p])->vtype == SCV_F64;
		for (int sl = 0; sl < cb->nslots; sl++)
		{
			direct |= cb->slot[sl].direct;
			filter |= !cb->slot[sl].is_key && !cb->slot[sl].direct;
		}
		if (wide == 0 && !direct)
			continue;
		tuples = filter ? rows : agg_rows;
		cost += tuples * wide * AGG_WIDE_LEAF + combo_entries(c, cb, tuples) * cb->nslots * AGG_EVAL_SLOT;
	}
	return cost;
}

/* a tuple of a bucket key: the number of its step, the lookup of the id */
#define AGG_BUCKET_ROW		3.0

/* the buckets of a stretch of the leaf of a key, by the statistics: the tuples when there are none */
static double
bucket_count(AggCtx *c, const SiftAggKey *k, double tuples)
{
	int64		lo,
				hi;

	return leaf_stats_range(c, k->leaf, &lo, &hi) ? Min(tuples, (double) (hi - lo) / USECS_PER_SEC / k->bucket + 1) : tuples;
}

/* a lookup per tuple of a bucket key, and PostgreSQL evaluating the expression once per bucket */
static double
bucket_eval_cost(AggCtx *c, const SiftAggSpec *s, double rows)
{
	double		cost = 0;

	for (int i = 0; i < s->nkeys; i++)
		if (s->key[i].bucket > 0)
			cost += rows * AGG_BUCKET_ROW + bucket_count(c, &s->key[i], rows) * AGG_EVAL_SLOT;
	return cost;
}

/*
 * What the executor really spends on an expression, in operators (PostgreSQL prices a function at procost, 1 for almost all, while a
 * numeric one allocates and does digit arithmetic).
 */
static bool
real_cost_walker(Node *n, double *excess)
{
	List	   *args = NIL;

	if (n == NULL)
		return false;
	if (IsA(n, FuncExpr))
		args = ((FuncExpr *) n)->args;
	else if (IsA(n, OpExpr) || IsA(n, DistinctExpr))
		args = ((OpExpr *) n)->args;
	if (IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, DistinctExpr))
	{
		double		real = exprType(n) == NUMERICOID ? AGG_REAL_NUMERIC : AGG_REAL_CALL;

		foreach_ptr(Node, a, args)
			if (exprType(a) == NUMERICOID)
				real = AGG_REAL_NUMERIC;
		*excess += real;
	}
	else if (IsA(n, CoerceViaIO))
		*excess += AGG_REAL_NUMERIC;
	return expression_tree_walker(n, real_cost_walker, excess);
}

/*
 * The credit of the expressions and numeric aggregates: both plans evaluate the expressions of a query, the normal plan once per row at the price PostgreSQL gives
 * them, which is the executor's cost of a call for the cheap ones and an order of magnitude less than the real one for a numeric one;
 * the scan evaluates once per distinct combination of values and is priced with the real cost of its machinery (AGG_EVAL_SLOT). The
 * rows the normal plan evaluates the expressions for, times what its price leaves out, are what the scan saves: AGG_PLAIN_CREDIT of it.
 */
static double
plain_credit(AggCtx *c, const SiftAggSpec *s, double agg_rows)
{
	double		excess = 0;

	for (int ci = 0; ci < s->ncombo; ci++)
		if (s->combo[ci].join < 0)
			for (int sl = 0; sl < s->combo[ci].nslots; sl++)
			{
				Node	   *e = (Node *) s->combo[ci].slot[sl].expr;
				QualCost	priced;
				double		real = 0;

				real_cost_walker(e, &real);
				cost_qual_eval_node(&priced, e, c->root);
				excess += Max(real - priced.per_tuple / cpu_operator_cost, 0.0);
			}
	{
		int			ops = 0;

		for (int i = 0; i < s->npred; i++)
			ops += pred_num_ops(s->pred[i]);
		for (int i = 0; i < s->nkeys; i++)
			ops += num_ops(&s->key[i].num);
		for (int i = 0; i < s->naggs; i++)
			ops += num_ops(&s->agg[i].num) + pred_num_ops(s->agg[i].filter);
		excess += ops * (AGG_REAL_NUMERIC - 2.0);
	}
	/* the transition of a numeric sum, average or extreme: PostgreSQL prices it as one operator, the executor allocates and adds digits */
	for (int ai = 0; ai < s->naggs; ai++)
	{
		const SiftAggAgg *a = &s->agg[ai];

		if (a->restype == NUMERICOID && (a->kind == SAA_SUM_NUM || a->kind == SAA_AVG_NUM || a->kind == SAA_MIN || a->kind == SAA_MAX ||
										 a->kind == SAA_X_SUM || a->kind == SAA_X_AVG || a->kind == SAA_X_MIN || a->kind == SAA_X_MAX))
			excess += AGG_REAL_NUMERIC - 2.0;
	}
	return excess * agg_rows * AGG_PLAIN_CREDIT;
}

/* the per tuple cost of the arithmetic of a PostgreSQL-evaluated aggregate */
static double
direct_agg_cost(const SiftAggAgg *a)
{
	switch (a->kind)
	{
		case SAA_X_MIN:
		case SAA_X_MAX:
			return AGG_DIRECT_CMP;
		case SAA_X_SUM:
		case SAA_X_AVG:
			return a->restype == NUMERICOID ? AGG_DIRECT_NUM : 0;
		default:
			return 0;
	}
}

/* what PostgreSQL divides the cost of a parallel scan by: the workers, and the share of the leader that scans too */
static double
parallel_divisor(int workers)
{
	double		d = workers;

	if (parallel_leader_participation)
		d += Max(1.0 - 0.3 * workers, 0.0);
	return d;
}

/* The correlation of the heap column a leaf is read from with the heap order (0: unknown, or the leaf is a jsonb path or an expression) */
static double
leaf_correlation(AggCtx *c, int leaf)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
	HeapTuple	tp;
	AttStatsSlot sslot;
	double		corr = 0.0;

	if (e == NULL || !IsA(e, Var))
		return 0.0;
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid),
						 Int16GetDatum(((Var *) e)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tp))
		return 0.0;
	if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		corr = sslot.numbers[0];
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	return corr;
}

/*
 * The share of the units a zone test leaves: a comparison keeps the fraction f of the rows (its selectivity, which for a
 * comparison is also the fraction of the leaf's range); the units are consecutive in the heap, so their zones are narrow in the
 * share the column is correlated with the heap order (corr^2) and as wide as the range otherwise: kept = f + (1 - f) * (1 - corr^2).
 * The most selective comparison counts, and the scan reads at least one unit (docs/COL-PLAN.md, "Zone pruning").
 */
static double
zone_kept(AggCtx *c, SiftAggSpec *s)
{
	double		kept = 1.0;

	foreach_ptr(SiftAggPred, p, sift_aggspec_zone_cmps(s, c->lay, c->lay->nleaves))
	{
		double		f = Min(Max(p->sel, 0.0), 1.0),
					corr = leaf_correlation(c, p->leaf);

		kept = Min(kept, f + (1.0 - f) * (1.0 - corr * corr));
	}
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_TID)
			kept = Min(kept, s->pred[i]->sel);
	return kept;
}

static bool
pred_uses_leaf(const SiftAggPred *p, int leaf)
{
	if (p == NULL)
		return false;
	if (p->kind != SAP_AND && p->kind != SAP_OR && (p->leaf == leaf || p->leaf2 == leaf))
		return true;
	for (int i = 0; i < p->nchild; i++)
		if (pred_uses_leaf(p->child[i], leaf))
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
		if (s->agg[i].leaf == leaf || pred_uses_leaf(s->agg[i].filter, leaf))
			return true;
	for (int i = 0; i < s->npred; i++)
		if (pred_uses_leaf(s->pred[i], leaf) && !(s->pred[i]->leaf == leaf && sift_aggspec_str_test(s->pred[i], c->lay, c->lay->nleaves - c->nvleaf, &t)))
			return true;
	return false;
}

/* The average width of the heap column a plain text leaf is read from (0: unknown, or the leaf is a jsonb path or an expression) */
static int
leaf_heap_width(AggCtx *c, int leaf)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;

	return e && IsA(e, Var) ? get_attavgwidth(planner_rt_fetch(c->relid, c->root)->relid, ((Var *) e)->varattno) : 0;
}

/* the share of the rows of a leaf that PostgreSQL's statistics expect to hold a distinct value (1: no statistics) */
static double
leaf_distinct_frac(AggCtx *c, int leaf, double rows)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
	HeapTuple	tup;
	double		nd = 0;

	if (e == NULL || !IsA(e, Var))
		return 1.0;
	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid),
						  Int16GetDatum(((Var *) e)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tup))
		return 1.0;
	nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
	ReleaseSysCache(tup);
	return nd > 0 ? Min(nd / Max(rows, 1.0), 1.0) : nd < 0 ? Min(-nd, 1.0) : 1.0;
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

/* The share of the units the pre-aggregates answer (a unit is exact when its pages are all-visible, its superblock plain); 0 when the spec cannot use them */
double
preagg_share(AggCtx *c, SiftAggSpec *s)
{
	SiftAggPaShape shape;

	if (!sift_agg_preagg || !sift_aggspec_preagg(s, c->lay, c->lay->nleaves, &c->pa, &shape))
		return 0.0;
	return Min(Max(c->stats.exact_frac, 0.0), 1.0);
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
static double
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

bool
extern_param_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param) && ((Param *) node)->paramkind == PARAM_EXTERN)
		return true;
	return expression_tree_walker(node, extern_param_walker, context);
}

/* Does a qual of the table compare with a bound parameter? Its selectivity is a default guess of the planner, not the data's. */
static bool
rel_has_param_qual(RelOptInfo *rel)
{
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		if (extern_param_walker((Node *) ri->clause, NULL))
			return true;
	return false;
}

/* does a btree index of the joined table start with columns that the table's `column = value` conditions give (map_load picks the same) */
static bool
join_index_usable(RelOptInfo *rel, const SiftAggJoin *j)
{
	foreach_node(IndexOptInfo, ii, rel->indexlist)
	{
		if (ii->relam != BTREE_AM_OID || ii->indpred != NIL || ii->hypothetical || ii->indexkeys[0] <= 0)
			continue;
		for (int k = 0; k < j->nidx; k++)
			if (j->idxatt[k] == ii->indexkeys[0] && op_in_opfamily(j->idxop[k], ii->opfamily[0]) && ii->indexcollations[0] == j->idxcoll[k])
				return true;
	}
	return false;
}

/* the heap pages of the scanned table: those of the partitions of a partitioned table */
double
rel_pages(AggCtx *c)
{
	return c->nparts > 0 ? c->part_pages : c->rel->pages;
}

void
agg_cost(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, Path *p, double rows_out)
{
	double		rows = Max(c->stats.nrows, Max(c->rel->tuples, 1.0)),
				elems = 1.0,
				sel_rows = Min(Max(c->rel->rows, 1.0), rows),
				units = Max(c->stats.nunits, ceil(rows / 2000.0)),
				vis = 1.0 - Min(Max(c->stats.allvisible_frac, 0.0), 1.0),
				scan_cost,
				open_cost,
				gen_cost = 0,
				exact,
				agg_rows,
				scan_rows;
	Cost		startup,
				child_cost = 0;

	if (s->group >= 0)
		elems = elems_of(c, s->group);
	open_cost = c->search ? sift_search_open_cost(c->search, rows) : 0;
	scan_cost = c->search ? sift_search_cost(c->search, rows, units, &rows, &units) : 0;
	for (int i = 0; i < s->nleaves; i++)
		scan_cost += rows * (leaf_level(c, s->leaves[i]) ? elems_of(c, leaf_of(c, s->leaves[i])->parent) : 1.0) * AGG_SCAN_LEAF;
	scan_cost += rows * (1 + (s->group >= 0 ? elems : 0)) * AGG_FILTER_VALUE * Max(s->npred, 1);
	scan_cost += rows * c->elem_work * AGG_FILTER_VALUE;
	for (int i = 0; i < s->nvleaf; i++)
		scan_cost += sift_aggvleaf_member(&s->vleaf[i]) ? rows * elems_of(c, s->vleaf[i].obj) * AGG_VLEAF_ELEM : 0;
	scan_cost += rows * inline_text_cost(c, s);
	for (int i = 0; i < s->nleaves; i++)
		if (s->recheck[i] != SRK_NONE && c->stats.ovf[s->leaves[i]])
		{
			scan_cost += rows * AGG_RECHECK_FRAC * AGG_RECHECK_ROW;
			break;
		}
	for (int i = 0; i < s->nleaves; i++)
		if (leaf_of(c, s->leaves[i])->wide)
			scan_cost += rows * (AGG_WIDE_ROW + AGG_WIDE_ENTRY * leaf_distinct_frac(c, s->leaves[i], rows));
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_WIDE && leaf_of(c, s->pred[i]->leaf)->vtype == SCV_CODE)
			scan_cost += rows * leaf_distinct_frac(c, s->pred[i]->leaf, rows) * AGG_WIDE_TEST;
	scan_cost = (scan_cost + units * AGG_UNIT_COST) * Max(zone_kept(c, s), 1.0 / units);
	agg_rows = sel_rows * (s->group >= 0 ? elems : 1.0);
	/* the lookup joins filter before the groups: no more tuples reach them than the rows PostgreSQL expects of the join */
	if (c->joins != NIL)
		agg_rows = Min(agg_rows, Max(input_rel->rows, 1.0));
	scan_rows = agg_rows;
	/* the rows of a preserved side no tuple matches are tuples too */
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind))
			agg_rows += Max(c->root->simple_rel_array[jc->rti]->tuples, 1.0);
	/* a group takes a tuple: the groups PostgreSQL expects of the unnested rows it assumes (a cap of 100 a call) are more than the elements hold */
	rows_out = Min(rows_out, Max(Max(s->nsets, 1) * agg_rows, 1.0));
	scan_cost += agg_rows * (Max(s->nsets, 1) * (AGG_TUPLE + s->nkeys * AGG_KEY_ROW + s->naggs * AGG_AGG_ROW) + s->ncombo * AGG_COMBO_ROW);
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		RelOptInfo *jrel = c->root->simple_rel_array[jc->rti];
		double		tuples = Max(jrel->tuples, 1.0);

		/* Correlated children run once per distinct input, with their native path's cost. */
		if (jc->j.params != NIL)
		{
			const SiftAggCombo *cb = &s->combo[jc->combo];

			child_cost += combo_entries(c, cb, agg_rows) * (jc->cpath->total_cost + cpu_operator_cost * AGG_EVAL_SLOT);
			continue;
		}
		/* a plan is run whole; an index scan reads the rows that pass the equalities, the rest of a table is read whole */
		if (jc->cpath != NULL)
		{
			scan_cost += Max(jc->cpath->rows, 1.0) * AGG_JOIN_ROW;
			child_cost += jc->cpath->total_cost;
		}
		else if (jc->j.nidx > 0 && join_index_usable(jrel, &jc->j))
			scan_cost += Min(Max(jrel->rows, 1.0), tuples) * AGG_IDX_ROW;
		else
			scan_cost += tuples * AGG_JOIN_ROW;
		if (jc->kind == SJ_SEMI || jc->kind == SJ_ANTI)
			scan_cost += sel_rows * AGG_SEMI_ROW;
	}
	for (int i = 0; i < s->naggs; i++)
	{
		if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
			scan_cost += agg_rows * AGG_ORDERED_ROW;
		scan_cost += agg_rows * (num_cost(&s->agg[i].num) + direct_agg_cost(&s->agg[i])) + rows * pred_num_cost(s->agg[i].filter);
		if (s->agg[i].kind == SAA_GEN)
			gen_cost += agg_rows * s->agg[i].gen->cost + rows_out * AGG_GEN_GROUP;
	}
	scan_cost += combo_eval_cost(c, s, rows, agg_rows, scan_rows) + bucket_eval_cost(c, s, agg_rows);
	for (int i = 0; i < s->nkeys; i++)
		scan_cost += agg_rows * num_cost(&s->key[i].num);
	for (int i = 0; i < s->npred; i++)
		scan_cost += rows * pred_num_cost(s->pred[i]);
	scan_cost = Max(scan_cost - plain_credit(c, s, agg_rows), scan_cost * 0.1);
	scan_cost += scan_bytes(c, s, rows) * AGG_BYTE * Max(zone_kept(c, s), 1.0 / units);
	/* a top-N reads a fraction of the units, but opens the match set whole */
	if (s->topn)
		scan_cost = (scan_cost - open_cost) * c->topn_frac + open_cost;
	scan_cost *= cpu_operator_cost;
	/*
	 * The normal plan of an unnest expects the rows of the function (100 per call for jsonb_to_recordset, jsonb_array_elements,
	 * jsonb_each) minus its filters, and prices its sort, join and aggregate with them, though the data has a handful per row: when it
	 * expects more unnested rows than the selected orders hold elements, this path is charged the square root of the excess. The
	 * estimates of the two plans then cross where the times do: a selective store set keeps the btree plan, a whole table or a
	 * filter on the elements (which cuts the expected rows) keeps its price. A qual with a bound parameter has no such estimate (a generic
	 * plan guesses its selectivity; the values are known at execution): no charge.
	 */
	if (s->group >= 0 && !rel_has_param_qual(c->rel))
		scan_cost *= Max(sqrt(input_rel->rows / (sel_rows * elems)), 1.0);
	/* the calls of the generic aggregates are the elements the data has, whatever the normal plan expects of the unnest */
	scan_cost += gen_cost * cpu_operator_cost;
	/* rows on pages that are not all-visible are checked in the heap, a page at a time */
	scan_cost += vis * Min(sel_rows, Max(rel_pages(c), 1.0)) * random_page_cost * 0.25 * (s->topn ? c->topn_frac : 1.0);
	/* the exact units cost their groups, not their rows */
	exact = preagg_share(c, s);
	if (exact > 0)
		scan_cost += exact * (units * (AGG_UNIT_COST + Min(rows_out, 256.0) * AGG_PREAGG_GROUP) * cpu_operator_cost - scan_cost);
	startup = AGG_START_COST * cpu_operator_cost + child_cost + c->subplan_cost;
	if (s->nworkers > 0)
	{
		/* each process aggregates its share into a table of its own (one per lane for float sums); the leader merges them */
		double		tables = sift_aggspec_ordered(s) ? SIFT_AGG_LANES : s->nworkers + 1;

		scan_cost = scan_cost / parallel_divisor(s->nworkers) +
			tables * Min(rows_out, Max(agg_rows / tables, 1.0)) * AGG_MERGE_GROUP * cpu_operator_cost;
		for (int i = 0; i < s->naggs; i++)
			if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
				scan_cost += agg_rows * AGG_ORDERED_MERGE * cpu_operator_cost;
		startup += parallel_setup_cost * AGG_SETUP_FACTOR;
	}
	p->startup_cost = startup + scan_cost;
	p->total_cost = p->startup_cost + rows_out * cpu_tuple_cost;
	/* the rows of a top-N are fetched from the heap, one page each */
	if (s->topn)
		p->total_cost += rows_out * random_page_cost * 0.5;
	p->rows = rows_out;
}
