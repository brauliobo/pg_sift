/*
 * agg/plan.c - planner side of SiftAgg: proves that a single-table aggregate query (optionally unnesting one array or
 * object of the column source) is answered by the column source with the plain plan's result, and adds a
 * Custom Scan (SiftAgg) path for it in create_upper_paths_hook (UPPERREL_GROUP_AGG). Everything not proven keeps the
 * normal plan. Execution is in agg/agg.c; the plan travels as a SiftAggSpec (agg/spec.h).
 *
 * Matching works on the query tree (root->parse), not on the paths: the FROM list must be the table (and at most one lateral
 * set-returning function over one of its jsonb leaves), the quals are compiled to bitmap programs over leaves, and the group
 * keys and aggregates to accumulators. A leaf reference is an expression that equals what the column source stored: an
 * attribute or index expression, a jsonb path of it with the declared type (`(x->>'k')::float8`, `x #>> '{a,b}'`), a field
 * of the unnested function's rows. Any boolean or scalar expression of ONE leaf that is a dictionary (text) or small integer
 * leaf is evaluated by the executor with PostgreSQL's own expression evaluator on every distinct value of the leaf, so its
 * semantics (collations, NULL handling, casts, CASE) are PostgreSQL's by construction.
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

/*
 * The leaf of the index that a leaf after the source's takes its values from: the value column of a member of an object, the array whose length it
 * is; -1 for the type of a path (the tags of its leaf say it, whatever the leaf holds). Whatever that leaf cannot store is read as NULL.
 */
static int
vleaf_source(AggCtx *c, int j)
{
	const SiftAggVLeaf *v = &c->vleaf[j];

	if (v->count)
		return v->obj;
	return sift_aggvleaf_member(v) ? child_leaf(c, v->obj, "value", leaf_of(c, c->lay->nleaves - c->nvleaf + j)->typid) : -1;
}

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
#define AGG_JOIN_ROW		4.0		/* a row of a joined table, read when the scan starts */
#define AGG_IDX_ROW			12.0	/* a row of the joined table read through an index: the btree descent and the heap fetch */
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
#define AGG_INLINE_WIDTH	200		/* average bytes of a text column from which its units store the strings themselves */
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

/* distinct value combinations of the leaves of a combo among `tuples`: what the statistics say of the columns, the tuples when a leaf is not one */
static double
combo_entries(AggCtx *c, const SiftAggCombo *cb, double tuples)
{
	List	   *exprs = NIL;

	for (int p = 0; p < cb->nleaves; p++)
	{
		SiftSrcLeaf *lf;

		if (cb->leaf[p] < 0)
			return tuples;
		lf = leaf_of(c, cb->leaf[p]);
		if (lf->parent >= 0 || lf->path != NIL || lf->attno > list_length(c->attexpr) || list_nth(c->attexpr, lf->attno - 1) == NULL)
			return tuples;
		exprs = lappend(exprs, list_nth(c->attexpr, lf->attno - 1));
	}
	return Min(tuples, estimate_num_groups(c->root, exprs, tuples, NULL, NULL));
}

/*
 * The evaluation PostgreSQL does for the combos with a 64-bit leaf or a direct slot: a hash probe per tuple for the id of each wide
 * leaf, and the expressions once per distinct combination of the leaves (every tuple when the column has as many values). Combos of
 * dictionary and small integer leaves have few combinations and keep the price they had.
 */
static double
combo_eval_cost(AggCtx *c, const SiftAggSpec *s, double rows, double agg_rows)
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
				double		candidates = jc->cpath ? jc->cpath->rows : c->root->simple_rel_array[jc->rti]->tuples;

				cost += combo_entries(c, cb, agg_rows) * Max(candidates, 1.0) * AGG_EVAL_SLOT;
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
				agg_rows;
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
	scan_cost += combo_eval_cost(c, s, rows, agg_rows) + bucket_eval_cost(c, s, agg_rows);
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

/* ---------- the path ---------- */

bool
unsupported_clause(AggCtx *c, Node *n)
{
	return n != NULL && (contain_volatile_functions(n) || has_exec_param_walker(n, c));
}

/* a path whose node outputs the columns of its choice (the others need a projection above them) */
static bool
projects_columns(Path *p)
{
	return IsA(p, SubqueryScanPath) || p->pathtype == T_ValuesScan || p->pathtype == T_FunctionScan || p->pathtype == T_CteScan;
}

/* the cheapest path of the rows of a sub-select, VALUES list, function or UNION ALL of sub-selects */
static Path *
child_path(RelOptInfo *rel, bool correlated)
{
	Path	   *best = NULL;

	foreach_ptr(Path, p, rel->pathlist)
		if ((p->param_info == NULL || (correlated && IsA(p, SubqueryScanPath))) && (projects_columns(p) || IsA(p, AppendPath) || IsA(p, MergeAppendPath)) &&
			(best == NULL || p->total_cost < best->total_cost))
			best = p;
	return best;
}

/* the calls (function or operator) on one column of the enclosing query that never raise: the child takes the call's value instead of the column */
static bool
derivable_call(Node *n)
{
	Oid			fid = func_of(n);

	return list_length(func_args(n)) >= 1 && !(IsA(n, FuncExpr) && ((FuncExpr *) n)->funcretset) &&
		(fid == F_JSONB_EXISTS || fid == F_JSONB_OBJECT_FIELD_TEXT || fid == F_JSONB_TYPEOF);
}

typedef struct DeriveCtx
{
	int			depth;			/* the query levels below the child's own */
	List	   *calls;			/* the calls, as the enclosing query evaluates them */
	List	   *params;			/* the parameter that stands for each */
	PlannerInfo *root;
} DeriveCtx;

/* A call of an enclosing column with constants becomes a parameter whose value the enclosing query computes (once per distinct column value). */
static Node *
derive_mutator(Node *n, DeriveCtx *x)
{
	Node	   *a;

	if (n == NULL)
		return NULL;
	if (IsA(n, Query))
	{
		Query	   *q;

		x->depth++;
		q = query_tree_mutator((Query *) n, derive_mutator, x, 0);
		x->depth--;
		return (Node *) q;
	}
	if (derivable_call(n) && IsA(a = strip_relabel(linitial(func_args(n))), Var) && ((Var *) a)->varlevelsup == x->depth + 1)
	{
		Node	   *call = copyObject(n);
		Param	   *prm = makeNode(Param);
		int			at = -1;

		foreach_ptr(Node, other, func_args(call))
			if (foreach_current_index(other) > 0 && !IsA(strip_relabel(other), Const))
				return expression_tree_mutator(n, derive_mutator, x);
		((Var *) strip_relabel(linitial(func_args(call))))->varlevelsup = 0;
		for (int i = 0; i < list_length(x->calls); i++)
			if (equal(list_nth(x->calls, i), call))
				at = i;
		if (at < 0)
		{
			x->calls = lappend(x->calls, call);
			x->params = lappend_int(x->params, assign_special_exec_param(x->root));
			at = list_length(x->calls) - 1;
		}
		prm->paramkind = PARAM_EXEC;
		prm->paramid = list_nth_int(x->params, at);
		prm->paramtype = exprType(n);
		prm->paramtypmod = exprTypmod(n);
		prm->paramcollid = exprCollation(n);
		prm->location = -1;
		return (Node *) prm;
	}
	return expression_tree_mutator(n, derive_mutator, x);
}

/* does the child still read a column of the enclosing query that a derived call took (it would have to take the whole column)? */
static bool
derived_column_walker(Node *n, DeriveCtx *x)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
	{
		bool		r;

		x->depth++;
		r = query_tree_walker((Query *) n, derived_column_walker, x, 0);
		x->depth--;
		return r;
	}
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == x->depth + 1)
		foreach_ptr(Node, call, x->calls)
		{
			Var		   *v = (Var *) strip_relabel(linitial(func_args(call)));

			if (v->varno == ((Var *) n)->varno && v->varattno == ((Var *) n)->varattno)
				return true;
		}
	return expression_tree_walker(n, derived_column_walker, x);
}

/* The sub-select a correlated function stands for: its columns, with the function's arguments read from inside as outer references. */
static Query *
function_query(RangeTblEntry *e)
{
	Query	   *q = makeNode(Query);
	RangeTblEntry *fe = copyObject(e);
	RangeTblRef *ref = makeNode(RangeTblRef);
	List	   *names = NIL,
			   *vars = NIL;
	ListCell   *lv,
			   *ln;

	if (e->funcordinality || list_length(e->functions) != 1)
		return NULL;
	fe->lateral = false;
	IncrementVarSublevelsUp(linitial_node(RangeTblFunction, fe->functions)->funcexpr, 1, 0);
	q->commandType = CMD_SELECT;
	q->canSetTag = true;
	q->rtable = list_make1(fe);
	ref->rtindex = 1;
	q->jointree = makeFromExpr(list_make1(ref), NULL);
#if PG_VERSION_NUM >= 180000
	expandRTE(fe, 1, 0, VAR_RETURNING_DEFAULT, -1, false, &names, &vars);
#else
	expandRTE(fe, 1, 0, -1, false, &names, &vars);
#endif
	forboth(lv, vars, ln, names)
		q->targetList = lappend(q->targetList, makeTargetEntry(lfirst(lv), list_length(q->targetList) + 1, pstrdup(strVal(lfirst(ln))), false));
	return q;
}

/*
 * What a correlated child is given for a column of the query. The value of a jsonb_each member is not a stored leaf (the leaf holds its
 * float8), so the child gets the jsonb the leaf stands for, built from the key (a member exists) and the number: a JSON null is the NULL
 * of the number, a number is the jsonb of its float8. A cast the child makes of it raises as it does on the stored value, for the
 * members the child runs for.
 */
