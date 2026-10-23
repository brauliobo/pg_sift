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
#include "executor/nodeHash.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
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
 * The executor's cost per value in units of cpu_operator_cost (about 3 ns: docs/COL-PLAN.md, "Cost", fitted on whole table runs of
 * one to three keys and aggregates), and the elements per row of an array whose statistics are missing.
 */
#define AGG_SCAN_LEAF		0.7		/* a leaf read (decode, null flags) */
#define AGG_SET_USEFUL		0.85	/* the share of the units a leaf's small-set summaries must keep at most, for the cost to count the units they skip */
#define AGG_SPARSE_MAX		0.125	/* the share of the rows a unit keeps up to which its leaves are decoded at the kept rows only */
#define AGG_SPARSE_LEAF		1.5		/* a leaf read at the kept rows costs this many times the share of the rows it reads (the positions are gathered) */
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
#define AGG_SCAN_SPEED		0.2		/* the share of the operators above the scan really takes: the shapes it runs alone (no lookup tables) take 0.4 to 0.9 ns for an operator counted at 3 ns */
#define AGG_UNIT_COST		200.0
#define AGG_PREAGG_GROUP	25.0	/* a group of a unit's histogram folded into the table: its key id and the aggregates (about 80 ns) */
#define AGG_START_COST		210000.0	/* opening the source, the lookup tables and the first units: 1 to 1.5 ms measured on a scan pruned to a few units, charged 0.6 ms because the arms of a pass open them once, which a selective btree plan does not pay */
#define AGG_PART_PAIR		4.0		/* times the square of the partitions the scan reads: opening them costs the scan 0.75 ms at 200, 9 ms at 800, 27 ms at 1,600 and 111 ms at 3,200 more than the normal plan's Append (11 ns for each pair, measured on partitions of a few rows) */
#define AGG_ARM_TUPLE		150.0	/* a tuple through an arm of a two level query: its cell started or added to, and flushed into the outer groups when the row ends (35 ns measured) */
#define AGG_ARM_HIDDEN		330.0	/* ... in an arm with hidden keys: the cells of the row are numbered in a table of their own (70 ns) */
#define AGG_MERGE_GROUP		8.0		/* a group of a partial table of a worker: dumped, sent, merged */
#define AGG_ORDERED_MERGE	1.5		/* a value kept for an ordered-set aggregate, as the leader merges the histograms of the workers (the distinct pairs, at most one per value) */
#define AGG_SHARE_ROW		1.0		/* a row of a lookup table written to the shared memory by the leader and mapped by each worker: a quarter of the cost of reading it */
#define AGG_KEY_SKEW_FLOOR	2.0		/* the rows a key owns beyond the mean's double are skew: a statistics sample and a mild unevenness of a column are not */
#define AGG_SETUP_FACTOR	22.0	/* times parallel_setup_cost: starting the workers and opening the source in each takes about 8 ms, which in this path's estimates (a unit is worth about 0.5 us there, the unnest excess below included) is 22 times the default 1000 */
#define AGG_DEFAULT_ELEMS	3.0
#define AGG_RECHECK_FRAC	0.02	/* share of the rows of a flagged unit the source reads again from the heap when no sample of the unit's overflow slots says (statistics: ovf_frac) */
#define AGG_RECHECK_ROW		500.0	/* a row read again: the heap fetch by TID, the document walked again and the unit it makes, in the operators AGG_SCAN_SPEED scales: about 1.6 microseconds a row, which the normal plan's cost prices at 0.25: the two plans take the same time at about 14% of the rows read again */
#define AGG_VET_PAGE		400.0	/* a heap page the check reads from shared buffers: the buffer lookup, its lock and the line pointers (1 to 1.5 us measured on 1,000 to 24,000 pages, 185 rows or 8 to a page), in the operators AGG_SCAN_SPEED scales */
#define AGG_VET_OS_PAGE		1000.0	/* ... from the cache of the operating system: the read call and the copy of the page (3.5 us measured, a heap of 185 MB under 16 MB of buffers) */
#define AGG_VET_ROW			1.2		/* a row of such a page tested against the snapshot (4 ns measured at 185 rows to a page) */
#define AGG_WIDE_ROW		3.0		/* a row of a wide leaf: its lanes decoded, the hash probe for its entry in the unit's dictionary */
#define AGG_WIDE_ENTRY		12.0	/* a new entry of it (a numeric built, about 100 ns), per distinct value */
#define AGG_WIDE_IMAGE		300.0	/* ... of a numeric stored as an image (unconstrained, or past 38 digits): the image is spelled as text and read back by numeric_in, 0.75 us measured */
#define AGG_DIST_CHUNK		400.0	/* a chunk of the bitmap of a count(DISTINCT) set allocated and zeroed for a group: 0.4 to 1.1 us measured (1,000 to 4,907 groups of 2 to 200 rows), in the 4 us a unit of a normal plan's cost takes on the same hosts */
#define AGG_DIST_CHUNK_BITS	4096.0	/* the values a chunk holds (agg/dist.c) */
#define AGG_DIST_CHUNK_BYTES	512.0
#define AGG_WIDE_TEST		1.0		/* a conjunct on a wide leaf of images, per dictionary entry: the engine makes its LUT once per segment (about 3 ns an entry) */
#define AGG_INLINE_WIDTH	200		/* average bytes of a text column from which its units store the strings themselves */
#define AGG_INLINE_ROW		56.0	/* a row of such a leaf read: the unit's strings are interned into a dictionary of its own */
#define AGG_INLINE_TEST		3.0		/* a row of it tested on the encoded strings (the prefixes decide most rows) */
#define AGG_KEY_ENTRY		40.0	/* an entry of a unit's dictionary given the id of a group key (the text built, hashed and looked up) in a table of 2,000 ids: 39 ns measured, which the read of the leaf prices; 90 ns at 20,000 ids and 200 ns at 200,000, as the table outgrows the caches (the 0.35 power of the ids over AGG_KEY_IDS): the excess over the first is charged */
#define AGG_KEY_IDS			2000.0
#define AGG_ID_SWEEP		8.0		/* an id of the key table, per spill of the group table: the table is walked and its arrays reset whatever number of its groups hold rows (45 ns measured), in cpu_operator_cost */

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
double
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