static Node *
child_arg(AggCtx *c, Node *arg)
{
	Var		   *v = IsA(arg, Var) ? (Var *) arg : NULL;
	Var		   *key;
	Node	   *number;
	CaseExpr   *out = makeNode(CaseExpr);
	CaseWhen   *no_member = makeNode(CaseWhen),
			   *json_null = makeNode(CaseWhen);
	NullTest   *absent = makeNode(NullTest),
			   *nil = makeNode(NullTest);

	if (v == NULL || c->func_kind != FK_EACH || v->varlevelsup != 0 || v->varno != c->func_rti || v->varattno != 2 || v->vartype != JSONBOID)
		return arg;
	key = copyObject(v);
	key->varattno = 1;
	key->vartype = TEXTOID;
	key->vartypmod = -1;
	key->varcollid = DEFAULT_COLLATION_OID;
	number = (Node *) makeFuncExpr(F_FLOAT8_JSONB, FLOAT8OID, list_make1(arg), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	absent->arg = (Expr *) key;
	absent->nulltesttype = IS_NULL;
	absent->location = -1;
	nil->arg = (Expr *) number;
	nil->nulltesttype = IS_NULL;
	nil->location = -1;
	no_member->expr = (Expr *) absent;
	no_member->result = (Expr *) makeNullConst(JSONBOID, -1, InvalidOid);
	no_member->location = -1;
	json_null->expr = (Expr *) nil;
	json_null->result = (Expr *) makeConst(JSONBOID, -1, InvalidOid, -1, DirectFunctionCall1(jsonb_in, CStringGetDatum("null")), false, false);
	json_null->location = -1;
	out->casetype = JSONBOID;
	out->args = list_make2(no_member, json_null);
	/* a row without the member and one with a JSON null are the same NULL: the child reads the value only through casts (null_blind) */
	if (c->func_key)
	{
		no_member->expr = (Expr *) nil;
		out->args = list_make1(no_member);
	}
	out->defresult = (Expr *) makeFuncExpr(F_TO_JSONB, JSONBOID, list_make1(copyObject(number)), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	out->location = -1;
	return (Node *) out;
}

typedef struct BareRead
{
	Index		rti;			/* the unnested function */
	int			level;			/* query levels below the child's own */
} BareRead;

/* does the child read a column of the function other than as the argument of a cast of jsonb to a number or boolean */
static bool
bare_read_walker(Node *n, BareRead *x)
{
	if (n == NULL)
		return false;
	if (IsA(n, FuncExpr) && json_cast_target(((FuncExpr *) n)->funcid) != InvalidOid && IsA(linitial(((FuncExpr *) n)->args), Var) &&
		((Var *) linitial(((FuncExpr *) n)->args))->varno == x->rti && ((Var *) linitial(((FuncExpr *) n)->args))->varlevelsup == x->level &&
		((Var *) linitial(((FuncExpr *) n)->args))->varattno == 2)
		return false;
	if (IsA(n, Var))
		return ((Var *) n)->varno == x->rti && ((Var *) n)->varlevelsup == x->level;
	if (IsA(n, Query))
	{
		bool		bare;

		x->level++;
		bare = query_tree_walker((Query *) n, bare_read_walker, x, 0);
		x->level--;
		return bare;
	}
	return expression_tree_walker(n, bare_read_walker, x);
}

/* The parameters of a child and what the scan gives for each. */
static void
child_params(AggCtx *c, JoinCtx *jc, List *items)
{
	foreach_node(PlannerParamItem, item, items)
	{
		jc->j.params = lappend_int(jc->j.params, item->paramId);
		jc->args = lappend(jc->args, child_arg(c, copyObject(item->item)));
	}
}

/* The plan PostgreSQL made of a correlated sub-select, with the quals it pushed into it: its parameters are what it read of the query's rows. */
static bool
planned_child(AggCtx *c, RelOptInfo *rel, JoinCtx *jc)
{
	if ((jc->cpath = child_path(rel, true)) == NULL || !IsA(jc->cpath, SubqueryScanPath) || rel->subplan_params == NIL)
		return false;
	jc->subroot = rel->subroot;
	child_params(c, jc, rel->subplan_params);
	return true;
}

/*
 * A sub-select or function that reads the query's other tables is planned in a query level of its own, with what it reads of them as
 * parameters: the scan gives it their values for each distinct input and runs the plan PostgreSQL makes of it (an Append of the arms of
 * a UNION ALL, an unnest of a joined table's array, a LIMIT lookup). A column it reads only through calls that never raise (key
 * presence) is replaced by the call's value, which the scan then needs, not the column. The scan runs the child for the distinct values of the
 * tuples that reach its join, which are those PostgreSQL's plan reaches it with only when no other join or unnest can come before it: a child that can
 * raise (raising_walker) keeps the normal plan unless it is the only one.
 */
static bool
correlated_child(AggCtx *c, RangeTblEntry *e, RelOptInfo *rel, JoinCtx *jc)
{
	DeriveCtx	x = {0, NIL, NIL, c->root};
	Query	   *q = e->rtekind == RTE_FUNCTION ? function_query(e) : copyObject(e->subquery),
			   *derived;
	List	   *saved = c->root->plan_params;
	RelOptInfo *final;
	SubqueryScanPath *p = makeNode(SubqueryScanPath);

	/* the member a LEFT JOIN LATERAL jsonb_each picks is NULL for a row without it and for a JSON null alike: a child reads it only through casts (a JSON null casts to NULL in PostgreSQL 18) */
	if (c->func_key && bare_read_walker(e->rtekind == RTE_FUNCTION ? (Node *) e->functions : (Node *) e->subquery, &(BareRead) {c->func_rti, 0}))
		return false;
	if (c->nsiblings > 1 && raising_walker(e->rtekind == RTE_FUNCTION ? (Node *) e->functions : (Node *) e->subquery, c))
		return false;
	if (rel->baserestrictinfo != NIL)
		return e->rtekind == RTE_SUBQUERY && planned_child(c, rel, jc);
	if (q == NULL)
		return false;
	derived = query_tree_mutator(copyObject(q), derive_mutator, &x, 0);
	if (x.calls != NIL && !query_tree_walker(derived, derived_column_walker, &x, 0))
		q = derived;
	else
		x.calls = NIL;
	/* the calls are plan parameters of this level while the child is planned, as PostgreSQL's own for a PlaceHolderVar are */
	c->root->plan_params = NIL;
	for (int i = 0; i < list_length(x.calls); i++)
	{
		PlannerParamItem *item = makeNode(PlannerParamItem);

		item->item = list_nth(x.calls, i);
		item->paramId = list_nth_int(x.params, i);
		c->root->plan_params = lappend(c->root->plan_params, item);
	}
	jc->subroot = sift_subquery_planner(c->root->glob, q, c->root, e->eref->aliasname);
	child_params(c, jc, c->root->plan_params);
	c->root->plan_params = saved;
	if (jc->j.params == NIL)
		return false;
	final = fetch_upper_rel(jc->subroot, UPPERREL_FINAL, NULL);
	set_cheapest(final);
	p->path.pathtype = T_SubqueryScan;
	p->path.parent = rel;
	p->path.pathtarget = rel->reltarget;
	p->subpath = final->cheapest_total_path;
	p->path.rows = p->subpath->rows;
	p->path.startup_cost = p->subpath->startup_cost;
	p->path.total_cost = p->subpath->total_cost + cpu_tuple_cost * p->path.rows;
	jc->cpath = (Path *) p;
	return true;
}

/* the rows the scan keeps of a correlated child, those of its plan for each distinct input, are bounded as the rows of a plan read once are */
static bool
child_rows_fit(AggCtx *c, JoinCtx *jc)
{
	return jc->cpath->rows * Min(estimate_num_groups(c->root, jc->args, c->rel->tuples, NULL, NULL), c->rel->tuples) <= sift_agg_lookup_max_rows;
}

/* the plan of a correlated child, made once for the query: the other table of a flipped join reads the same child */
static bool
planned_correlated_child(AggCtx *c, RangeTblEntry *e, RelOptInfo *rel, JoinCtx *jc)
{
	JoinCtx    *hit;

	foreach_ptr(JoinCtx, done, c->planned)
		if (done->rti == jc->rti)
		{
			jc->cpath = done->cpath;
			jc->subroot = done->subroot;
			jc->j.params = done->j.params;
			jc->args = done->args;
			return done->cpath != NULL;
		}
	hit = palloc0(sizeof(JoinCtx));
	hit->rti = jc->rti;
	if (correlated_child(c, e, rel, jc) && child_rows_fit(c, jc))
		*hit = *jc;
	c->planned = lappend(c->planned, hit);
	return hit->cpath != NULL;
}

/* a parameter that a query above carries into the tree (PostgreSQL has replaced a column of it in the expressions of the query itself) */
static bool
ancestor_param_walker(Node *n, PlannerInfo *root)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
		for (PlannerInfo *up = root->parent_root; up != NULL; up = up->parent_root)
			foreach_node(PlannerParamItem, item, up->plan_params)
				if (item->paramId == ((Param *) n)->paramid)
					return true;
	return expression_tree_walker(n, ancestor_param_walker, root);
}

/* does the right side read a column (or aggregate, or CTE) of the query, or above it when skip is 1? */
static bool
reads_outer(AggCtx *c, RangeTblEntry *e, int skip)
{
	/* the expressions of a function or VALUES list are in the query itself, the body of a sub-select one level below it */
	OuterRef	oref = {c->root, e->rtekind == RTE_SUBQUERY ? 0 : -1, skip};
	Node	   *expr = e->rtekind == RTE_VALUES ? (Node *) e->values_lists : (Node *) e->functions;

	if (e->rtekind == RTE_SUBQUERY)
		return query_tree_walker(e->subquery, outer_ref_walker, &oref, QTW_EXAMINE_RTES_BEFORE);
	if (e->rtekind != RTE_VALUES && e->rtekind != RTE_FUNCTION)
		return false;
	return outer_ref_walker(expr, &oref) || (skip && ancestor_param_walker(expr, c->root));
}

/*
 * The right side of a join that is a plan: a sub-select PostgreSQL did not pull up (GROUP BY, LIMIT, DISTINCT, a set operation), a VALUES list
 * or a function that reads nothing of the query's other tables. Its rows are those of PostgreSQL's plan of it, read once when the scan starts
 * (SiftAggJoin.child). A correlated subquery keeps its native parameterized path and is rescanned per cached input tuple.
 */
static bool
add_plan_join(AggCtx *c, RangeTblEntry *e, Index rti, JoinCtx *jc, const char **what)
{
	RelOptInfo *rel = c->root->simple_rel_array[rti];

	switch (e->rtekind)
	{
		case RTE_CTE:
		{
			PlannerInfo *owner = c->root;

			for (int level = 0; level < e->ctelevelsup; level++)
				owner = owner->parent_root;
			*what = "cte";
			/* A statement-level CTE has no changing outer-row inputs, wherever it is referenced. */
			if (owner->parent_root != NULL || e->self_reference)
				return false;
			break;
		}
		case RTE_SUBQUERY:
			*what = "subquery";
			if (e->subquery->rowMarks != NIL || contain_volatile_functions((Node *) e->subquery))
				return false;
			break;
		case RTE_VALUES:
			*what = "values";
			if (contain_volatile_functions((Node *) e->values_lists))
				return false;
			break;
		default:
			*what = "function";
			if (contain_volatile_functions((Node *) e->functions))
				return false;
			break;
	}
	/*
	 * A column of a query above the aggregate's own reaches the child as a parameter of that query's plan, which changes per row of it; the
	 * scan replays its groups when rescanned (agg_rescan) and never sees it. Only the query's own tables are an input of the child.
	 */
	if (rel == NULL || rel->reloptkind != RELOPT_BASEREL || reads_outer(c, e, 1))
		return false;
	if (reads_outer(c, e, 0) || !bms_is_empty(rel->lateral_relids))
		return (e->rtekind == RTE_SUBQUERY || e->rtekind == RTE_FUNCTION) && planned_correlated_child(c, e, rel, jc);
	if ((jc->cpath = child_path(rel, false)) == NULL || jc->cpath->rows > sift_agg_lookup_max_rows)
		return false;
	jc->j.child = 0;
	foreach_ptr(JoinCtx, o, c->joins)
		jc->j.child += o->cpath != NULL && o->j.params == NIL;
	return true;
}

/* a table or plan in the FROM list besides the scanned one: it joins by equality of its columns to leaves */
bool
add_join(AggCtx *c, FromScan *fs, Index rti)
{
	RangeTblEntry *e = rt_fetch(rti, c->root->parse->rtable);
	JoinCtx    *jc = palloc0(sizeof(JoinCtx));
	const char *what = NULL;

	jc->rti = rti;
	jc->j.child = -1;
	jc->j.fslot = -1;
	if (e->rtekind == RTE_RELATION)
	{
		if (e->relkind != RELKIND_RELATION)
			return false;
		jc->j.relid = e->relid;
	}
	else if (!add_plan_join(c, e, rti, jc, &what))
		return false;
	foreach_ptr(LeftJoin, lj, fs->lefts)
		if (lj->rti == rti)
		{
			jc->kind = lj->kind;
			jc->clauses = lj->quals;
		}
	jc->j.kind = jc->kind;
	jc->j.desc = psprintf("%s%s", what ? (e->rtekind == RTE_VALUES ? "values" : psprintf("%s (%s)", e->eref->aliasname, what)) : get_rel_name(e->relid),
						  jc->kind == SJ_LEFT ? " (left)" : jc->kind == SJ_SEMI ? " (semi)" : jc->kind == SJ_ANTI ? " (anti)" :
						  jc->kind == SJ_RIGHT ? " (preserved)" : jc->kind == SJ_FULL ? " (full)" : "");
	jc->combo = combo_index(c, new_combo(c, list_length(c->joins)));
	c->joins = lappend(c->joins, jc);
	return true;
}

/*
 * `column = value` among the table's own conditions (the value reads no column, is not volatile and takes no executor parameter): the rows of
 * the table that can match are those of an index scan on the column, when the table has a btree on it (the executor looks, map_load).
 * The conjunct stays in the filter; this only narrows what is read.
 */
static void
join_index_quals(AggCtx *c, JoinCtx *jc, List *conds)
{
	SiftAggJoin *j = &jc->j;

	if (jc->cpath != NULL || sift_aggjoin_preserves(jc->kind))
		return;
	foreach_ptr(Node, q, conds)
	{
		OpExpr	   *op;
		Node	   *l,
				   *r,
				   *val;
		Oid			opno;

		if (!IsA(q, OpExpr) || j->nidx == SIFT_AGG_MAX_JIDX || list_length(((OpExpr *) q)->args) != 2)
			continue;
		op = (OpExpr *) q;
		l = strip_relabel(linitial(op->args));
		r = strip_relabel(lsecond(op->args));
		opno = op->opno;
		if (join_of_var(c, l) == jc)
			val = r;
		else if (join_of_var(c, r) == jc)
		{
			val = l;
			l = r;
			opno = get_commutator(opno);
		}
		else
			continue;
		if (!OidIsValid(opno) || !op_mergejoinable(opno, exprType(l)) || unsupported_clause(c, val) || contain_var_clause(val))
			continue;
		j->idxatt[j->nidx] = ((Var *) l)->varattno;
		j->idxop[j->nidx] = opno;
		j->idxcoll[j->nidx] = op->inputcollid;
		j->idxval[j->nidx++] = val;
	}
}

/* the rest of the ON clause of a joined table: a boolean slot of its combo, over the leaves and the columns of a candidate row */
static bool
join_filter(AggCtx *c, JoinCtx *jc, List *conds)
{
	int			ci,
				si;

	join_index_quals(c, jc, conds);

	if (!combo_slot_in(c, jc, normalize_expr(c, (Node *) make_ands_explicit(conds)), false, false, &ci, &si) || ci != jc->combo)
		return false;
	jc->j.fslot = si;
	return true;
}

bool
preserved_join(AggCtx *c)
{
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind))
			return true;
	return false;
}

/* Nulling join RTIs describe expression boundaries, not lookup inputs. */
static Relids
lookup_inputs(AggCtx *c, Node *q)
{
	Relids		vars = pull_varnos(c->root, q);

	for (int rti = -1; (rti = bms_next_member(vars, rti)) >= 0;)
		if (rt_fetch(rti, c->root->parse->rtable)->rtekind == RTE_JOIN)
			vars = bms_del_member(vars, rti);
	return vars;
}

/* A conjunct can probe this lookup when all its other inputs have already been produced. */
static bool
lookup_ready(AggCtx *c, JoinCtx *jc, List *quals, Relids done)
{
	Relids		available = bms_add_member(bms_copy(done), jc->rti);

	if (jc->j.params != NIL)
		return bms_is_subset(lookup_inputs(c, (Node *) jc->args), done);
	foreach_ptr(Node, q, quals)
	{
		Relids		vars = lookup_inputs(c, q);

		if (bms_is_member(jc->rti, vars) && bms_is_subset(vars, available))
			return true;
	}
	return false;
}

static bool
order_inner_lookups(AggCtx *c, List *pending, List *quals, List **ordered, Relids *done)
{
	while (pending != NIL)
	{
		JoinCtx    *next = NULL;

		foreach_ptr(JoinCtx, jc, pending)
			if (lookup_ready(c, jc, quals, *done))
			{
				next = jc;
				break;
			}
		if (next == NULL)
			return false;
		*ordered = lappend(*ordered, next);
		*done = bms_add_member(*done, next->rti);
		pending = list_delete_ptr(pending, next);
	}
	return true;
}

/* Inner lookups commute within a join boundary; outer/semi/anti joins keep their relative position. */
static bool
order_lookups(AggCtx *c, List *quals)
{
	List	   *pending = NIL,
			   *ordered = NIL,
			   *combos = NIL;
	Relids		done = bms_make_singleton(c->relid);
	int			child = 0;

	if (c->func_rti)
		done = bms_add_member(done, c->func_rti);
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		if (jc->kind == SJ_INNER)
		{
			pending = lappend(pending, jc);
			continue;
		}
		if (!order_inner_lookups(c, pending, quals, &ordered, &done))
			return false;
		pending = NIL;
		foreach_ptr(Node, q, jc->clauses)
			if (!bms_is_subset(lookup_inputs(c, q), bms_add_member(bms_copy(done), jc->rti)))
				return false;
		if (jc->j.params != NIL && !bms_is_subset(lookup_inputs(c, (Node *) jc->args), done))
			return false;
		ordered = lappend(ordered, jc);
		done = bms_add_member(done, jc->rti);
	}
	if (!order_inner_lookups(c, pending, quals, &ordered, &done))
		return false;
	Assert(list_length(c->combos) == list_length(c->joins));
	foreach_ptr(JoinCtx, jc, ordered)
	{
		ComboCtx   *cb = list_nth(c->combos, jc->combo);

		jc->combo = list_length(combos);
		cb->join = jc->combo;
		combos = lappend(combos, cb);
		if (jc->cpath && jc->j.params == NIL)
			jc->j.child = child++;
	}
	c->joins = ordered;
	c->combos = combos;
	return true;
}

/*
 * The conjuncts of the WHERE and of the ON clauses that read a joined table. Those of an inner join are its ON clause (the same rows
 * either way); those of an outer, semi or anti join filter its output rows (*post). In the ON clause of every join an equality of a
 * column with a leaf, or with an expression of leaves, is a key; the rest filters the table's rows for each tuple (join_filter).
 * *quals is left with the conjuncts that read no joined table.
 */
static bool
join_quals(AggCtx *c, List **quals, List **post, bool nullable_input)
{
	List	   *rest = NIL;

	foreach_ptr(Node, q, *quals)
	{
		Relids		v = pull_varnos(c->root, q);
		JoinCtx    *owner = NULL;

		foreach_ptr(JoinCtx, jc, c->joins)
			if (bms_is_member(jc->rti, v))
			{
				owner = jc;
			}
		if (owner == NULL)
			rest = lappend(rest, q);
		else if (owner->kind == SJ_INNER && (nullable_input || !preserved_join(c)))
			owner->clauses = lappend(owner->clauses, q);
		else
			*post = lappend(*post, q);
	}
	/* a WHERE the scanned table's NULL row can pass is evaluated after the outer join: it is not a filter of the rows the scan reads */
	if (rest != NIL && !nullable_input && preserved_join(c))
		return false;
	*quals = rest;
	return true;
}

bool
setup_joins(AggCtx *c, List **quals, List **post, List **scan_quals, List **scan_post)
{
	List	   *probe_quals = preserved_join(c) ? list_copy(*scan_quals) : list_concat_copy(*quals, *scan_quals);

	foreach_ptr(JoinCtx, jc, c->joins)
		if (jc->kind == SJ_INNER)
			probe_quals = list_concat(probe_quals, list_copy(jc->clauses));
	if (!order_lookups(c, probe_quals))
		return false;
	if (!join_quals(c, quals, post, false) || !join_quals(c, scan_quals, scan_post, true))
		return false;
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		List	   *conds = NIL;
		ComboCtx   *cb = list_nth(c->combos, jc->combo);

		foreach_ptr(Node, arg, jc->args)
		{
			Node       *expr = normalize_expr(c, arg);
			ComboWalk   w = {c};

			combo_walker(expr, &w);
			if (w.bad || (w.jc && w.jc->combo >= jc->combo))
				return false;
			for (int p = 0; p < w.nl; p++)
			{
				if (combo_leaf(cb, w.leaf[p], true, w.ptype[p], w.ptypmod[p], w.pcoll[p]) < 0)
					return false;
				use_leaf(c, w.leaf[p]);
			}
			foreach_ptr(Var, v, w.vars)
				if (combo_reference(c, cb, join_of_var(c, (Node *) v), v) < 0)
					return false;
			jc->j.args = lappend(jc->j.args, expr);
		}
		foreach_ptr(Node, q, jc->clauses)
			if (jc->j.params != NIL || (!join_clause(c, jc, q) && !join_expr_clause(c, jc, q)))
				conds = lappend(conds, q);
		if ((jc->j.nkey == 0 && conds == NIL && jc->j.params == NIL) || (conds != NIL && !join_filter(c, jc, conds)))
			return false;
	}
	return true;
}

/*
 * The FROM list of root's query as AggCtx: the scanned table with a column source, at most one lateral unnest of one of its leaves,
 * the tables that join to it by equality. input_rel (NULL: not the query being planned) is the join the planner made of it.
 */
/*
 * The key of `LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'`, NULL for any other outer join of the function. An object has one member
 * per key, so the join is the member k of each row (a row without it keeps NULL columns): a row level value, not an unnest. A JSON null and
 * a missing key are the same NULL there; before PostgreSQL 18 a JSON null raises where the missing key does not, so the normal plan stays.
 */
static const char *
left_each_key(LeftJoin *lj, Index rti, FuncKind kind)
{
#if PG_VERSION_NUM >= 180000
	OpExpr	   *op;
	Node	   *a,
			   *b;

	if (kind != FK_EACH || lj->kind != SJ_LEFT || list_length(lj->quals) != 1 || !IsA(linitial(lj->quals), OpExpr))
		return NULL;
	op = linitial_node(OpExpr, lj->quals);
	set_opfuncid(op);
	if (op->opfuncid != F_TEXTEQ)
		return NULL;
	a = strip_relabel(linitial(op->args));
	b = strip_relabel(lsecond(op->args));
	if (IsA(b, Var))
	{
		Node	   *t = a;

		a = b;
		b = t;
	}
	if (!IsA(a, Var) || ((Var *) a)->varno != rti || ((Var *) a)->varlevelsup != 0 || ((Var *) a)->varattno != 1 || !IsA(b, Const) ||
		((Const *) b)->constisnull)
		return NULL;
	return TextDatumGetCString(((Const *) b)->constvalue);
#else
	return NULL;
#endif
}

bool
agg_base(AggCtx *c, PlannerInfo *root, RelOptInfo *input_rel, FromScan *fs)
{
	Query	   *parse = root->parse;
	IndexOptInfo *ii;
	Bitmapset  *allrtis = NULL;
	List	   *live_rtis = NIL,
			   *joined = NIL;
	int			nin = 0;

	scan_from((Node *) parse->jointree, fs);
	if (!fs->ok)
		return false;
	/* Join removal leaves the original FROM tree in place, but removes its base relations. */
	foreach_int(rti, fs->rtis)
		if (root->simple_rel_array[rti] != NULL)
			live_rtis = lappend_int(live_rtis, rti);
	fs->rtis = live_rtis;
	c->root = root;
	c->group = -1;
	c->nulling = fs->ojrels;
	foreach_int(rti, fs->rtis)
	{
		RangeTblEntry *e = rt_fetch(rti, parse->rtable);

		allrtis = bms_add_member(allrtis, rti);
		if (e->rtekind == RTE_RELATION)
		{
			bool		part = e->inh && e->relkind == RELKIND_PARTITIONED_TABLE;

			if ((e->inh && !part) || e->tablesample || e->securityQuals != NIL ||
				(e->relkind != RELKIND_RELATION && e->relkind != RELKIND_MATVIEW && !part))
				return false;
			if (c->relid == 0)
				c->relid = rti;
			else if (part || c->joinless)
				return false;
			else
				joined = lappend_int(joined, rti);
		}
		else if (e->rtekind == RTE_FUNCTION && e->lateral && c->func_rti == 0 && !e->funcordinality && list_length(e->functions) == 1 &&
				 func_kind_of(linitial_node(RangeTblFunction, e->functions)) != FK_NONE)
		{
			c->func_rti = rti;
			c->func = linitial_node(RangeTblFunction, e->functions);
			c->func_kind = func_kind_of(c->func);
			/* the scan unnests the elements that pass: a row without one is no row of an outer join (it keeps NULL columns) */
			foreach_ptr(LeftJoin, lj, fs->lefts)
				if (lj->rti == rti && (c->func_key = left_each_key(lj, rti, c->func_kind)) == NULL)
					return false;
		}
		else if (input_rel == NULL || c->joinless ||
				 (e->rtekind != RTE_SUBQUERY && e->rtekind != RTE_VALUES && e->rtekind != RTE_FUNCTION && e->rtekind != RTE_CTE))
			return false;
		else
			joined = lappend_int(joined, rti);
	}
	if (c->relid == 0)
		return false;
	/* a plan joined to the scan by an outer join keeps its own rows: the scanned table is never the nullable side */
	foreach_ptr(LeftJoin, lj, fs->lefts)
		if (lj->rti == c->relid)
			return false;
	c->rel = root->simple_rel_array[c->relid];
	c->heapoid = planner_rt_fetch(c->relid, root)->relid;
	/* the joins come after the source: planning the sub-selects of a query with no column index to scan would be a waste */
	if (c->rel == NULL || !find_source(c, &ii))
		return false;
	c->nsiblings = list_length(joined) + (c->func_rti != 0);
	foreach_int(rti, joined)
		if (!add_join(c, fs, rti))
			return false;
	if (input_rel != NULL)
	{
		if (!bms_is_member(c->relid, input_rel->relids))
			return false;
		for (int m = -1; (m = bms_next_member(input_rel->relids, m)) >= 0;)
			if (rt_fetch(m, parse->rtable)->rtekind != RTE_JOIN)
			{
				if (!bms_is_member(m, allrtis))
					return false;
				nin++;
			}
		if (nin + list_length(c->joins) < list_length(fs->rtis))
			return false;
	}
	/* the rows of a preserved side that nothing matched are known at the end of a single scan of one join */
	if (fs->ojrels && (input_rel == NULL || c->nparts > 0))
		return false;
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind) && jc != llast(c->joins))
			return false;
	build_attexprs(c);
	if (c->func_rti)
	{
		int			leaf = group_leaf_of(c, linitial(((FuncExpr *) c->func->funcexpr)->args), func_leaf_kind(c->func_kind));

		if (leaf < 0)
			return false;
		if (c->func_key)
			c->func_obj = leaf;
		else
		{
			c->group = leaf;
			c->used = bms_add_member(c->used, leaf);
		}
	}
	return true;
}

/* agg_base, then the other table of an outer join of two tables as the scanned one when the first did not give a plan (a preserved side) */
static bool
agg_base_either(AggCtx *c, PlannerInfo *root, RelOptInfo *input_rel, FromScan *fs)
{
	List	   *planned;

	if (agg_base(c, root, input_rel, fs))
		return true;
	if (!fs->flippable)
		return false;
	planned = c->planned;
	memset(c, 0, sizeof(AggCtx));
	c->planned = planned;
	*fs = (FromScan) {.ok = true, .flip = true};
	return agg_base(c, root, input_rel, fs);
}

SiftAggSpec *
agg_spec_new(AggCtx *c)
{
	SiftAggSpec *s = palloc0(sizeof(SiftAggSpec));

	s->indexoid = c->indexoid;
	s->heapoid = c->heapoid;
	s->nparts = c->nparts;
	s->partheap = c->partheap;
	s->partindex = c->partindex;
	s->group = c->group;
	return s;
}

/* the share of the heap pages the TID range of the conjuncts leaves, on the first of them (zone_kept) */
static void
tid_share(AggCtx *c, SiftAggSpec *s)
{
	uint64		lo,
				hi;

	if (!sift_aggspec_tid_range(s, &lo, &hi))
		return;
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_TID)
		{
			s->pred[i]->sel = lo >= hi ? 0.0 : Min(((double) ((hi - 1) >> 16) - (double) (lo >> 16) + 1.0) / Max(rel_pages(c), 1.0), 1.0);
			return;
		}
}