/* the distinct values of column col of a joined table by PostgreSQL's statistics, its rows when there are none */
static double
joined_distinct(AggCtx *c, JoinCtx *jc, int col)
{
	RelOptInfo *rel = c->root->simple_rel_array[jc->rti];
	HeapTuple	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(jc->rti, c->root)->relid),
									  Int16GetDatum(jc->j.colatt[col]), BoolGetDatum(false));
	double		nd = 0;

	if (HeapTupleIsValid(tup))
	{
		nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
		ReleaseSysCache(tup);
		nd = nd > 0 ? nd : -nd * rel->tuples;
	}
	return nd > 0 ? Min(nd, Max(rel->rows, 1.0)) : Max(rel->rows, 1.0);
}

/*
 * distinct value combinations of the inputs of a combo among `tuples`: what the statistics say of the columns, the tuples when an input is
 * not one; a column of an earlier lookup has at most as many values as the lookup has rows. The combo of a lookup per element is not
 * evaluated for the tuples but for the combinations of the values of the elements (the index counts those of each leaf) and of the column
 * of the lookup it reads.
 */
static double
combo_entries(AggCtx *c, const SiftAggSpec *s, const SiftAggCombo *cb, double tuples)
{
	List	   *exprs = NIL;
	double		entries = 1.0,
				elem_nd = 1.0;
	Bitmapset  *lookups = NULL;
	bool		scoped = cb->join >= 0 && ((JoinCtx *) list_nth(c->joins, cb->join))->j.scoped;

	for (int p = 0; p < cb->nleaves; p++)
	{
		SiftSrcLeaf *lf;
		Node	   *e;
		double		nd;

		if (cb->leaf[p] < 0)
		{
			foreach_ptr(JoinCtx, jc, c->joins)
				if (jc->combo == cb->refcombo[p])
				{
					/* the columns of one row of a lookup are one value combination of it, however many the combo reads */
					if (scoped)
						entries *= joined_distinct(c, jc, cb->refcol[p]);
					else if (!bms_is_member(jc->combo, lookups))
					{
						double		rows = Max(c->root->simple_rel_array[jc->rti]->rows, 1.0);

						/* a correlated child gives its rows for each distinct input of its own combo */
						entries *= jc->j.params != NIL ? rows * combo_entries(c, s, &s->combo[jc->combo], tuples) : rows;
					}
					lookups = bms_add_member(lookups, jc->combo);
				}
			continue;
		}
		lf = leaf_of(c, cb->leaf[p]);
		/* the values of a member or an element have no statistics: PostgreSQL's default number of distinct values, as for a column it knows nothing of */
		if (lf->parent >= 0 || lf->path != NIL)
		{
			nd = scoped && c->stats.ndistinct[cb->leaf[p]] > 0 ? c->stats.ndistinct[cb->leaf[p]] : DEFAULT_NUM_DISTINCT;
			/* the fields of one element go together (a category and its kind): the combinations are about as many as the values of the widest */
			if (scoped)
				elem_nd = Max(elem_nd, nd);
			else
				entries *= nd;
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
	return Max(Min(entries * elem_nd, tuples), 1.0);
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

				cost += combo_entries(c, s, cb, scan_rows) * Max(candidates, 1.0) * AGG_EVAL_SLOT;
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
		cost += tuples * wide * AGG_WIDE_LEAF + combo_entries(c, s, cb, tuples) * cb->nslots * AGG_EVAL_SLOT;
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
 * The share of the units whose zone a test on an integer leaf overlaps. A test on the fraction f of the leaf's range overlaps the zone
 * of a unit when its range and the zone's meet: f plus the mean width of the zones (the index stores it: a column the units follow
 * has narrow zones, one they do not has the whole range in each). A source without zones has the correlation of the heap column: the
 * units are consecutive in the heap, so their zones are narrow in the share the column is correlated with (corr^2).
 */
static double
zone_overlap(AggCtx *c, int leaf, double f)
{
	double		span = c->stats.zone_span[leaf],
				corr;

	if (span > 0)
		return Min(f + span, 1.0);
	corr = leaf_correlation(c, leaf);
	return f + (1.0 - f) * (1.0 - corr * corr);
}

/* the lookup join whose key is the earlier lookup's column `col` (a later inner or semi join: its rows are the only ones that complete a tuple) */
static const JoinCtx *
dependent_join(AggCtx *c, SiftAggSpec *s, const JoinCtx *jc)
{
	foreach_ptr(JoinCtx, dj, c->joins)
	{
		const SiftAggCombo *cb = &s->combo[dj->combo];
		const SiftAggJoin *j = &s->join[cb->join];

		if (dj == jc || (j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL)
			continue;
		for (int k = 0; k < j->nkey; k++)
		{
			Node	   *kx = j->keyexpr[k];
			int			pos = kx && IsA(kx, Param) ? ((Param *) kx)->paramid : -1;

			if (pos >= 0 && pos < cb->nleaves && cb->leaf[pos] == -1 && cb->refcombo[pos] == jc->combo)
				return dj;
		}
	}
	return NULL;
}

/* the rows of a joined table or plan: what the scan loads */
static double
lookup_rows(AggCtx *c, const JoinCtx *jc)
{
	return Max(jc->cpath ? child_rows(jc->cpath) : c->root->simple_rel_array[jc->rti]->rows, 1.0);
}

/*
 * The units the join key filters leave (join_filters in agg/init.c): an inner or semi join on an integer leaf tests the leaf against
 * the keys of the lookup, narrowed to the rows a later inner or semi join keyed on one of its columns can match (a scope list joined
 * to the table's rows). The keys are that many of the distinct values of the leaf: the share of the leaf's range the zones keep, as for
 * a comparison. *n: the filters the scan applies.
 */
static double
join_key_zone(AggCtx *c, SiftAggSpec *s, int *n)
{
	double		kept = 1.0;

	*n = 0;
	if (!sift_agg_join_filter || s->topn)
		return kept;
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		const SiftAggCombo *cb = &s->combo[jc->combo];
		const SiftAggJoin *j = &s->join[cb->join];
		const JoinCtx *dj = dependent_join(c, s, jc);
		double		keys = Min(lookup_rows(c, jc), dj ? lookup_rows(c, dj) : HUGE_VAL);

		if ((j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || cb->level != 0)
			continue;
		for (int k = 0; k < j->nkey; k++)
		{
			int			leaf = j->keypos[k] < 0 ? -1 : cb->leaf[j->keypos[k]];
			const SiftSrcLeaf *lf = leaf < 0 || leaf >= c->lay->nleaves - c->nvleaf ? NULL : &c->lay->leaf[leaf];
			Node	   *e = lf && lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
			double		nd,
						f;

			if (lf == NULL || lf->kind != SCL_SCALAR || lf->parent >= 0 || lf->vtype != SCV_I32 || e == NULL || !IsA(e, Var) ||
				(nd = column_distinct(c, (Var *) e)) <= 0)
				continue;
			f = Min(keys / nd, 1.0);
			/* a key is a point of the range: it meets the zones that hold it, a zone's width a key */
			kept = Min(kept, c->stats.zone_span[leaf] > 0 ? Min(keys * c->stats.zone_span[leaf], 1.0) : zone_overlap(c, leaf, f));
			(*n)++;
		}
	}
	return kept;
}

/* does PostgreSQL know the values a filter of a table compares (statistics of the column or of an expression index)? Its guess of what the filter keeps is a default otherwise */
static bool
filter_has_stats(PlannerInfo *root, RestrictInfo *ri)
{
	List	   *args = IsA(ri->clause, OpExpr) ? ((OpExpr *) ri->clause)->args :
		IsA(ri->clause, ScalarArrayOpExpr) ? ((ScalarArrayOpExpr *) ri->clause)->args : NIL;
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft,
				known;

	if (list_length(args) != 2 || !get_restriction_variable(root, args, 0, &vardata, &other, &varonleft))
		return true;
	known = HeapTupleIsValid(vardata.statsTuple) || vardata.isunique;
	ReleaseVariableStats(vardata);
	return known;
}

/*
 * Are the rows PostgreSQL expects of a lookup its statistics' count? A table whose filters all have statistics, a VALUES list and a function of
 * constants (unnest of a list) are; a sub-select, a CTE or a function of other arguments is estimated through its own joins and defaults.
 */
static bool
lookup_counted(AggCtx *c, JoinCtx *jc)
{
	RangeTblEntry *rte = planner_rt_fetch(jc->rti, c->root);

	if (rte->rtekind == RTE_VALUES)
		return true;
	if (rte->rtekind == RTE_FUNCTION)
	{
		RangeTblFunction *rtf = list_length(rte->functions) == 1 ? linitial_node(RangeTblFunction, rte->functions) : NULL;

		if (rtf == NULL || !IsA(rtf->funcexpr, FuncExpr))
			return false;
		foreach_ptr(Node, a, ((FuncExpr *) rtf->funcexpr)->args)
			if (!IsA(a, Const))
				return false;
		return true;
	}
	if (rte->rtekind != RTE_RELATION)
		return false;
	foreach_node(RestrictInfo, ri, c->root->simple_rel_array[jc->rti]->baserestrictinfo)
		if (!filter_has_stats(c->root, ri))
			return false;
	return true;
}

/*
 * How many times more rows than PostgreSQL expects the table's own filters keep, for the groups the path reports: a filter without statistics
 * (a jsonb path, an expression) keeps a default guess of the rows, a few hundred of a large table, and the groups of that many rows are
 * fewer than the keys have. The rows are taken as the geometric mean of the guess and the table's rows, as child_rows does.
 */
double
filter_rows_ratio(AggCtx *c)
{
	foreach_node(RestrictInfo, ri, c->rel->baserestrictinfo)
		if (!filter_has_stats(c->root, ri))
			return sqrt(Max(c->rel->tuples / Max(c->rel->rows, 1.0), 1.0));
	return 1.0;
}

/*
 * How many times more rows the key of a row owns than the mean: nd * sum(freq^2) over the values of the column, the frequencies of its
 * most common values by PostgreSQL's statistics and the rest of the rows spread evenly. 1 for a flat column or one without statistics.
 * The mean is what PostgreSQL prices a probe of the key with, and the keys a query passes are the ones with rows to look at.
 */
static double
column_skew(AggCtx *c, Var *v)
{
	HeapTuple	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid), Int16GetDatum(v->varattno), BoolGetDatum(false));
	AttStatsSlot sslot;
	double		nd,
				left,
				sq = 0;

	if (!HeapTupleIsValid(tup))
		return 1.0;
	nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
	nd = nd > 0 ? nd : -nd * c->rel->tuples;
	left = 1.0 - ((Form_pg_statistic) GETSTRUCT(tup))->stanullfrac;
	if (nd >= 1 && get_attstatsslot(&sslot, tup, STATISTIC_KIND_MCV, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		for (int i = 0; i < sslot.nnumbers; i++)
		{
			sq += (double) sslot.numbers[i] * sslot.numbers[i];
			left -= sslot.numbers[i];
		}
		if (left > 0 && nd > sslot.nnumbers)
			sq += left * left / (nd - sslot.nnumbers);
		free_attstatsslot(&sslot);
	}
	else
		nd = 0;
	ReleaseSysCache(tup);
	return Max(nd * sq, 1.0);
}

/*
 * How many times more keys than PostgreSQL expects the scope lists of the query hold, for the normal plan's price. A lookup join on a
 * column of the scanned table (a list of ids, the rows of a sub-select) is counted by PostgreSQL from its own rows, and those are a
 * default guess (a row or two) when the filters of the lookup read a jsonb path, an expression or the output of a plan: the normal plan
 * is then priced for a handful of keys, though the lists the queries pass hold a good share of the table's distinct values. The keys
 * are taken as the geometric mean of the rows PostgreSQL expects and the distinct values of the scanned column (as child_rows does for
 * the rows of a set-returning function). A lookup PostgreSQL has statistics for keeps its estimate of the keys, and the rows of each are priced
 * by the skew of the column (column_skew, sift.agg_key_skew). With groups set (the rows the path reports, spec_finish) the keys are all the
 * distinct values and the skew is not counted: the groups are the keys' own.
 */
double
lookup_set_ratio(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, bool groups)
{
	double		f = 1.0;

	if (!sift_agg_join_filter || s->topn || sift_agg_param_set_size <= 0)
		return f;
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		const SiftAggCombo *cb = &s->combo[jc->combo];
		const SiftAggJoin *j = &s->join[cb->join];
		double		keys = lookup_rows(c, jc),
					nd = 0;
		Var		   *key = NULL;

		if ((j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || cb->level != 0)
			continue;
		for (int k = 0; k < j->nkey; k++)
		{
			int			leaf = j->keypos[k] < 0 ? -1 : cb->leaf[j->keypos[k]];
			const SiftSrcLeaf *lf = leaf < 0 || leaf >= c->lay->nleaves - c->nvleaf ? NULL : &c->lay->leaf[leaf];
			Node	   *e = lf && lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
			double		d;

			if (lf != NULL && lf->kind == SCL_SCALAR && lf->parent < 0 && e != NULL && IsA(e, Var) && (d = column_distinct(c, (Var *) e)) > nd)
			{
				key = (Var *) e;
				nd = d;
			}
		}
		if (key == NULL)
			continue;
		/* a counted lookup's keys are as many as PostgreSQL says, but each has the rows of the keys a query passes, not the mean's */
		if (lookup_counted(c, jc))
		{
			if (sift_agg_key_skew && !groups)
				f *= sqrt(Max(column_skew(c, key) / AGG_KEY_SKEW_FLOOR, 1.0));
		}
		else if (nd > keys)
			f *= groups ? nd / keys : sqrt(nd / keys);
	}
	/* the normal plan reads no more than the rows the table's own filters keep, whatever the keys */
	return Min(f, Max(c->rel->rows / Max(input_rel->rows, 1.0), 1.0));
}

/*
 * The share of the units the zone tests of p leave (the scan tests each disjunct of an OR on its own, init_filter.c): a comparison keeps the
 * zones its selectivity overlaps, an AND the most selective of its conjuncts' share, an OR the sum of its disjuncts' (1: none refuses a unit).
 */
static double
pred_zone_kept(AggCtx *c, const SiftAggPred *p)
{
	double		kept = 1.0;

	if (p->kind == SAP_AND)
		for (int i = 0; i < p->nchild; i++)
			kept = Min(kept, pred_zone_kept(c, p->child[i]));
	else if (p->kind == SAP_OR)
	{
		kept = 0.0;
		for (int i = 0; i < p->nchild; i++)
			kept += pred_zone_kept(c, p->child[i]);
		kept = Min(kept, 1.0);
	}
	else if (sift_aggspec_zone_cmp(p, c->lay, c->lay->nleaves))
		kept = zone_overlap(c, p->leaf, Min(Max(p->sel, 0.0), 1.0));
	return kept;
}

/*
 * The share of the units the small-set summaries of the leaves the conjuncts test leave (col/setsum.h): an equality or IN on the share s of the leaf's
 * distinct values (p->sel for an integer leaf, nvals / ndistinct for a text one) keeps a unit that holds n of them with the probability
 * 1 - (1 - n / distinct) ^ (s * distinct), about 1 - exp(-s * n); a unit without a summary stays. A summary that keeps nearly every unit is not counted:
 * the scan reads it for nothing then, as for a leaf that has none.
 */
static double
set_kept(AggCtx *c, SiftAggSpec *s)
{
	double		kept = 1.0;

	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];
		SiftSrcStrTest st;
		double		share;

		if (p->kind == SAP_LUT && sift_aggspec_str_test(p, c->lay, c->lay->nleaves - c->nvleaf, &st) && (st.op == SSTR_EQ || st.op == SSTR_IN) &&
			c->stats.ndistinct[p->leaf] > 0)
			share = st.nvals / c->stats.ndistinct[p->leaf];
		else if ((p->kind == SAP_CMP && p->op == SCMP_EQ) || (p->kind == SAP_IN && p->op == 0))
			share = Min(Max(p->sel, 0.0), 1.0);
		else
			continue;
		if (c->stats.set_cover[p->leaf] > 0)
		{
			double		cover = c->stats.set_cover[p->leaf],
						held = 1.0 - exp(-share * c->stats.set_keys[p->leaf]);

			if (held < AGG_SET_USEFUL)
				kept = Min(kept, 1.0 - cover + cover * held);
		}
	}
	return kept;
}