/* the WHERE of the scan: bitmap predicates, and the conjuncts over the joined rows */
bool
agg_quals(AggCtx *c, SiftAggSpec *s, FromScan *fs)
{
	List	   *quals = fs->quals,
			   *joined = NIL,
			   *scan_quals = fs->scan_quals,
			   *scan_post = NIL;
	StringInfoData txt;

	if (!setup_joins(c, &quals, &joined, &scan_quals, &scan_post))
		return false;
	quals = list_concat(quals, scan_quals);
	s->pred = palloc(Max(list_length(quals), 1) * sizeof(SiftAggPred *));
	s->post = palloc(Max(list_length(quals) + list_length(joined) + list_length(scan_post), 1) * sizeof(SiftAggPred *));
	initStringInfo(&txt);
	foreach_ptr(Node, q, quals)
	{
		Node	   *e = normalize_expr(c, q);
		SiftAggPred *p = compile_typeof(c, e);

		if (p == NULL)
			p = compile_wide(c, e);
		if (p == NULL)
			p = compile_tid(c, e);
		if (p == NULL)
			p = compile_pred(c, e);
		if (p == NULL || unsupported_clause(c, q))
			return false;
		if (p->level > 1)
			s->post[s->npost++] = p;
		else
			s->pred[s->npred++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	foreach_ptr(Node, q, scan_post)
	{
		SiftAggPred *p = compile_combo_pred(c, normalize_expr(c, q));

		if (p == NULL || unsupported_clause(c, q))
			return false;
		s->post[s->npost++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	s->nprepost = preserved_join(c) ? s->npost : 0;
	foreach_ptr(Node, q, joined)
	{
		SiftAggPred *p = compile_combo_pred(c, normalize_expr(c, q));

		if (p == NULL || unsupported_clause(c, q))
			return false;
		s->post[s->npost++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	s->desc_quals = txt.data;
	tid_share(c, s);
	return true;
}

static void
spec_deps(PlannerInfo *root, SiftAggSpec *s)
{
	if (s->search)
		record_deps(root, s->search);
	for (int i = 0; i < s->npred; i++)
		pred_deps(root, s->pred[i]);
	for (int i = 0; i < s->nkeys; i++)
		record_deps(root, s->key[i].expr);
	for (int i = 0; i < s->naggs; i++)
		pred_deps(root, s->agg[i].filter);
	for (int i = 0; i < s->ncombo; i++)
		for (int k = 0; k < s->combo[i].nslots; k++)
			record_deps(root, s->combo[i].slot[k].expr);
	for (int i = 0; i < s->njoin; i++)
		for (int k = 0; k < s->join[i].nkey; k++)
			record_deps(root, s->join[i].keyexpr[k]);
	for (int a = 0; a < s->narms; a++)
	{
		for (int i = 0; i < s->arm[a].nfilter; i++)
			pred_deps(root, s->arm[a].filter[i]);
		for (int i = 0; i < s->arm[a].ninner; i++)
			pred_deps(root, s->arm[a].inner[i].filter);
	}
}

/* the groups the scan makes: those of the keys, or of each grouping set */
/* what PostgreSQL has statistics of for a key: the column behind an expression that denotes a stored generated column, else the expression */
static Node *
stats_expr(AggCtx *c, Node *e)
{
	int			att = attr_of_expr(c, e);

	return att > 0 ? list_nth(c->attexpr, att - 1) : e;
}

static double
set_groups(AggCtx *c, PlannerInfo *root, SiftAggSpec *s, List *keys, double rows)
{
	double		n = 0;

	for (int set = 0; set < Max(s->nsets, 1); set++)
	{
		List	   *sub = NIL;
		double		bucketed = 1.0,
					g;

		for (int j = 0; j < list_length(keys); j++)
		{
			double		b = key_groups(c, &s->key[j]);

			if (s->nsets != 0 && !(s->setmask[set] >> j & 1))
				continue;
			if (b > 0)
				bucketed *= b;
			else
				sub = lappend(sub, stats_expr(c, list_nth(keys, j)));
		}
		/* Min and Max evaluate their arguments more than once: the estimate is made once */
		g = sub == NIL ? 1.0 : estimate_num_groups(root, sub, rows, NULL, NULL);
		n += Min(rows, Max(g, 1.0) * bucketed);
	}
	return n;
}

/* the position (1..) of column att in the output of a joined plan */
static int
child_pos(List *vars, int att)
{
	foreach_ptr(Var, v, vars)
		if (v->varattno == att)
			return foreach_current_index(v) + 1;
	elog(ERROR, "sift: column %d is not an output of the joined plan", att);
}

/* A path with the given number of workers: the spec travels with it, and the cheaper of the paths wins */
void
add_agg_path(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, List *scan_tlist, int nworkers)
{
	SiftAggPath *ap = (SiftAggPath *) newNode(sizeof(SiftAggPath), T_CustomPath);
	CustomPath *cp = &ap->path;

	s->nworkers = nworkers;
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = target;
	cp->path.parallel_safe = false;
	cp->methods = &agg_path_methods;
	agg_cost(c, s, input_rel, &cp->path, s->est_groups);
	cp->custom_private = list_make2(sift_aggspec_pack(s), scan_tlist);
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		ap->correlated = lappend(ap->correlated, jc->j.params != NIL ? ((SubqueryScanPath *) jc->cpath)->subpath : NULL);
		ap->subroots = lappend(ap->subroots, jc->subroot);
		if (jc->cpath != NULL && jc->j.params == NIL)
			cp->custom_paths = lappend(cp->custom_paths, jc->cpath);
	}
	add_path(output_rel, &cp->path);
}

/*
 * A column of a sub-select that only a condition pushed down into it read is a NULL in its plan (remove_unused_subquery_outputs), so the
 * spec, which reads the condition again, must not read it: the normal plan stays. The plan of a sub-select, VALUES list or function keeps
 * the columns of the conditions it evaluates; an Append or MergeAppend outputs the relation's target only.
 */
static bool
child_cols_kept(AggCtx *c, JoinCtx *jc)
{
	RelOptInfo *rel = find_base_rel(c->root, jc->rti);
	Bitmapset  *used = NULL;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &used);
	if (projects_columns(jc->cpath))
		foreach_ptr(RestrictInfo, ri, list_concat_copy(rel->baserestrictinfo, rel->joininfo))
			pull_varattnos((Node *) ri->clause, rel->relid, &used);
	foreach_ptr(Var, v, jc->cvars)
		if (!bms_is_member(v->varattno - FirstLowInvalidHeapAttributeNumber, used))
			return false;
	return true;
}

/*
 * The plans the joins read, as paths that output the columns the spec reads (the columns of the joined rows are positions of that output): the
 * path PostgreSQL chose for the rows of the sub-select, VALUES list or function, projected on them. The scan nodes project any column.
 */
bool
child_paths(AggCtx *c, SiftAggSpec *s)
{
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		SiftAggJoin *j = &s->join[foreach_current_index(jc)];
		PathTarget *t = create_empty_pathtarget();
		Path	   *p;
		Size		size;

		if (jc->cpath == NULL)
			continue;
		if (!child_cols_kept(c, jc))
			return false;
		if (jc->j.params != NIL)
			continue;
		/* the child's own columns, before any outer join nulls them: the plan below the projection outputs them so */
		foreach_ptr(Var, v, jc->cvars)
		{
			Var		   *own = copyObject(v);

			own->varnullingrels = NULL;
			add_column_to_pathtarget(t, (Expr *) own, 0);
		}
		set_pathtarget_cost_width(c->root, t);
		for (int k = 0; k < j->nkey; k++)
			j->keyatt[k] = child_pos(jc->cvars, j->keyatt[k]);
		for (int k = 0; k < j->ncol; k++)
			j->colatt[k] = child_pos(jc->cvars, j->colatt[k]);
		if (!projects_columns(jc->cpath))
		{
			jc->cpath = (Path *) create_projection_path(c->root, c->root->simple_rel_array[jc->rti], jc->cpath, t);
			continue;
		}
		size = IsA(jc->cpath, SubqueryScanPath) ? sizeof(SubqueryScanPath) : sizeof(Path);
		p = memcpy(palloc(size), jc->cpath, size);
		p->pathtarget = t;
		jc->cpath = p;
	}
	return true;
}

/*
 * Workers for the scan, as PostgreSQL decides them for a parallel seq scan of the table (size, reloption, the GUCs), when the whole
 * query is safe to run in a worker (the workers evaluate the spec's expressions) and the spec's tables can be merged
 * (sift_aggspec_parallel_ok).
 */
static int
parallel_workers(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	/* the relations' own flags, as PostgreSQL's grouping does: the query-wide hazard counts the sub-selects it pulled up into semi joins as restricted */
	if (!sift_aggspec_parallel_ok(s) || c->nsubplan > 0 || !c->root->glob->parallelModeOK || !output_rel->consider_parallel ||
		!input_rel->consider_parallel || !c->rel->consider_parallel || max_parallel_workers_per_gather <= 0)
		return 0;
	/* of the equal keys 1.0 and 1.00 the group shows the one of the first row, which the workers' tables merge in no order */
	for (int i = 0; s->nkeys > 0 && i < s->nleaves; i++)
		if (leaf_of(c, s->leaves[i])->wide)
			return 0;
	return compute_parallel_worker(c->rel, rel_pages(c), -1, max_parallel_workers_per_gather);
}

/*
 * Does the spec read a leaf that holds values the index cannot store? The scan raises an error when a row of its snapshot has one
 * (check_overflow), the normal plan reads them from the heap: the query keeps its plan. Unless the source reads the rows again as SQL reads
 * them (spec_recheck). A numeric leaf is not one: its only such value is NaN, which the scan evaluates (SiftSrcVec.ovf). Nor is the leaf of a
 * jsonb_typeof conjunct (SAP_TYPE): the source keeps the rows of the types and tells the type of each value it cannot store.
 */
static bool
reads_overflow(AggCtx *c, const SiftAggSpec *s)
{
	Bitmapset  *typed = NULL,
			   *leaves;
	int			m = -1;

	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_TYPE)
			typed = bms_add_member(typed, s->pred[i]->leaf);
	leaves = bms_union(c->used, c->func_ovf);
	for (int j = 0; j < c->nvleaf; j++)
		if (bms_is_member(c->lay->nleaves - c->nvleaf + j, c->used) && vleaf_source(c, j) >= 0)
			leaves = bms_add_member(leaves, vleaf_source(c, j));
	while ((m = bms_next_member(leaves, m)) >= 0)
		if (c->stats.ovf[m] && !bms_is_member(m, typed) && spec_recheck(c, s, m) == SRK_NONE)
		{
			ereport(DEBUG1, (errmsg("sift: the aggregate pushdown does not read \"%s\": some unit holds values the leaf cannot store", leaf_name(c, m))));
			return true;
		}
	return false;
}

/* the leaves the spec reads and the combos; false when the spec cannot be run. keys: the group expressions of root's query */
bool
spec_finish(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, List *keys)
{
	if (c->other_group || types_guard_reads(c))
		return false;
	share_ordered(s);
	if (c->group < 0)
		for (int i = 0; i < c->lay->nleaves; i++)
			if (bms_is_member(i, c->used) && leaf_of(c, i)->parent >= 0)
				return false;
	if (c->group >= 0 && !bms_is_member(c->group, c->used))
		return false;
	if (list_length(c->combos) > SIFT_AGG_MAX_COMBOS || list_length(c->joins) > SIFT_AGG_MAX_JOINS)
		return false;
	typeof_reads(c, s);
	s->npreds_total = c->npred;
	s->nvleaf = c->nvleaf;
	memcpy(s->vleaf, c->vleaf, sizeof(c->vleaf));
	c->used = bms_union(c->used, c->elem_used);
	if (reads_overflow(c, s))
		return false;
	finish_leaves(c, s);
	finish_combos(c, s);
	s->est_groups = set_groups(c, root, s, keys, input_rel->rows);
	if (c->search)
		s->search = c->search->spec;

	spec_deps(root, s);
	return true;
}

/*
 * A set filter bound as one array parameter (`col = ANY($1)`) has no length in a generic plan: PostgreSQL assumes 10 elements and prices an
 * index or bitmap scan for them, while the scan of this node takes the whole array into a byte table when it starts and costs the same
 * for 10 or 100,000 elements. The paths that fetch the rows of such a set one by one are charged the elements sift.agg_param_set_size
 * assumes (their cost grows with the set), and only when this node can run the query, so that a query it cannot take keeps the plan it
 * has. A custom plan sees the array and needs none of this.
 */
int			sift_agg_param_set_size = 1000;
int			sift_agg_lookup_max_rows = 1000000;

static double
param_set_ratio(AggCtx *c)
{
	double		f = 1.0;

	if (sift_agg_param_set_size <= 0)
		return 1.0;
	foreach_node(RestrictInfo, ri, c->rel->baserestrictinfo)
	{
		ScalarArrayOpExpr *sa;
		Node	   *arr;
		double		n0;

		if (!IsA(ri->clause, ScalarArrayOpExpr))
			continue;
		sa = (ScalarArrayOpExpr *) ri->clause;
		if (!sa->useOr || list_length(sa->args) != 2)
			continue;
		arr = strip_relabel(lsecond(sa->args));
		if (!IsA(arr, Param) || ((Param *) arr)->paramkind != PARAM_EXTERN || !IsA(strip_relabel(linitial(sa->args)), Var) ||
			match_leaf(c, linitial(sa->args)) < 0)
			continue;
		n0 = estimate_array_length(c->root, arr);
		if (sift_agg_param_set_size > n0)
			f *= sift_agg_param_set_size / n0;
	}
	return Min(f, Max(c->rel->tuples / Max(c->rel->rows, 1.0), 1.0));
}

/* the scan of the rows of the table a plan reads them with, when it is an index or bitmap scan under aggregation, sorts and gathers */
static Path *
row_scan_of(Path *p)
{
	for (;;)
		switch (nodeTag(p))
		{
			case T_IndexPath:
			case T_BitmapHeapPath:
				return p;
			case T_AggPath:
				p = ((AggPath *) p)->subpath;
				break;
			case T_GroupPath:
				p = ((GroupPath *) p)->subpath;
				break;
			case T_SortPath:
				p = ((SortPath *) p)->subpath;
				break;
			case T_IncrementalSortPath:
				p = ((IncrementalSortPath *) p)->spath.subpath;
				break;
			case T_GatherPath:
				p = ((GatherPath *) p)->subpath;
				break;
			case T_GatherMergePath:
				p = ((GatherMergePath *) p)->subpath;
				break;
			case T_ProjectionPath:
				p = ((ProjectionPath *) p)->subpath;
				break;
			default:
				return NULL;
		}
}

static void
param_set_penalty(AggCtx *c, RelOptInfo *output_rel)
{
	double		f = param_set_ratio(c);

	if (f <= 1.0)
		return;
	foreach_ptr(Path, p, output_rel->pathlist)
		if (row_scan_of(p) != NULL)
		{
			p->startup_cost *= f;
			p->total_cost *= f;
		}
}

/* the cost; adds the paths to output_rel */
void
agg_finish(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, List *keys, List *scan_tlist)
{
	int			nworkers;

	if (!spec_finish(c, s, root, input_rel, keys) || !first_fits(s) || !child_paths(c, s))
		return;
	param_set_penalty(c, output_rel);
	add_agg_path(c, s, input_rel, output_rel, target, scan_tlist, 0);
	nworkers = preagg_share(c, s) >= AGG_PREAGG_EXACT ? 0 : parallel_workers(c, s, input_rel, output_rel);
	if (nworkers > 0)
		add_agg_path(c, s, input_rel, output_rel, target, scan_tlist, nworkers);
}

/* the index among the group columns of the one with the sortgroupref ref */
static int
key_of_ref(Query *parse, int ref)
{
	foreach_node(SortGroupClause, sgc, parse->groupClause)
		if (sgc->tleSortGroupRef == ref)
			return foreach_current_index(sgc);
	elog(ERROR, "sift: grouping column %d not found", ref);
}

/* GROUPING(columns): the keys of its arguments; the executor makes its bitmask of the set of each group */
bool
compile_grouping(Query *parse, GroupingFunc *g, SiftAggAgg *out)
{
	if (g->agglevelsup != 0 || parse->groupingSets == NIL)
		return false;
	out->kind = SAA_GROUPING;
	out->leaf = -1;
	out->restype = INT4OID;
	out->nref = list_length(g->refs);
	out->ref = palloc(out->nref * sizeof(int));
	foreach_int(ref, g->refs)
		out->ref[foreach_current_index(ref)] = key_of_ref(parse, ref);
	out->desc = "GROUPING";
	return true;
}

/*
 * The grouping sets as PostgreSQL expanded them (parse->groupingSets: a list of lists of sortgrouprefs, ROLLUP and CUBE included,
 * duplicates as written): a mask over the group columns for each, and the number of the set as one more key, so that the groups of
 * two sets never meet. The scan emits it as a constant column, which nothing reads.
 */
void
add_sets(Query *parse, SiftAggSpec *s, List **scan_tlist)
{
	SiftAggKey *k = &s->key[s->nkeys];

	s->nsets = list_length(parse->groupingSets);
	s->setmask = palloc(s->nsets * sizeof(uint32));
	foreach_ptr(List, set, parse->groupingSets)
	{
		uint32		mask = (uint32) 1 << s->nkeys;

		foreach_int(ref, set)
			mask |= (uint32) 1 << key_of_ref(parse, ref);
		s->setmask[foreach_current_index(set)] = mask;
	}
	k->kind = SAK_SET;
	k->leaf = -1;
	k->typid = INT4OID;
	k->typmod = -1;
	k->eqop = Int4EqualOperator;
	k->desc = "grouping set";
	*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(0), false, true),
													   list_length(*scan_tlist) + 1, NULL, false));
	s->nkeys++;
}

/* Does the grouping have a key that is not a constant? */
static bool
groupClause_has_nonconst(PlannerInfo *root, List *groupClause)
{
	foreach_node(SortGroupClause, sgc, groupClause)
		if (!IsA(get_sortgroupclause_expr(sgc, root->processed_tlist), Const))
			return true;
	return false;
}

/* ---------- DISTINCT ON: the first row of a group by the ORDER BY ---------- */

#define FIRST_MAX_KEYS		4		/* sort columns after the DISTINCT ON columns */
#define FIRST_GROUP_BYTES	96		/* what a group of a DISTINCT ON column keeps besides its values: the best row's header, the state and the result */

/*
 * Do the sort columns hold the columns of a unique index of the table (plain columns, NOT NULL or with NULLs not distinct), so that no two
 * rows tie? The first row of a group by the ORDER BY is then the same in every plan: a tie would be PostgreSQL's sort's choice.
 */
static bool
sort_is_total(AggCtx *c, List *sortexprs)
{
	Bitmapset  *atts = NULL;
	Relation	rel;
	bool		total = false;

	foreach_ptr(Node, e, sortexprs)
	{
		Var		   *v = (Var *) strip_relabel(e);

		if (IsA(v, Var) && v->varno == c->relid && v->varlevelsup == 0 && v->varattno > 0)
			atts = bms_add_member(atts, v->varattno);
	}
	rel = table_open(c->heapoid, NoLock);
	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		HeapTuple	it = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(ioid));
		Form_pg_index ix = (Form_pg_index) GETSTRUCT(it);
		bool		all = ix->indisunique && ix->indimmediate && ix->indisvalid && ix->indnkeyatts > 0 &&
			heap_attisnull(it, Anum_pg_index_indpred, NULL) && heap_attisnull(it, Anum_pg_index_indexprs, NULL);

		for (int i = 0; all && i < ix->indnkeyatts; i++)
		{
			int			att = ix->indkey.values[i];
			HeapTuple	at = SearchSysCache2(ATTNUM, ObjectIdGetDatum(c->heapoid), Int16GetDatum(att));

			all = bms_is_member(att, atts) && (ix->indnullsnotdistinct || (HeapTupleIsValid(at) && ((Form_pg_attribute) GETSTRUCT(at))->attnotnull));
			if (HeapTupleIsValid(at))
				ReleaseSysCache(at);
		}
		ReleaseSysCache(it);
		total |= all;
	}
	table_close(rel, NoLock);
	return total;
}

/*
 * The value of e in the first row of a group by the sort columns `more`: an aggregate that only carries the argument and the sort keys of
 * the generic aggregates (its function is array_agg, whose catalog entry the compile of a generic aggregate reads); the executor keeps
 * the best row as the rows pass (SiftAggGen.first), and the group's value is e, of its own type.
 */
static Aggref *
first_aggref(AggCtx *c, Node *e, List *more)
{
	Aggref	   *a;
	Oid			t = exprType(e);
	List	   *args = list_make1(makeTargetEntry((Expr *) copyObject(e), 1, NULL, false)),
			   *order = NIL;

	if (type_is_array(t) || arg_joined(c, e))
		return NULL;
	foreach_node(SortGroupClause, sgc, more)
	{
		TargetEntry *k = makeTargetEntry((Expr *) copyObject(get_sortgroupclause_expr(sgc, c->root->processed_tlist)), list_length(args) + 1, NULL, true);
		SortGroupClause *o = copyObject(sgc);

		o->tleSortGroupRef = k->ressortgroupref = k->resno;
		args = lappend(args, k);
		order = lappend(order, o);
	}
	a = makeNode(Aggref);
	a->aggfnoid = F_ARRAY_AGG_ANYNONARRAY;
	a->aggtype = t;
	a->aggcollid = a->inputcollid = exprCollation(e);
	a->aggtranstype = INTERNALOID;
	a->aggargtypes = list_make1_oid(t);
	a->args = args;
	a->aggorder = order;
	a->aggkind = AGGKIND_NORMAL;
	a->aggsplit = AGGSPLIT_SIMPLE;
	a->aggno = a->aggtransno = -1;
	a->location = -1;
	return a;
}

/*
 * The output of DISTINCT ON: the ON columns are the group keys; any other expression of the output (the select list, and the sort columns
 * the sort above reads) is the value of the first row of its group, one aggregate for each, in the scan tuple at the place of the expression.
 */
bool
first_aggs(AggCtx *c, PathTarget *target, List *keys, List *more, List **firsts)
{
	/* the normal plan evaluates a volatile function for every row, before it picks one */
	if (contain_volatile_functions((Node *) target->exprs))
		return false;
	foreach_ptr(Node, e, target->exprs)
	{
		OutWalk		w = {keys, NIL, false};
		Aggref	   *a;
		bool		have = false;

		out_walker(e, &w);
		if (!w.bad)
			continue;
		if ((a = first_aggref(c, e, more)) == NULL)
			return false;
		foreach_ptr(Aggref, f, *firsts)
			have |= equal(f, a);
		if (!have)
			*firsts = lappend(*firsts, a);
	}
	return true;
}

/* a DISTINCT ON aggregate keeps the best row instead of sorting the rows of the group; the first of them compares the sort keys, the others follow its answer */
void
first_mode(SiftAggAgg *out, int nsort, bool leader)
{
	SiftAggGen *g = out->gen;

	g->sorted = false;
	g->first = true;
	g->parallel = true;
	g->cost = leader ? AGG_GEN_FIRST + nsort * AGG_GEN_FIRST_KEY : AGG_GEN_FOLLOW;
	out->desc = psprintf("first_row%s", out->desc + strlen("array_agg"));
}

/* do the best rows of the groups fit the memory a hash aggregate gets? */
bool
first_fits(SiftAggSpec *s)
{
	double		bytes = 0;

	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].kind == SAA_GEN && s->agg[i].gen->first)
		{
			const SiftAggGen *g = s->agg[i].gen;

			bytes += FIRST_GROUP_BYTES;
			for (int k = 0; k < g->nargs; k++)
				bytes += sizeof(Datum) + 1 + get_typavgwidth(g->arg[k].typid, -1);
		}
	return bytes == 0 || s->est_groups * bytes <= (double) get_hash_memory_limit();
}

/* the sort columns after the DISTINCT ON columns, when the ORDER BY is a total order of the rows */
bool
distinct_on_more(AggCtx *c, Query *parse, List **more)
{
	List	   *sortexprs = NIL;

	foreach_node(SortGroupClause, sgc, parse->sortClause)
	{
		sortexprs = lappend(sortexprs, get_sortgroupclause_expr(sgc, c->root->processed_tlist));
		if (arg_joined(c, llast(sortexprs)))
			return false;
		if (get_sortgroupref_clause_noerr(sgc->tleSortGroupRef, parse->distinctClause) == NULL)
			*more = lappend(*more, sgc);
	}
	return list_length(*more) <= FIRST_MAX_KEYS && sort_is_total(c, sortexprs);
}

/*
 * The rows of one scan grouped by `groupClause` (the GROUP BY of the query, or the columns of its SELECT DISTINCT, which is a grouping with no
 * aggregate): the SiftAgg path of output_rel, whose rows are those of `target` (the grouping target of the query, or its distinct target).
 */