/*
 * The share of the units the zone tests leave: the comparisons (their selectivity is also the fraction of the leaf's range they keep) and
 * the ORs of them, the join key filters and a ctid range. The most selective counts, and the scan reads at least one unit (docs/COL-PLAN.md, "Zone pruning").
 */
static double
zone_kept(AggCtx *c, SiftAggSpec *s, int *nkeyfilters)
{
	double		kept = join_key_zone(c, s, nkeyfilters);
	List	   *rest;

	kept = Min(kept, zone_sample_kept(c, s, &rest));
	kept = Min(kept, set_kept(c, s));
	foreach_ptr(SiftAggPred, p, rest)
		kept = Min(kept, p->kind == SAP_TID ? p->sel : pred_zone_kept(c, p));
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

/* the share of the rows of a leaf that PostgreSQL's statistics expect to hold a distinct value; 0 when it has none (a path of a document, an expression) */
static double
leaf_stat_frac(AggCtx *c, int leaf, double rows)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
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
static double
preagg_unit_ops(AggCtx *c, SiftAggSpec *s, double units, double rows, double rows_out)
{
	double		tuples = 1.0,
				elems,
				ecost,
				tcost;
	SiftAggPaShape shape;

	if (s->group < 0 && (!sift_aggspec_preagg(s, c->lay, c->lay->nleaves, &c->pa, &shape) || !shape.rows))
		return Min(rows_out, 256.0) * AGG_PREAGG_GROUP;
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

/* The running terms of agg_cost: the work (in operator costs) is added in this order, which fixes its rounding */
typedef struct CostAcc
{
	double		rows;
	double		elems;
	double		sel_rows;
	double		units;
	double		open_cost;
	double		scan_cost;
	double		load_cost;		/* of scan_cost, the reads of the lookup tables (the leader's alone) and their rows */
	double		load_rows;
	double		gen_cost;
	double		arm_cost;		/* the tuples through the arms of a two level query, in operators (not in scan_cost: cost_parallel) */
	double		child_cost;
	double		zone_kept;
	double		agg_rows;
	double		scan_rows;
	double		rows_out;
} CostAcc;

/*
 * The rows read again from the heap: of the rows of the units a leaf the scan rechecks flags (the share of units the zones say), the share of them that
 * hold an overflow value (a sample of the flagged units says; AGG_RECHECK_FRAC where it could not be taken), one row of each unit at least. A table with
 * half of its documents dirty reads half of them again: past a few percent the normal plan is the cheaper one.
 */
static double
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
			pred_uses_leaf(s->pred[i], leaf))
			return true;
	return false;
}

/* The rows read and tested, the zones kept and the units opened */
/*
 * The dictionary entries a wide leaf builds for each row read, in operators: a unit has its own dictionary, so a column of 50,000 values read in
 * units of 10,000 rows builds about 9,500 entries a unit, not a fifth of its rows.
 */
static double
wide_entry_cost(AggCtx *c, int leaf, const CostAcc *k)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
	double		units = c->stats.nunits > 0 ? c->stats.nunits : k->units,
				nd = Max(leaf_distinct_frac(c, leaf, k->rows) * k->rows, 1.0),
				per_unit = k->rows / units,
				entries = nd * -expm1(-per_unit / nd);
	int32		typmod = e && IsA(e, Var) ? ((Var *) e)->vartypmod : -1;
	bool		image = false;

	if (lf->wtypid == NUMERICOID)
	{
		int			precision = ((typmod - VARHDRSZ) >> 16) & 0xffff,
					scale = (((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024;

		image = typmod < 0 || precision > 38 || scale < 0 || scale > 38;
	}
	return Min(entries * units / Max(k->rows, 1.0), 1.0) * (image ? AGG_WIDE_IMAGE : AGG_WIDE_ENTRY);
}

/*
 * A key read as dictionary codes gives an id to every entry of the dictionary of each unit read, whether a row that passes the filters holds it
 * or not: a near-unique text column mapped over the units the zones leave costs its rows in entries, and the table of ids outgrows the groups
 * the query has. Returns the entries mapped; *ids: the ids they make (the most of one key); *ops: what mapping them costs.
 */
static double
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
		double		frac,
					nd,
					entries;

		if ((s->key[i].kind != SAK_LEAF && s->key[i].kind != SAK_LUT) || s->key[i].leaf < 0)
			continue;
		lf = &c->lay->leaf[s->key[i].leaf];
		/* without statistics the values are not known to be many */
		if (lf->vtype != SCV_CODE || lf->wide)
			continue;
		frac = leaf_stat_frac(c, s->key[i].leaf, k->rows);
		if (frac <= 0)
			continue;
		nd = Max(frac * k->rows, 1.0);
		entries = nd * -expm1(-k->rows / units / nd) * read;
		total += entries;
		*ops += entries * AGG_KEY_ENTRY * (pow(Max(Min(entries, nd) / AGG_KEY_IDS, 1.0), 0.35) - 1.0);
		*ids = Max(*ids, Min(entries, nd));
	}
	return total;
}