static void
group_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, GroupPathExtraData *gextra, List *groupClause)
{
	Query	   *parse = root->parse;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	OutWalk		ow = {NIL, NIL, false};
	List	   *scan_tlist = NIL,
			   *more = NIL,
			   *firsts = NIL;
	bool		lead = true;

	if (!agg_base_either(c, root, input_rel, &fs))
		return;
	s = agg_spec_new(c);
	if (c->nparts == 0)
		c->search = sift_search_extract(root, c->rel, &fs.quals, c->indexoid, c->heapoid, false);
	if (!agg_quals(c, s, &fs))
		return;

	/* keys */
	if (list_length(groupClause) + (parse->groupingSets != NIL) > SIFT_AGG_MAX_KEYS)
		return;
	s->key = palloc0((list_length(groupClause) + 1) * sizeof(SiftAggKey));
	foreach_node(SortGroupClause, sgc, groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);

		/* a constant adds no group: the output keeps it as the constant it is (not with grouping sets, whose empty set has no key, and not alone: an empty input has no group) */
		if (IsA(e, Const) && parse->groupingSets == NIL && groupClause_has_nonconst(root, groupClause))
			continue;
		if (!compile_key(c, sgc, e, &s->key[s->nkeys]))
			return;
		combo_set_key(c, &s->key[s->nkeys], s->nkeys);
		ow.keys = lappend(ow.keys, e);
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) e, list_length(scan_tlist) + 1, NULL, false));
		s->nkeys++;
	}
	if (parse->groupingSets != NIL)
		add_sets(parse, s, &scan_tlist);

	/* DISTINCT ON: the other columns are the first row's */
	if (parse->hasDistinctOn)
	{
		if (!distinct_on_more(c, parse, &more) || !first_aggs(c, target, ow.keys, more, &firsts))
			return;
		ow.aggs = firsts;
	}
	else
	{
		/* aggregates, in the output and the HAVING clause */
		foreach_ptr(Node, e, target->exprs)
			out_walker(e, &ow);
	}
	if (gextra && gextra->havingQual)
		out_walker((Node *) gextra->havingQual, &ow);
	if (ow.bad)
		return;
	s->agg = palloc0(Max(list_length(ow.aggs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(Node, a, ow.aggs)
	{
		bool		first = list_member_ptr(firsts, a);

		if (!(IsA(a, GroupingFunc) ? compile_grouping(parse, (GroupingFunc *) a, &s->agg[s->naggs]) :
			  compile_agg(c, (Aggref *) a, &s->agg[s->naggs], s->naggs)))
			return;
		if (first)
		{
			first_mode(&s->agg[s->naggs], list_length(more), lead);
			lead = false;
		}
		scan_tlist = lappend(scan_tlist, makeTargetEntry(first ? (Expr *) linitial_node(TargetEntry, ((Aggref *) a)->args)->expr : (Expr *) a,
														 list_length(scan_tlist) + 1, NULL, false));
		s->naggs++;
	}
	agg_finish(c, s, root, input_rel, output_rel, target, ow.keys, scan_tlist);
}

void
sift_agg_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel,
					 void *extra)
{
	Query	   *parse = root->parse;

	if (stage == UPPERREL_ORDERED)
	{
		topn_paths(root, input_rel, output_rel);
		return;
	}
	if (!sift_enable_agg || parse->commandType != CMD_SELECT || parse->hasTargetSRFs || root->rowMarks != NIL || parse->hasRecursive)
		return;
	/* SELECT DISTINCT of a plain scan is a group-by with no aggregate; over a grouped or windowed query it is not the scan's */
	if (stage == UPPERREL_DISTINCT)
	{
		if (parse->distinctClause != NIL && root->upper_targets[UPPERREL_DISTINCT] != NULL &&
			!(parse->hasAggs || parse->groupClause || parse->groupingSets || parse->hasWindowFuncs || parse->havingQual))
			group_paths(root, input_rel, output_rel, root->upper_targets[UPPERREL_DISTINCT], NULL, parse->distinctClause);
		return;
	}
	if (stage != UPPERREL_GROUP_AGG || parse->hasDistinctOn || !(parse->hasAggs || parse->groupClause || parse->groupingSets))
		return;
	/* a window function is planned above the grouping stage: this stage is the same with or without one */
	if (list_length(parse->rtable) > 0 && parse->jointree->fromlist != NIL && list_length(parse->jointree->fromlist) == 1 &&
		IsA(linitial(parse->jointree->fromlist), RangeTblRef) &&
		rt_fetch(((RangeTblRef *) linitial(parse->jointree->fromlist))->rtindex, parse->rtable)->rtekind == RTE_SUBQUERY)
	{
		if (!parse->hasWindowFuncs)
			sift_agg_two_level(root, input_rel, output_rel, (GroupPathExtraData *) extra);
		return;
	}
	group_paths(root, input_rel, output_rel, output_rel->reltarget, (GroupPathExtraData *) extra, parse->groupClause);
}

/* ---------- top-N: ORDER BY <columns> LIMIT n ---------- */

/* The rows a plan assumes for a LIMIT it cannot know (a parameter of a generic plan): a page of a list */
#define TOPN_ASSUMED_ROWS	1000

/* LIMIT and OFFSET are the same at every execution of the plan: no sub-plan, no executor parameter, no volatile function */
static bool
topn_bound_ok(Node *e)
{
	return e == NULL || (sift_operand_ok(e) && !contain_volatile_functions(e));
}

/* The value the planner expects of a LIMIT or OFFSET (a parameter is a constant of a custom plan); -1 for NULL, `unknown` when it is not a constant */
static int64
topn_value(PlannerInfo *root, Node *e, int64 unknown)
{
	Node	   *v = estimate_expression_value(root, e);

	if (!IsA(v, Const))
		return unknown;
	return ((Const *) v)->constisnull ? -1 : DatumGetInt64(((Const *) v)->constvalue);
}

/*
 * LIMIT and OFFSET of the query: the rows the top-N must produce, as far as the planner knows them; 0 when there is no bound it can use.
 * A parameter of a generic plan is not known: the scan gets its value when it starts, and handles any (agg/topn.h), so the plan assumes
 * a few rows; a value the planner knows, past the cap, keeps the normal plan.
 */
static int64
topn_rows(Query *parse, PlannerInfo *root)
{
	int64		count,
				offset;

	if (parse->limitCount == NULL || parse->limitOption != LIMIT_OPTION_COUNT || !topn_bound_ok(parse->limitCount) || !topn_bound_ok(parse->limitOffset))
		return 0;
	count = topn_value(root, parse->limitCount, TOPN_ASSUMED_ROWS);
	offset = parse->limitOffset ? topn_value(root, parse->limitOffset, 0) : 0;
	if (count < 1 || offset < 0 || count > SIFT_TOPN_MAX_ROWS || offset > SIFT_TOPN_MAX_ROWS)
		return 0;
	return count + offset <= SIFT_TOPN_MAX_ROWS ? count + offset : 0;
}

/* The sort column i of the query as a leaf the top-N can order by, in the direction of its operator */
static bool
topn_key(AggCtx *c, SortGroupClause *sgc, int *leaf, bool *desc, StringInfo txt)
{
	Node	   *e = (Node *) get_sortgroupclause_expr(sgc, c->root->processed_tlist);
	Oid			opfamily,
				opcintype;
#if PG_VERSION_NUM >= 180000
	CompareType strategy;
#define TOPN_DESC	COMPARE_GT
#else
	int16		strategy;
#define TOPN_DESC	BTGreaterStrategyNumber
#endif

	if (!get_ordering_op_properties(sgc->sortop, &opfamily, &opcintype, &strategy))
		return false;
	*leaf = match_leaf(c, normalize_expr(c, e));
	if (*leaf < 0 || !sift_topn_key_ok(leaf_of(c, *leaf)) || opcintype != leaf_of(c, *leaf)->typid ||
		opfamily != get_opclass_family(GetDefaultOpClass(opcintype, BTREE_AM_OID)))
		return false;
	*desc = strategy == TOPN_DESC;
	appendStringInfo(txt, "%s%s%s", txt->len ? ", " : "", leaf_name(c, *leaf), *desc ? " DESC" : "");
	if (sgc->nulls_first != *desc)
		appendStringInfoString(txt, sgc->nulls_first ? " NULLS FIRST" : " NULLS LAST");
	use_leaf(c, *leaf);
	return true;
}

/*
 * A first sort column that follows the physical order of the table (pg_statistic correlation) lets the scan stop at the end its best values
 * are at: it reads a window of superblocks, and more only while the rows it needs are not in it. Otherwise it reads them all.
 */
static double
topn_fraction(AggCtx *c, Node *key, double matches, int64 limit)
{
	Node	   *x = strip_relabel(key);
	HeapTuple	tp;
	AttStatsSlot sslot;
	double		corr = 0,
				nsb = Max(c->stats.nunits, 1.0);

	if (!IsA(x, Var))
		return 1.0;
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(c->heapoid), Int16GetDatum(((Var *) x)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tp))
		return 1.0;
	if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		corr = sslot.nnumbers > 0 ? sslot.numbers[0] : 0;
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	if (fabs(corr) < 0.9)
		return 1.0;
	return Min(1.0, (8.0 + 2.0 * nsb * (double) limit / Max(matches, 1.0)) / nsb);
}

/*
 * The scan tuple of the top-N is the heap columns the output reads, in custom_scan_tlist: their attribute numbers go in the spec, and
 * the node's tlist (PostgreSQL's projection) computes the rest from them.
 */
static bool
topn_columns(AggCtx *c, PathTarget *target, List **scan_tlist, int *attno)
{
	List	   *vars;

	if (contain_volatile_functions((Node *) target->exprs))
		return false;
	vars = pull_var_clause((Node *) target->exprs, PVC_RECURSE_PLACEHOLDERS);
	foreach_node(Var, v, vars)
	{
		if (v->varno != c->relid || v->varlevelsup != 0 || v->varattno <= 0)
			return false;
		if (list_member(*scan_tlist, v) || tlist_member((Expr *) v, *scan_tlist))
			continue;
		if (list_length(*scan_tlist) >= MaxHeapAttributeNumber)
			return false;
		attno[list_length(*scan_tlist)] = v->varattno;
		*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) copyObject(v), list_length(*scan_tlist) + 1, NULL, false));
	}
	return true;
}

void
topn_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	Query	   *parse = root->parse;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	CustomPath *cp;
	StringInfoData txt;
	List	   *scan_tlist = NIL;
	uint64		tlo,
				thi;
	int			leaf[SIFT_TOPN_MAX_KEYS];
	bool		desc[SIFT_TOPN_MAX_KEYS],
				nulls_first[SIFT_TOPN_MAX_KEYS];
	int		   *attno;
	int64		rows = topn_rows(parse, root);
	int			nkey = list_length(parse->sortClause);
	PathTarget *target = root->upper_targets[UPPERREL_ORDERED];

	if (!sift_enable_agg || !sift_enable_topn || rows == 0 || target == NULL || parse->commandType != CMD_SELECT || nkey == 0 || nkey > SIFT_TOPN_MAX_KEYS ||
		root->sort_pathkeys == NIL || parse->hasAggs || parse->groupClause || parse->groupingSets || parse->havingQual || parse->hasWindowFuncs ||
		parse->hasTargetSRFs || parse->distinctClause || parse->setOperations || parse->hasSubLinks || parse->hasRecursive || root->rowMarks != NIL)
		return;
	c->joinless = true;
	if (!agg_base(c, root, input_rel, &fs) || c->nparts > 0 || c->joins != NIL || c->func_rti != 0 || fs.lefts != NIL ||
		!(OidIsValid(c->indexoid) ? sift_colsrc_can_tids(get_rel_relam(c->indexoid), true) : sift_colsrc_can_tids(get_rel_relam(c->heapoid), false)))
		return;
	s = agg_spec_new(c);
	c->search = sift_search_extract(root, c->rel, &fs.quals, c->indexoid, c->heapoid, true);
	if (!agg_quals(c, s, &fs) || s->npost > 0 || sift_aggspec_tid_range(s, &tlo, &thi))
		return;
	initStringInfo(&txt);
	foreach_node(SortGroupClause, sgc, parse->sortClause)
	{
		int			j = foreach_current_index(sgc);

		if (!topn_key(c, sgc, &leaf[j], &desc[j], &txt))
			return;
		nulls_first[j] = sgc->nulls_first;
	}
	attno = palloc(MaxHeapAttributeNumber * sizeof(int));
	if (!topn_columns(c, target, &scan_tlist, attno) || !spec_finish(c, s, root, input_rel, NIL))
		return;
	c->topn_frac = topn_fraction(c, (Node *) get_sortgroupclause_expr(linitial_node(SortGroupClause, parse->sortClause), root->processed_tlist),
								 input_rel->rows, rows);
	s->topn = sift_topn_spec(nkey, leaf, desc, nulls_first, list_length(scan_tlist), attno, copyObject(parse->limitCount), copyObject(parse->limitOffset));
	record_deps(root, parse->limitCount);
	record_deps(root, parse->limitOffset);
	s->desc_topn = txt.data;

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = target;
	cp->path.parallel_safe = false;
	cp->path.pathkeys = root->sort_pathkeys;
	cp->methods = &agg_path_methods;
	agg_cost(c, s, input_rel, &cp->path, Min((double) rows, Max(input_rel->rows, 1.0)));
	cp->custom_private = list_make2(sift_aggspec_pack(s), scan_tlist);
	add_path(output_rel, &cp->path);
}

/* ---------- two level: an outer aggregate over aggregates grouped by row ---------- */

#define AGG_MAX_ARMS	4

typedef struct CteSub
{
	Index		rti;			/* 0: the arm reads the table itself */
	List	   *tlist;			/* the target list of the common table expression */
} CteSub;

static Node *
cte_sub_mutator(Node *n, CteSub *s)
{
	if (n == NULL)
		return NULL;
	if (s->rti != 0 && IsA(n, Var) && ((Var *) n)->varno == s->rti && ((Var *) n)->varlevelsup == 0)
	{
		TargetEntry *te = get_tle_by_resno(s->tlist, ((Var *) n)->varattno);

		return te ? (Node *) copyObject(te->expr) : n;
	}
	return expression_tree_mutator(n, cte_sub_mutator, s);
}

/* what an arm (a subquery of the outer query) is, in the terms of the table it reads */
typedef struct ArmIn
{
	CteSub		sub;
	List	   *cols;			/* the expressions of its output columns */
	List	   *group;			/* its group expressions */
	List	   *gsgc;			/* their SortGroupClauses */
	List	   *used;			/* the group expressions that are an outer key */
	List	   *quals;			/* its WHERE, when it reads a common table expression */
	Node	   *id;				/* the group expression that is unique per row */
	SiftAggArm *out;
} ArmIn;