static void
cost_read(AggCtx *c, SiftAggSpec *s, CostAcc *k)
{
	int			nkeyfilters;
	double		sparse;

	k->zone_kept = zone_kept(c, s, &nkeyfilters);
	/*
	 * A unit that few rows pass the filters is read at those rows only (docs/COL-SEARCH.md: a unit with a kept row in eight or fewer is
	 * compacted, its leaves decoded for the kept rows): the leaves the filters test are read whole, the others for the share that passes.
	 */
	sparse = Min(k->sel_rows / (k->rows * Max(k->zone_kept, 1.0 / k->units)), 1.0);
	sparse = sparse <= AGG_SPARSE_MAX ? Min(sparse * AGG_SPARSE_LEAF, 1.0) : 1.0;
	for (int i = 0; i < s->nleaves; i++)
		k->scan_cost += k->rows * (leaf_level(c, s->leaves[i]) ? elems_of(c, leaf_of(c, s->leaves[i])->parent) : 1.0) * AGG_SCAN_LEAF *
			(leaf_filtered(s, s->leaves[i]) ? 1.0 : sparse);
	k->scan_cost += k->rows * (1 + (s->group >= 0 ? k->elems : 0)) * AGG_FILTER_VALUE * Max(s->npred + nkeyfilters, 1);
	k->scan_cost += k->rows * c->elem_work * AGG_FILTER_VALUE;
	for (int i = 0; i < s->nvleaf; i++)
		k->scan_cost += sift_aggvleaf_member(&s->vleaf[i]) ? k->rows * elems_of(c, s->vleaf[i].obj) * AGG_VLEAF_ELEM : 0;
	k->scan_cost += k->rows * inline_text_cost(c, s);
	for (int i = 0; i < s->nleaves; i++)
		if (leaf_of(c, s->leaves[i])->wide)
			k->scan_cost += k->rows * (AGG_WIDE_ROW + wide_entry_cost(c, s->leaves[i], k));
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_WIDE && leaf_of(c, s->pred[i]->leaf)->vtype == SCV_CODE)
			k->scan_cost += k->rows * leaf_distinct_frac(c, s->pred[i]->leaf, k->rows) * AGG_WIDE_TEST;
	k->scan_cost = (k->scan_cost + k->units * AGG_UNIT_COST) * Max(k->zone_kept, 1.0 / k->units);
}

/* The tuples that reach the groups and what each takes */
static void
cost_tuples(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, CostAcc *k)
{
	k->agg_rows = k->sel_rows * (s->group >= 0 ? k->elems : 1.0);
	/* the lookup joins filter before the groups: no more tuples reach them than the rows PostgreSQL expects of the join */
	if (c->joins != NIL)
		k->agg_rows = Min(k->agg_rows, Max(input_rel->rows, 1.0));
	k->scan_rows = k->agg_rows;
	/* the rows of a preserved side no tuple matches are tuples too */
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind))
			k->agg_rows += Max(c->root->simple_rel_array[jc->rti]->tuples, 1.0);
	/* a group takes a tuple: the groups PostgreSQL expects of the unnested rows it assumes (a cap of 100 a call) are more than the elements hold */
	k->rows_out = Min(k->rows_out, Max(Max(s->nsets, 1) * k->agg_rows, 1.0));
	k->scan_cost += k->agg_rows * (Max(s->nsets, 1) * (AGG_TUPLE + s->nkeys * AGG_KEY_ROW + s->naggs * AGG_AGG_ROW) + s->ncombo * AGG_COMBO_ROW);
}

/* the rows the scan loads of a lookup table, and what each costs: a plan is run whole; an index scan reads the rows that pass the equalities, the rest of a table is read whole */
static double
join_load_rows(JoinCtx *jc, PlannerInfo *root, double *row_cost)
{
	RelOptInfo *jrel = root->simple_rel_array[jc->rti];
	double		tuples = Max(jrel->tuples, 1.0);

	if (jc->cpath != NULL)
	{
		*row_cost = AGG_JOIN_ROW;
		return Max(jc->cpath->rows, 1.0);
	}
	if (jc->j.nidx > 0 && join_index_usable(jrel, &jc->j))
	{
		*row_cost = AGG_IDX_ROW;
		return Min(Max(jrel->rows, 1.0), tuples);
	}
	*row_cost = AGG_JOIN_ROW;
	return tuples;
}

/* the rows of the lookup tables a parallel scan has the leader write to the shared memory (agg/lookshare.c) */
double
agg_share_rows(AggCtx *c)
{
	double		rows = 0;

	foreach_ptr(JoinCtx, jc, c->joins)
	{
		double		row_cost;

		if (jc->j.params == NIL)
			rows += join_load_rows(jc, c->root, &row_cost);
	}
	return rows;
}

/* The lookup joins: the children's plans and the lookup tables' reads */
static void
cost_joins(AggCtx *c, SiftAggSpec *s, CostAcc *k)
{
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		double		rows,
					row_cost;

		/* Correlated children run once per distinct input, with their native path's cost. */
		if (jc->j.params != NIL)
		{
			const SiftAggCombo *cb = &s->combo[jc->combo];
			SiftAggFirst *f = &s->join[foreach_current_index(jc)].first;
			double		native = combo_entries(c, s, cb, k->agg_rows) * (jc->cpath->total_cost + cpu_operator_cost * AGG_EVAL_SLOT);

			/* the child that is a lookup of one row by unique key: the table's rows read once, if that costs less than the child for each input */
			f->use = f->ok && jc->frows * AGG_FIRST_ROW < native;
			if (f->use)
			{
				k->load_rows += jc->frows;
				k->load_cost += jc->frows * AGG_FIRST_ROW;
			}
			else
				k->child_cost += native;
			continue;
		}
		rows = join_load_rows(jc, c->root, &row_cost);
		k->load_rows += rows;
		k->load_cost += rows * row_cost;
		if (jc->cpath != NULL)
			k->child_cost += jc->cpath->total_cost;
		if (jc->kind == SJ_SEMI || jc->kind == SJ_ANTI)
			k->scan_cost += k->sel_rows * AGG_SEMI_ROW;
	}
	k->scan_cost += k->load_cost;
}

/*
 * The chunks of the bitmap sets of a count(DISTINCT) of a leaf: a group's values fall in chunks of AGG_DIST_CHUNK_BITS bits of the value
 * range (nd values wide), and a chunk is allocated and zeroed when a value first touches it. Only while the chunks fit the set's memory:
 * past it the sets move to a hash set, whose pair costs between 0.1 and 2 us with the column and the size of the set (not priced: 0).
 */
static double
distinct_chunks(AggCtx *c, SiftAggAgg *a, CostAcc *k)
{
	double		groups = Max(Min(k->rows_out, k->agg_rows), 1.0),
				nd = a->leaf >= 0 ? c->stats.ndistinct[a->leaf] : 0,
				chunks,
				touched;

	if (nd <= 0)
		return 0;
	chunks = Max(ceil(nd / AGG_DIST_CHUNK_BITS), 1.0);
	touched = Min(groups * chunks * -expm1(-k->agg_rows / groups / chunks), k->agg_rows);
	return touched * AGG_DIST_CHUNK_BYTES > (double) get_hash_memory_limit() ? 0 : touched;
}

/* The aggregates, combos, keys and predicates of every tuple, less what the plain path saves, plus the bytes read */
static void
cost_work(AggCtx *c, SiftAggSpec *s, CostAcc *k)
{
	double		chunks = 0;

	for (int i = 0; i < s->naggs; i++)
	{
		if (s->agg[i].kind == SAA_COUNT_DISTINCT)
			chunks += distinct_chunks(c, &s->agg[i], k);
		if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
			k->scan_cost += k->agg_rows * AGG_ORDERED_ROW;
		k->scan_cost += k->agg_rows * (num_cost(&s->agg[i].num) + direct_agg_cost(&s->agg[i])) + k->rows * pred_num_cost(s->agg[i].filter);
		if (s->agg[i].kind == SAA_GEN)
			k->gen_cost += k->agg_rows * s->agg[i].gen->cost + k->rows_out * AGG_GEN_GROUP;
	}
	for (int a = 0; a < s->narms; a++)
		k->arm_cost += k->agg_rows * (s->arm[a].hmask ? AGG_ARM_HIDDEN : AGG_ARM_TUPLE);
	k->scan_cost += combo_eval_cost(c, s, k->rows, k->agg_rows, k->scan_rows) + bucket_eval_cost(c, s, k->agg_rows);
	for (int i = 0; i < s->nkeys; i++)
		k->scan_cost += k->agg_rows * num_cost(&s->key[i].num);
	for (int i = 0; i < s->npred; i++)
		k->scan_cost += k->rows * pred_num_cost(s->pred[i]);
	k->scan_cost = Max(k->scan_cost - plain_credit(c, s, k->agg_rows), k->scan_cost * 0.1);
	/* after the credit, which prices what the normal plan under-estimates: the rows read again from the heap are work it does not do */
	k->scan_cost += recheck_cost(c, s, k) * Max(k->zone_kept, 1.0 / k->units);
	k->scan_cost += chunks * AGG_DIST_CHUNK;
	k->scan_cost += scan_bytes(c, s, k->rows) * AGG_BYTE * Max(k->zone_kept, 1.0 / k->units);
}