/* the root of the common table expression a CTE scan of `root` reads; NULL if it is not one the scan can look through */
static PlannerInfo *
cte_root(PlannerInfo *root, RangeTblEntry *rte)
{
	PlannerInfo *cteroot = root;
	int			ndx = 0;
	CommonTableExpr *cte = NULL;

	for (Index up = rte->ctelevelsup; up > 0 && cteroot != NULL; up--)
		cteroot = cteroot->parent_root;
	if (cteroot == NULL)
		return NULL;
	foreach_node(CommonTableExpr, x, cteroot->parse->cteList)
	{
		if (strcmp(x->ctename, rte->ctename) == 0)
		{
			cte = x;
			break;
		}
		ndx++;
	}
	if (cte == NULL || cte->cterecursive || cte->search_clause || cte->cycle_clause || ndx >= list_length(cteroot->cte_plan_ids))
		return NULL;
	return list_nth(root->glob->subroots, list_nth_int(cteroot->cte_plan_ids, ndx) - 1);
}

/* the subqueries of a UNION ALL: a member that is itself a UNION ALL (a subquery of arms in an arm's place) gives its own */
static bool
append_arms(PlannerInfo *root, Index parent, List **arms)
{
	foreach_node(AppendRelInfo, ai, root->append_rel_list)
	{
		RelOptInfo *rel;
		int			i = 0;

		if (ai->parent_relid != parent)
			continue;
		rel = root->simple_rel_array[ai->child_relid];
		if (rel == NULL)
			return false;
		foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
			if (!rinfo->pseudoconstant)
				return false;
		foreach_ptr(Node, tv, ai->translated_vars)
			if (tv == NULL || !IsA(tv, Var) || ((Var *) tv)->varno != ai->child_relid || ((Var *) tv)->varattno != ++i)
				return false;
		if (rel->subroot != NULL)
			*arms = lappend(*arms, rel->subroot);
		else if (!rt_fetch(ai->child_relid, root->parse->rtable)->inh || !append_arms(root, ai->child_relid, arms))
			return false;
	}
	return true;
}

/*
 * The arms of the outer query's one FROM item: the subqueries of a UNION ALL (appendrel) or the subquery itself. The quals of the
 * outer query that PostgreSQL pushed into the arms (to their WHERE, or HAVING) are theirs now: an arm whose rel still has a qual
 * (one it did not push: volatile, a one-time filter) is not one.
 */
static List *
outer_arms(PlannerInfo *root, Index srti, List **gates)
{
	List	   *arms = NIL;
	RelOptInfo *parent = root->simple_rel_array[srti];

	if (parent == NULL)
		return NIL;
	/* the conditions without a column stay above the arms as PostgreSQL's one-time filter: the scan takes them as its own gate */
	foreach_node(RestrictInfo, rinfo, parent->baserestrictinfo)
		if (rinfo->pseudoconstant)
			*gates = lappend(*gates, rinfo->clause);
		else if (!rt_fetch(srti, root->parse->rtable)->inh)
			return NIL;
	if (!rt_fetch(srti, root->parse->rtable)->inh)
		return parent->subroot ? list_make1(parent->subroot) : NIL;
	return append_arms(root, srti, &arms) ? arms : NIL;
}

/*
 * The table an arm reads: through its CTE scan to the common table expression's root, or the arm's own root (its FROM is the scan itself:
 * a table with its unnests and lookups, which is what an arm over a common table expression that PostgreSQL inlined looks like); NULL if
 * the arm is not a simple aggregate
 */
static PlannerInfo *
arm_base(PlannerInfo *arm, ArmIn *in)
{
	Query	   *q = arm->parse;
	RangeTblEntry *rte;

	if (!q->hasAggs || q->groupClause == NIL || q->groupingSets || q->hasWindowFuncs || q->hasTargetSRFs ||
		q->distinctClause || q->sortClause || q->limitCount || q->limitOffset || q->groupDistinct || q->hasForUpdate ||
		q->setOperations || q->cteList != NIL)
		return NULL;
	in->sub.rti = 0;
	if (list_length(q->jointree->fromlist) != 1 || !IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return arm;
	rte = rt_fetch(((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex, q->rtable);
	if (rte->rtekind != RTE_CTE)
		return arm;
	in->sub.rti = ((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex;
	return cte_root(arm, rte);
}

/*
 * Two arms that read their own copy of one scan (a common table expression that PostgreSQL inlined into each): the same tables and the same
 * WHERE. The entries of pulled up subqueries and of the grouping belong to their arm, and the column names a plan prunes differ: none counts.
 */
static bool
arm_private_rte(RTEKind kind)
{
#if PG_VERSION_NUM >= 180000
	if (kind == RTE_GROUP)
		return true;
#endif
	return kind == RTE_SUBQUERY;
}

static bool
same_scan(PlannerInfo *a, PlannerInfo *b)
{
	ListCell   *la,
			   *lb;

	if (list_length(a->parse->rtable) != list_length(b->parse->rtable) || !equal(a->parse->jointree, b->parse->jointree))
		return false;
	forboth(la, a->parse->rtable, lb, b->parse->rtable)
	{
		RangeTblEntry same = *(RangeTblEntry *) lfirst(la),
				   *rb = lfirst(lb);

		same.eref = rb->eref;
		if (same.rtekind != rb->rtekind || (!arm_private_rte(same.rtekind) && !equal(&same, rb)))
			return false;
	}
	return true;
}

static bool
null_const(Node *e)
{
	return IsA(e, Const) && ((Const *) e)->constisnull;
}

/* `operand op constant` (either side) on an int8 or float8 operand: the operand, the comparison and the constant in *out */
static bool
cond_split(Node *q, Node **operand, SiftAggCond *out)
{
	Oid			lt,
				rt,
				vtype;
	OpExpr	   *op;
	Node	   *l,
			   *r;
	Const	   *k;
	int			cmp;

	if (!IsA(q, OpExpr) || list_length(((OpExpr *) q)->args) != 2)
		return false;
	op = (OpExpr *) q;
	if ((cmp = builtin_cmp(op->opno, &lt, &rt)) < 0)
		return false;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if (IsA(r, Const))
	{
		*operand = l;
		k = (Const *) r;
	}
	else if (IsA(l, Const))
	{
		*operand = r;
		k = (Const *) l;
		cmp = commute(cmp);
	}
	else
		return false;
	vtype = exprType(*operand);
	out->op = cmp;
	if (k->constisnull)
		return false;
	if (vtype == FLOAT8OID && k->consttype == FLOAT8OID)
	{
		out->isint = false;
		out->cd = DatumGetFloat8(k->constvalue);
	}
	else if (vtype == INT8OID && (k->consttype == INT8OID || k->consttype == INT4OID || k->consttype == INT2OID))
	{
		out->isint = true;
		out->ci = k->consttype == INT8OID ? DatumGetInt64(k->constvalue) : k->consttype == INT4OID ?
			DatumGetInt32(k->constvalue) : DatumGetInt16(k->constvalue);
	}
	else
		return false;
	return true;
}

/* `column op constant` on an output column of the arms: the column in out->col */
static bool
outer_cond(Node *q, Index srti, SiftAggCond *out)
{
	Node	   *x;

	if (!cond_split(q, &x, out) || !IsA(x, Var) || ((Var *) x)->varno != srti || ((Var *) x)->varlevelsup != 0)
		return false;
	out->col = ((Var *) x)->varattno - 1;
	return true;
}

/* the inner aggregate e of the arm, added to its inner aggregates: its index; -1 if the arm cannot give it */
static int
arm_inner_agg(AggCtx *c, ArmIn *in, Node *e, Oid want_type)
{
	SiftAggArm *a = in->out;
	SiftAggAgg *ia = &a->inner[a->ninner];

	if (!IsA(e, Aggref) || exprType(e) != want_type || !compile_agg(c, (Aggref *) e, ia, -1))
		return -1;
	switch (ia->kind)
	{
		case SAA_COUNT_STAR:
		case SAA_COUNT:
		case SAA_SUM_INT:
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			break;
		case SAA_X_SUM:
		case SAA_X_AVG:
			/* the expression is evaluated for every tuple of the scan: an arm's own WHERE or a FILTER would keep it off the tuples they drop */
			if (ia->restype != FLOAT8OID || in->quals != NIL || ia->filter != NULL)
				return -1;
			break;
		default:
			return -1;
	}
	return a->ninner++;
}

/* the inner aggregate that is the arm's column `col`, compiled once: its index in the arm; -1 if the arm cannot give it */
static int
arm_inner(AggCtx *c, ArmIn *in, int col, Oid want_type)
{
	SiftAggArm *a = in->out;

	if (a->colsrc[col] < 0)
		a->colsrc[col] = arm_inner_agg(c, in, list_nth(in->cols, col), want_type);
	return a->colsrc[col];
}

/* a conjunct of the arm's HAVING: `inner aggregate op constant` (in terms of the scanned table), with the aggregate's index as cond.col */
static bool
arm_having(AggCtx *c, ArmIn *in, Node *q, SiftAggCond *out)
{
	Node	   *x;

	return cond_split(q, &x, out) && (out->col = arm_inner_agg(c, in, x, exprType(x))) >= 0;
}

/* the conditions of the outer query without a column, which PostgreSQL left above the arms, as conjuncts of the scan */
static bool
arm_gates(AggCtx *c, SiftAggSpec *s, List *gates)
{
	s->pred = repalloc(s->pred, Max(s->npred + list_length(gates), 1) * sizeof(SiftAggPred *));
	foreach_ptr(Node, q, gates)
	{
		SiftAggPred *p = compile_pred(c, normalize_expr(c, q));

		if (p == NULL || p->kind != SAP_GATE)
			return false;
		s->pred[s->npred++] = p;
		s->desc_quals = psprintf("%s%s%s", s->desc_quals, *s->desc_quals ? " AND " : "", p->desc);
	}
	return true;
}

typedef struct HavingSub
{
	AggCtx	   *c;
	ArmIn	   *in;
	List	   *src;			/* inner aggregate of each Param made */
	bool		bad;
} HavingSub;

/* the inner aggregates of a HAVING conjunct become Params; a column (a group expression) or a volatile function is not allowed */
static Node *
having_mutator(Node *n, HavingSub *h)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Aggref))
	{
		int			ai = h->bad || list_length(h->src) >= SIFT_AGG_MAX_PARAMS ? -1 : arm_inner_agg(h->c, h->in, n, exprType(n));
		Param	   *p = makeNode(Param);

		if (ai < 0)
			h->bad = true;
		p->paramkind = PARAM_EXEC;
		p->paramid = list_length(h->src);
		p->paramtype = exprType(n);
		p->paramtypmod = -1;
		p->paramcollid = InvalidOid;
		p->location = -1;
		h->src = lappend_int(h->src, ai);
		return (Node *) p;
	}
	if (IsA(n, Var) || (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC) || IsA(n, SubPlan) || IsA(n, WindowFunc) || IsA(n, GroupingFunc))
		h->bad = true;
	return expression_tree_mutator(n, having_mutator, h);
}

/* the conjuncts of the arm's HAVING that are not `inner aggregate op constant`: PostgreSQL evaluates them on the values of each cell */
static bool
arm_having_expr(AggCtx *c, ArmIn *in, List *quals)
{
	HavingSub	h = {c, in, NIL, false};
	SiftAggArm *a = in->out;

	if (contain_volatile_functions((Node *) quals))
		return false;
	a->hexpr = having_mutator((Node *) make_ands_explicit(quals), &h);
	a->nhp = list_length(h.src);
	a->hsrc = palloc(Max(a->nhp, 1) * sizeof(int));
	foreach_int(ai, h.src)
		a->hsrc[foreach_current_index(ai)] = ai;
	return !h.bad;
}

static bool
count_aggs_walker(Node *n, int *count)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
		(*count)++;
	return expression_tree_walker(n, count_aggs_walker, count);
}

/* The outer aggregate: avg/sum of a float8 or int8 column of the arms, count(DISTINCT id), count(*), with a FILTER of comparisons of columns */
static bool
outer_agg(AggCtx *c, SiftAggSpec *s, Aggref *oa, Index srti, ArmIn *arms, int narms, SiftAggAgg *out)
{
	Oid			fid = oa->aggfnoid;
	Node	   *arg = NULL;
	Var		   *v = NULL;
	List	   *conds = oa->aggfilter ? make_ands_implicit(oa->aggfilter) : NIL;
	int			nc = 0;

	if (oa->aggkind != AGGKIND_NORMAL || oa->aggorder != NIL || oa->agglevelsup != 0 || oa->aggvariadic || contain_volatile_functions((Node *) oa))
		return false;
	out->leaf = -1;
	out->col = -1;
	out->restype = oa->aggtype;
	out->cond = palloc0(Max(list_length(conds), 1) * sizeof(SiftAggCond));
	foreach_ptr(Node, q, conds)
	{
		if (!outer_cond(q, srti, &out->cond[nc++]))
			return false;
	}
	out->ncond = nc;
	for (int i = 0; i < nc; i++)
		if (out->cond[i].col < 0 || out->cond[i].col >= list_length(arms[0].cols))
			return false;
	if (fid != F_COUNT_)
	{
		if (list_length(oa->args) != 1)
			return false;
		arg = strip_relabel((Node *) linitial_node(TargetEntry, oa->args)->expr);
		if (!IsA(arg, Var) || ((Var *) arg)->varno != srti || ((Var *) arg)->varlevelsup != 0)
			return false;
		v = (Var *) arg;
		out->col = v->varattno - 1;
		if (out->col < 0 || out->col >= list_length(arms[0].cols))
			return false;
	}
	if (fid == F_COUNT_ && oa->aggdistinct == NIL)
		out->kind = SAA_O_COUNT_STAR;
	else if (fid == F_COUNT_ANY && oa->aggdistinct != NIL)
	{
		out->kind = SAA_O_COUNT_ROWS;
		for (int k = 0; k < narms; k++)
			if (!equal(list_nth(arms[k].cols, out->col), arms[k].id))
				return false;
	}
	else if (oa->aggdistinct == NIL && (fid == F_AVG_FLOAT8 || fid == F_SUM_FLOAT8) && v && v->vartype == FLOAT8OID)
		out->kind = fid == F_AVG_FLOAT8 ? SAA_O_AVG_F64 : SAA_O_SUM_F64;
	else if (oa->aggdistinct == NIL && fid == F_SUM_INT8 && v && v->vartype == INT8OID)
		out->kind = SAA_O_SUM_I64;
	else
		return false;
	for (int k = 0; k < narms; k++)
	{
		if (out->kind == SAA_O_AVG_F64 || out->kind == SAA_O_SUM_F64 || out->kind == SAA_O_SUM_I64)
			if (arm_inner(c, &arms[k], out->col, v->vartype) < 0)
				return false;
		for (int i = 0; i < out->ncond; i++)
			if (arm_inner(c, &arms[k], out->cond[i].col, out->cond[i].isint ? INT8OID : FLOAT8OID) < 0)
				return false;
	}
	out->desc = psprintf("%s", out->kind == SAA_O_COUNT_STAR ? "count(*)" : out->kind == SAA_O_COUNT_ROWS ? "count(DISTINCT row)" :
						 out->kind == SAA_O_AVG_F64 ? "avg(inner)" : out->kind == SAA_O_SUM_F64 ? "sum(inner)" : "sum(inner int8)");
	return true;
}

/*
 * A cell is an inner group, the share of one row in one group of an arm. When an arm groups by more than the outer keys and its id, one
 * row has several cells in an outer group, so the group expressions that are no outer key are keys of the cells: one key each, shared by
 * the arms that have an equal expression, after the outer keys and before the number of the set. They name no group (a group has id 0
 * for them, and the scan emits NULL).
 */
static bool
hidden_keys(AggCtx *c, SiftAggSpec *s, ArmIn *arms, int narms, List **scan_tlist, bool sets)
{
	int			nouter = s->nkeys;
	List	   *defs = NIL;

	for (int k = 0; k < narms; k++)
		for (int i = 0; i < list_length(arms[k].group); i++)
		{
			Node	   *g = list_nth(arms[k].group, i);
			SortGroupClause *sgc = list_nth_node(SortGroupClause, arms[k].gsgc, i);
			int			pos;
			SiftAggKey *key;

			if (equal(g, arms[k].id) || list_member(arms[k].used, g))
				continue;
			pos = -1;
			foreach_ptr(Node, d, defs)
				if (equal(d, g))
					pos = foreach_current_index(d);
			if (pos < 0)
			{
				if (s->nkeys + sets >= SIFT_AGG_MAX_KEYS)
					return false;
				key = &s->key[s->nkeys];
				if (!compile_key(c, sgc, g, key))
					return false;
				combo_set_key(c, key, s->nkeys);
				*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) makeNullConst(key->typid, key->typmod, key->collid),
																   list_length(*scan_tlist) + 1, NULL, false));
				defs = lappend(defs, g);
				pos = s->nkeys++ - nouter;
			}
			arms[k].out->hmask |= (uint32) 1 << (nouter + pos);
		}
	s->nhidden = s->nkeys - nouter;
	return true;
}