/*
 * Each process aggregates its share into a table of its own (one per lane for float sums); the leader merges them. The lookup tables are
 * read once, by the leader, who writes them to the shared memory (agg/lookshare.c): the divisor is not theirs.
 */
static void
cost_parallel(SiftAggSpec *s, CostAcc *k, Cost *startup)
{
	double		tables = sift_aggspec_ordered(s) ? SIFT_AGG_LANES : s->nworkers + 1;
	double		load = Min(k->load_cost * cpu_operator_cost, k->scan_cost);
	double		arm = k->arm_cost * AGG_SCAN_SPEED * cpu_operator_cost,
				serial = k->scan_cost,
				setup = parallel_setup_cost * AGG_SETUP_FACTOR;

	k->scan_cost = (k->scan_cost + arm - load) / parallel_divisor(s->nworkers) + load + k->load_rows * AGG_SHARE_ROW * AGG_SCAN_SPEED * cpu_operator_cost +
		tables * Min(k->rows_out, Max(k->agg_rows / tables, 1.0)) * AGG_MERGE_GROUP * AGG_SCAN_SPEED * cpu_operator_cost;
	for (int i = 0; i < s->naggs; i++)
		if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
			k->scan_cost += k->agg_rows * AGG_ORDERED_MERGE * AGG_SCAN_SPEED * cpu_operator_cost;
	if (arm > 0)
	{
		/*
		 * The price of the path without workers leaves the arms out: the plan competes with the arms alone, whose rows the operators above them
		 * price below what they cost. The workers take the share of it that they take of the whole work, with the arms in.
		 */
		k->scan_cost = serial * (k->scan_cost + setup) / (serial + arm);
		return;
	}
	*startup += setup;
}

/* the per-group arrays of one aggregate, as acc_alloc (agg/group.c) allocates them */
static double
acc_bytes(const SiftAggAgg *a)
{
	double		b = sizeof(int64);	/* n */

	switch (a->kind)
	{
		case SAA_SUM_INT:
		case SAA_AVG_INT:
		case SAA_PCT_CONT:
		case SAA_PCT_DISC:
		case SAA_O_SUM_I64:
			return b + sizeof(int64);
		case SAA_SUM_NUM:
		case SAA_AVG_NUM:
			return b + sizeof(sift_i128) + 1;
		case SAA_MIN:
		case SAA_MAX:
			return b + sizeof(int64) + sizeof(double) + (a->num.n ? sizeof(sift_i128) : 0);
		case SAA_SUM_F64:
		case SAA_AVG_F64:
		case SAA_O_SUM_F64:
		case SAA_O_AVG_F64:
			return b + sizeof(double) + 1;
		case SAA_M_PG:
			return b + 1 + sizeof(Datum);
		case SAA_X_SUM:
		case SAA_X_AVG:
			return b + (a->restype == NUMERICOID ? sizeof(int64) + sizeof(sift_i128) : 0) + sizeof(double) + 1 + sizeof(Datum);
		case SAA_M_SUM:
		case SAA_M_AVG:
			return b + sizeof(double) + 1 + sizeof(Datum);
		case SAA_M_MIN:
		case SAA_M_MAX:
		case SAA_X_MIN:
		case SAA_X_MAX:
			return b + sizeof(Datum);
		case SAA_O_COUNT_ROWS:
		case SAA_COUNT_ROWS:
			return b + sizeof(uint64);
		case SAA_GEN:
			return b + 2 * sizeof(Datum) + 1 + (a->gen->ordered || a->gen->sorted || a->gen->distinct ? sizeof(int64) : 0);
		default:
			return b;
	}
}

/*
 * The bytes a group holds in the table of the scan: its keys' ids and values, its row in the group arrays, the arrays' room to grow.
 * *state: of them, what a partial state of the group carries when the table is written out (its aggregates and its key values).
 */
static double
group_bytes(const SiftAggSpec *s, double *state)
{
	double		b = sizeof(int64);	/* seen */
	double		vals = 0;

	for (int i = 0; i < s->naggs; i++)
		b += acc_bytes(&s->agg[i]);
	*state = b;
	/* the id tuple of the hash table and its slot, at most half full */
	b += s->nkeys * sizeof(uint32) + 4 * sizeof(uint32);
	for (int j = 0; j < s->nkeys; j++)
		vals += get_typavgwidth(s->key[j].typid, s->key[j].typmod);
	*state += vals;
	return b * AGG_GROUP_SLACK + s->nkeys * AGG_GROUP_KEY_BYTES + vals;
}

/*
 * The groups the table of the scan can hold: the estimate, and no more than the elements the index has (the normal plan expects 100 rows of
 * every call of an unnesting function, and the groups of a scan of 12,000 rows are not a million)
 */
double
group_count(AggCtx *c, const SiftAggSpec *s)
{
	double		rows = Max(c->stats.nrows, Max(c->rel->tuples, 1.0)) * (s->group >= 0 ? elems_of(c, s->group) : 1.0);

	return Min(Max(s->est_groups, 1.0), Max(rows, 1.0));
}

/* The bytes of the group table of the scan for the groups it is expected to hold. *state: of a group, what its partial state carries */
double
group_table_bytes(const SiftAggSpec *s, double groups, double *state)
{
	return groups * group_bytes(s, state);
}

/*
 * The group table of a spillable spec (agg/spill.c) that outgrows the memory a hash aggregate gets writes the partial states of its groups
 * to partitions on disk and reads them back by the end: the price is that of the spill of the normal plan (cost_agg): the pages of what is
 * written and read back and the tuples through them. A group in no partition is in the table: the states written are the share of the
 * rows whose group is not in it, and no more than a state for each group of each time the table fills. The table holds the ids of the keys
 * (key_entries), not only the groups that have rows: it fills with them, and each time it is walked for the groups it writes.
 */
static double
group_spill_cost(const SiftAggSpec *s, double groups, double ids, double rows, double *table)
{
	double		mem = (double) get_hash_memory_limit(),
				state,
				runs,
				written;

	*table = group_table_bytes(s, Max(groups, ids), &state);
	if (*table <= mem || !sift_aggspec_spillable(s))
		return 0;
	runs = ceil(*table / mem);
	written = Min(rows * (1.0 - mem / *table), groups * runs);
	return written * state / BLCKSZ * (random_page_cost + seq_page_cost) + 2.0 * cpu_tuple_cost * written +
		written * log2(runs + 1.0) * 2.0 * cpu_operator_cost + runs * ids * AGG_ID_SWEEP * cpu_operator_cost;
}