/*
 * The outer query aggregates the rows of its FROM item, the arms: a UNION ALL of aggregates (or one) of the same scan, each grouped by
 * a unique row id and by outer keys (a NULL constant where an arm has none of them): see SiftAggArm. The arms are read from their
 * planned roots, so that group expressions and constants are in the planner's final form; an arm that reads a common table
 * expression is looked through to it.
 */
void
sift_agg_two_level(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *gextra)
{
	Query	   *parse = root->parse;
	Index		srti = ((RangeTblRef *) linitial(parse->jointree->fromlist))->rtindex;
	List	   *roots;
	ArmIn		arms[AGG_MAX_ARMS] = {0};
	int			narms = 0;
	PlannerInfo *base = NULL;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	OutWalk		ow = {NIL, NIL, false};
	List	   *scan_tlist = NIL;
	int			okey[SIFT_AGG_MAX_KEYS];
	int			ncols = 0;
	List	   *gates = NIL;
	int			nkeyslots = list_length(parse->groupClause) + 1;

	if (list_length(parse->groupClause) > SIFT_AGG_MAX_KEYS - 1)
		return;
	roots = outer_arms(root, srti, &gates);
	if (roots == NIL || list_length(roots) > AGG_MAX_ARMS)
		return;
	foreach_ptr(PlannerInfo, ar, roots)
	{
		PlannerInfo *b = arm_base(ar, &arms[narms]);

		if (b == NULL || (base != NULL && b != base && !(b == ar && same_scan(base, b))))
			return;
		if (base == NULL)
			base = b;
		arms[narms].sub.tlist = b->parse->targetList;
		narms++;
	}
	if (!agg_base(c, base, NULL, &fs) || c->nparts > 0)
		return;
	s = agg_spec_new(c);
	if (!agg_quals(c, s, &fs) || !arm_gates(c, s, gates))
		return;

	/* the arms in the terms of the scanned table */
	for (int k = 0; k < narms; k++)
	{
		Query	   *aq = ((PlannerInfo *) list_nth(roots, k))->parse;
		ArmIn	   *in = &arms[k];
		int			nid = 0,
					nagg;
		List	   *rest = NIL;

		foreach_node(TargetEntry, te, aq->targetList)
			if (!te->resjunk)
				in->cols = lappend(in->cols, normalize_expr(c, cte_sub_mutator((Node *) te->expr, &in->sub)));
		if (k == 0)
			ncols = list_length(in->cols);
		else if (ncols != list_length(in->cols))
			return;
		foreach_node(SortGroupClause, sgc, aq->groupClause)
		{
			Node	   *g = normalize_expr(c, cte_sub_mutator((Node *) get_sortgroupclause_expr(sgc, aq->targetList), &in->sub));

			in->group = lappend(in->group, g);
			in->gsgc = lappend(in->gsgc, sgc);
			if (unique_row_column(c, g))
			{
				in->id = g;
				nid++;
			}
		}
		if (nid != 1)
			return;
		if (in->sub.rti != 0 && aq->jointree->quals != NULL)
			foreach_ptr(Node, q, (List *) aq->jointree->quals)
				in->quals = lappend(in->quals, normalize_expr(c, cte_sub_mutator(q, &in->sub)));
		in->out = palloc0(sizeof(SiftAggArm));
		in->out->colsrc = palloc(Max(ncols, 1) * sizeof(int));
		for (int i = 0; i < ncols; i++)
			in->out->colsrc[i] = -1;
		nagg = ncols;
		count_aggs_walker(aq->havingQual, &nagg);
		in->out->inner = palloc0((nagg + 1) * sizeof(SiftAggAgg));
		in->out->having = palloc0(Max(list_length((List *) aq->havingQual), 1) * sizeof(SiftAggCond));
		foreach_ptr(Node, q, (List *) aq->havingQual)
		{
			Node	   *hq = normalize_expr(c, cte_sub_mutator(q, &in->sub));

			if (arm_having(c, in, hq, &in->out->having[in->out->nhaving]))
				in->out->nhaving++;
			else
				rest = lappend(rest, hq);
		}
		if (rest != NIL && !arm_having_expr(c, in, rest))
			return;
		in->out->filter = palloc(Max(list_length(in->quals), 1) * sizeof(SiftAggPred *));
		foreach_ptr(Node, q, in->quals)
		{
			SiftAggPred *p = compile_combo_pred(c, q);

			if (p == NULL || unsupported_clause(c, q))
				return;
			in->out->filter[in->out->nfilter++] = p;
		}
	}

	/* the outer keys: columns that every arm groups by or has as NULL */
	for (int k = 0; k < narms; k++)
		nkeyslots += list_length(arms[k].group);
	s->key = palloc0(nkeyslots * sizeof(SiftAggKey));
	foreach_node(SortGroupClause, sgc, parse->groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);
		Node	   *def = NULL;

		if (!IsA(e, Var) || ((Var *) e)->varno != srti || ((Var *) e)->varlevelsup != 0 || ((Var *) e)->varattno > ncols)
			return;
		okey[s->nkeys] = ((Var *) e)->varattno - 1;
		for (int k = 0; k < narms; k++)
		{
			Node	   *x = list_nth(arms[k].cols, okey[s->nkeys]);

			if (null_const(x))
				continue;
			if (!list_member(arms[k].group, x) || equal(x, arms[k].id) || (def != NULL && !equal(x, def)))
				return;
			def = x;
			arms[k].used = lappend(arms[k].used, x);
			arms[k].out->keymask |= (uint32) 1 << s->nkeys;
		}
		if (def == NULL || !compile_key(c, sgc, def, &s->key[s->nkeys]))
			return;
		combo_set_key(c, &s->key[s->nkeys], s->nkeys);
		ow.keys = lappend(ow.keys, e);
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) e, list_length(scan_tlist) + 1, NULL, false));
		s->nkeys++;
	}
	if (!hidden_keys(c, s, arms, narms, &scan_tlist, parse->groupingSets != NIL))
		return;
	if (parse->groupingSets != NIL)
		add_sets(parse, s, &scan_tlist);

	/* the outer aggregates */
	foreach_ptr(Node, e, output_rel->reltarget->exprs)
		out_walker(e, &ow);
	if (gextra && gextra->havingQual)
		out_walker((Node *) gextra->havingQual, &ow);
	if (ow.bad)
		return;
	s->agg = palloc0(Max(list_length(ow.aggs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(Node, a, ow.aggs)
	{
		if (!(IsA(a, GroupingFunc) ? compile_grouping(parse, (GroupingFunc *) a, &s->agg[s->naggs]) :
			  IsA(a, Aggref) && outer_agg(c, s, (Aggref *) a, srti, arms, narms, &s->agg[s->naggs])))
			return;
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) a, list_length(scan_tlist) + 1, NULL, false));
		s->naggs++;
	}
	s->narms = narms;
	s->arm = palloc0(narms * sizeof(SiftAggArm));
	for (int k = 0; k < narms; k++)
	{
		s->arm[k] = *arms[k].out;
		s->arm[k].ncol = ncols;
	}
	agg_finish(c, s, root, input_rel, output_rel, output_rel->reltarget, ow.keys, scan_tlist);
}

/* ---------- plan ---------- */

/* the native plans of the correlated children, made now that the plan has its place in the statement; true when the spec holds their IDs */
static bool
plan_correlated_children(PlannerInfo *root, CustomPath *best, SiftAggSpec *spec)
{
	bool		planned = false;

	for (int i = 0; i < spec->njoin; i++)
		if (spec->join[i].params != NIL)
		{
			Path	   *subpath = list_nth(((SiftAggPath *) best)->correlated, i);
			PlannerInfo *subroot = list_nth(((SiftAggPath *) best)->subroots, i);
			Plan       *plan = create_plan(subroot, subpath);

			root->glob->subplans = lappend(root->glob->subplans, plan);
			root->glob->subpaths = lappend(root->glob->subpaths, subpath);
			root->glob->subroots = lappend(root->glob->subroots, subroot);
			spec->join[i].subplan = list_length(root->glob->subplans);
			planned = true;
		}
	return planned;
}

static Plan *
agg_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	List	   *pack = linitial(best->custom_private);
	List	   *scan_tlist = lsecond(best->custom_private);
	SiftAggSpec *spec = sift_aggspec_unpack(pack);
	GroupPathExtraData *gextra = NULL;

	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->custom_plans = custom_plans;
	cs->custom_scan_tlist = copyObject(scan_tlist);
	cs->custom_private = plan_correlated_children(root, best, spec) ? sift_aggspec_pack(spec) : pack;
	cs->methods = &sift_agg_plan_methods;
	if (root->hasHavingQual && root->parse->havingQual)
		cs->scan.plan.qual = (List *) copyObject(root->parse->havingQual);
	(void) gextra;
	if (OidIsValid(spec->indexoid))
		root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->indexoid);
	for (int i = 0; i < spec->nparts; i++)
		root->glob->relationOids = lappend_oid(lappend_oid(root->glob->relationOids, spec->partheap[i]), spec->partindex[i]);
	for (int i = 0; i < spec->njoin; i++)
		if (OidIsValid(spec->join[i].relid))
			root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->join[i].relid);
	return &cs->scan.plan;
}

const CustomPathMethods agg_path_methods = {"SiftAgg", agg_plan_path, NULL};

const CustomScanMethods sift_agg_plan_methods = {"SiftAgg", sift_agg_create_state};