/*
 * The heap check of the rows on pages that are not all-visible (vis is their share), a page at a time: the pages of the units the zones keep
 * (zone_kept) that hold a selected row, which the rows reach by chance (Poisson). A page is in shared buffers, in the cache the operating system keeps
 * (PostgreSQL's own assumption for it is effective_cache_size), or read from the disk: a sequential read when the scan reaches most of the pages it
 * passes and a random one when it skips them.
 */
static double
vet_cost(AggCtx *c, double vis, double sel_rows, double zone_kept)
{
	double		pages = Max(rel_pages(c), 1.0),
				vet_pages = vis * Min(Max(pages * zone_kept, 1.0), pages),
				rows = vis * sel_rows,
				buffered = Min(NBuffers / pages, 1.0),
				cached = Max(Min(effective_cache_size / pages, 1.0), buffered),
				touched,
				disk,
				op = AGG_SCAN_SPEED * cpu_operator_cost;

	if (vet_pages <= 0)
		return 0;
	touched = vet_pages * (1.0 - exp(-rows / vet_pages));
	disk = seq_page_cost + (random_page_cost - seq_page_cost) * (1.0 - touched / vet_pages);
	return touched * (buffered * AGG_VET_PAGE * op + (cached - buffered) * AGG_VET_OS_PAGE * op + (1.0 - cached) * disk) + rows * AGG_VET_ROW * op;
}

void
agg_cost(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, Path *p, double rows_out)
{
	CostAcc		k = {0};
	double		vis = 1.0 - Min(Max(c->stats.allvisible_frac, 0.0), 1.0),
				exact,
				vet,
				spill,
				table,
				groups,
				entries,
				key_ops,
				ids;
	Cost		startup;

	k.rows =Max(c->stats.nrows, Max(c->rel->tuples, 1.0));
	k.elems = s->group >= 0 ? elems_of(c, s->group) : 1.0;
	k.sel_rows = Min(Max(c->rel->rows, 1.0), k.rows);
	k.units = Max(c->stats.nunits, ceil(k.rows / 2000.0));
	k.rows_out = rows_out;
	k.open_cost = c->search ? sift_search_open_cost(c->search, k.rows) : 0;
	k.scan_cost = c->search ? sift_search_cost(c->search, k.rows, k.units, &k.rows, &k.units) : 0;
	cost_read(c, s, &k);
	cost_tuples(c, s, input_rel, &k);
	cost_joins(c, s, &k);
	cost_work(c, s, &k);
	entries = key_entries(c, s, &k, &ids, &key_ops);
	k.scan_cost += key_ops;
	/* the reads of the lookup tables are PostgreSQL's own, priced as its plans are */
	k.scan_cost = (k.scan_cost - k.load_cost) * AGG_SCAN_SPEED + k.load_cost;
	/* a top-N reads a fraction of the units, but opens the match set whole */
	if (s->topn)
		k.scan_cost = (k.scan_cost - k.open_cost) * c->topn_frac + k.open_cost;
	k.scan_cost *= cpu_operator_cost;
	/*
	 * The normal plan of an unnest expects the rows of the function (100 per call for jsonb_to_recordset, jsonb_array_elements,
	 * jsonb_each) minus its filters, and prices its sort, join and aggregate with them, though the data has a handful per row: when it
	 * expects more unnested rows than the selected orders hold elements, this path is charged the square root of the excess. The
	 * estimates of the two plans then cross where the times do: a selective store set keeps the btree plan, a whole table or a
	 * filter on the elements (which cuts the expected rows) keeps its price. A qual with a bound parameter has no such estimate (a generic
	 * plan guesses its selectivity; the values are known at execution): no charge.
	 */
	if (s->group >= 0 && !rel_has_param_qual(c->rel))
		k.scan_cost *= Max(sqrt(input_rel->rows / (k.sel_rows * k.elems)), 1.0);
	/* the calls of the generic aggregates are the elements the data has, whatever the normal plan expects of the unnest */
	k.scan_cost += k.gen_cost * cpu_operator_cost;
	/* rows on pages that are not all-visible are checked in the heap */
	vet = vet_cost(c, vis, k.sel_rows, Min(Max(k.zone_kept, 1.0 / k.units), 1.0)) * (s->topn ? c->topn_frac : 1.0);
	k.scan_cost += vet;
	/* the exact units cost their groups, not their rows */
	exact = preagg_share(c, s);
	if (exact > 0)
	{
		double		pre_cost = k.units * (AGG_UNIT_COST + preagg_unit_ops(c, s, k.units, k.rows, k.rows_out)) * AGG_SCAN_SPEED * cpu_operator_cost;

		k.scan_cost += exact * (Min(pre_cost, k.scan_cost) - k.scan_cost);
	}
	startup = AGG_START_COST * cpu_operator_cost + k.child_cost + c->subplan_cost + AGG_PART_PAIR * c->nparts * c->nparts * cpu_operator_cost;
	if (s->nworkers > 0)
		cost_parallel(s, &k, &startup);
	/* each process holds a table of its own (and spills it) */
	groups = group_count(c, s);
	/* a top-N reads a fraction of the units, and makes the ids of those */
	spill = group_spill_cost(s, groups, Min(ids, entries / (s->nworkers + 1)) * (s->topn ? c->topn_frac : 1.0), k.agg_rows / (s->nworkers + 1), &table);
	k.scan_cost += spill;
	agg_arm_spill(spill, groups, table);
	agg_arm_vet(vet, vis * 100.0);
	p->startup_cost = startup + k.scan_cost;
	p->total_cost = p->startup_cost + k.rows_out * cpu_tuple_cost;
	/* the rows of a top-N are fetched from the heap, one page each */
	if (s->topn)
		p->total_cost += c->topn_fetch * random_page_cost * 0.5;
	/* the groups of the rows the lists pass (spec_finish) tell the plan above how many rows to expect; they are not part of the path's own price */
	p->rows = s->topn ? k.rows_out : Max(k.rows_out, c->path_rows);
}
