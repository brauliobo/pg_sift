/*
 * agg/cost/combo.c - what a combo and a numeric or JSON expression cost a tuple
 * The operators of a numeric expression, the slots of a combo, the buckets of a key and the
 * direct cost of an aggregate (direct_agg_cost).
 */
#include "postgres.h"

#include <math.h>

#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/selfuncs.h"
#include "catalog/pg_type.h"

#include "agg/cost/int.h"

static bool real_cost_walker(Node *n, double *excess);

double
num_cost(const SiftAggNum *o)
{
	double		cost = 0;

	for (int k = 0; k < o->n; k++)
		cost += o->step[k].op >= SNX_ROUND ? AGG_NUM_DIV : o->step[k].op > SNX_CONST ? AGG_NUM_STEP : 0;
	return cost;
}

/* the numeric programs of a predicate tree, per row that reaches them */
double
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
 * distinct value combinations of the inputs of a combo among `tuples`: what the statistics say of the columns, the tuples when an input is
 * not one; a column of an earlier lookup has at most as many values as the lookup has rows. The combo of a lookup per element is not
 * evaluated for the tuples but for the combinations of the values of the elements (the index counts those of each leaf) and of the column
 * of the lookup it reads. `counted` prices the elements of an unscoped lookup by those counts too, not by PostgreSQL's default.
 */
double
combo_entries(AggCtx *c, const SiftAggSpec *s, const SiftAggCombo *cb, double tuples, bool counted)
{
	List	   *exprs = NIL;
	double		entries = 1.0,
				elem_nd = 1.0;
	Bitmapset  *lookups = NULL;
	bool		scoped = cb->join >= 0 && ((JoinCtx *) list_nth(c->joins, cb->join))->j.scoped;
	int			parents[SIFT_AGG_MAX_CLEAVES],
				nparents = 0;
	double		parent_nd[SIFT_AGG_MAX_CLEAVES] = {0};

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
						entries *= jc->j.params != NIL ? rows * combo_entries(c, s, &s->combo[jc->combo], tuples, counted) : rows;
					}
					lookups = bms_add_member(lookups, jc->combo);
				}
			continue;
		}
		lf = leaf_of(c, cb->leaf[p]);
		/* the values of a member or an element have no statistics (but the counts of the index, where asked): PostgreSQL's default number of distinct values, as for a column it knows nothing of */
		if (lf->parent >= 0 || lf->path != NIL)
		{
			nd = (scoped || counted) && c->stats.ndistinct[cb->leaf[p]] > 0 ? c->stats.ndistinct[cb->leaf[p]] : DEFAULT_NUM_DISTINCT;
			/* the fields of one element go together (a category and its kind): the combinations are about as many as the values of the widest */
			if (scoped)
				elem_nd = Max(elem_nd, nd);
			else if (counted && lf->parent >= 0)
			{
				int			i = 0;

				while (i < nparents && parents[i] != lf->parent)
					i++;
				if (i == nparents)
					parents[nparents++] = lf->parent;
				parent_nd[i] = Max(parent_nd[i], nd);
			}
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
	for (int i = 0; i < nparents; i++)
		entries *= parent_nd[i];
	if (exprs != NIL)
		entries *= estimate_num_groups(c->root, exprs, tuples, NULL, NULL);
	return Max(Min(entries * elem_nd, tuples), 1.0);
}

/* what a test of the rest of an ON clause spends beyond the price of the slot: the jsonb values it builds, in operators PostgreSQL's executor runs */
static bool
json_build_walker(Node *n, double *ops)
{
	if (n == NULL)
		return false;
	if (IsA(n, FuncExpr) && ((FuncExpr *) n)->funcresulttype == JSONBOID)
		*ops += AGG_REAL_JSON;
	return expression_tree_walker(n, json_build_walker, ops);
}

/* the expressions of a combo that PostgreSQL evaluates to a numeric: an entry of those allocates and compares digits (an integer one was not measured to cost as much) */
static int
combo_numeric_slots(const SiftAggCombo *cb)
{
	int			n = 0;

	for (int sl = 0; sl < cb->nslots; sl++)
		n += exprType((Node *) cb->slot[sl].expr) == NUMERICOID;
	return n;
}

/* the combo is only the argument of sums and averages that add the entries of their dictionary leaf (SiftAggAgg.wsum): no tuple is evaluated by it */
bool
combo_wsum_only(const SiftAggSpec *s, int ci)
{
	const SiftAggCombo *cb = &s->combo[ci];
	bool		only = cb->nslots > 0;

	for (int sl = 0; sl < cb->nslots; sl++)
	{
		bool		used = false;

		only &= cb->slot[sl].direct;
		for (int ai = 0; ai < s->naggs; ai++)
			if ((s->agg[ai].kind >= SAA_X_COUNT && s->agg[ai].kind <= SAA_X_AVG) && s->agg[ai].combo == ci && s->agg[ai].slot == sl)
			{
				only &= s->agg[ai].wsum != 0;
				used = true;
			}
		only &= used;
	}
	return only;
}

/*
 * The evaluation PostgreSQL does for the combos with a 64-bit leaf or a direct slot: a hash probe per tuple for the id of each wide
 * leaf, and the expressions once per distinct combination of the leaves (every tuple when the column has as many values). Combos of
 * dictionary and small integer leaves have few combinations and keep the price they had.
 */
double
combo_eval_cost(AggCtx *c, const SiftAggSpec *s, double rows, double agg_rows, double scan_rows, double kept_rows, double alive, double *entry_cost)
{
	double		cost = 0;

	*entry_cost = 0;

	for (int ci = 0; ci < s->ncombo; ci++)
	{
		const SiftAggCombo *cb = &s->combo[ci];
		int			wide = 0;
		bool		direct = false,
					filter = false,
					lookup_ref = false;
		double		tuples;

		if (cb->join >= 0)
		{
			const SiftAggJoin *j = &s->join[cb->join];
			JoinCtx    *jc = list_nth(c->joins, cb->join);

			/*
			 * With equality keys, every tuple the filters keep probes the table before the joins narrow the rows to the ones that reach the groups
			 * (a join keyed by the output of an earlier lookup sees what that one let through: the mean of the two counts), and the rest of the
			 * ON clause is evaluated for each distinct combination of the values it reads, which for a key of many values is nearly every tuple.
			 */
			if (j->nkey > 0 && j->params == NIL)
			{
				double		probes = alive;

				for (int p = 0; p < cb->nleaves; p++)
					if (cb->leaf[p] < 0)
						probes = Min(alive, sqrt(alive * Max(agg_rows, 1.0)));
				cost += probes * (join_lut(c, s, jc) ? AGG_JOIN_LUT : AGG_JOIN_PROBE * sqrt(Max(c->root->simple_rel_array[jc->rti]->rows, 1.0) / 250000.0));
				/* the rest of the ON clause is PostgreSQL's expression, for each entry; the executor's evaluation does not get AGG_SCAN_SPEED's share (110 ns an entry measured, 36 counted) */
				if (on_tests_tuple(s, ci) && !combo_wsum_only(s, ci))
				{
					double		real = 0;

					real_cost_walker((Node *) cb->slot[j->fslot].expr, &real);
					*entry_cost += combo_entries(c, s, cb, probes, false) * (AGG_EVAL_SLOT + real) / AGG_SCAN_SPEED;
				}
				else if (join_extra_leaves(cb, j) > 0 && combo_numeric_slots(cb) > 0)
					*entry_cost += combo_entries(c, s, cb, probes, false) * (AGG_ENTRY_ROW + cb->nslots * AGG_ENTRY_SLOT) / AGG_SCAN_SPEED;
			}
			/* Without equality keys, every cached input tests every child row. */
			if (j->nkey == 0 && j->params == NIL)
			{
				double		candidates = jc->cpath ? child_rows(jc->cpath) : c->root->simple_rel_array[jc->rti]->tuples,
							built = 0;

				/* agg_cost scales the scan by AGG_SCAN_SPEED, which the executor's jsonb builds do not get */
				if (j->fslot >= 0)
					json_build_walker((Node *) cb->slot[j->fslot].expr, &built);
				cost += combo_entries(c, s, cb, scan_rows, false) * Max(candidates, 1.0) * (AGG_EVAL_SLOT + built / AGG_SCAN_SPEED);
			}
			continue;
		}
		for (int p = 0; p < cb->nleaves; p++)
		{
			if (cb->leaf[p] >= 0)
				wide += leaf_of(c, cb->leaf[p])->vtype == SCV_I64 || leaf_of(c, cb->leaf[p])->vtype == SCV_F64;
			else
				lookup_ref = true;
		}
		for (int sl = 0; sl < cb->nslots; sl++)
		{
			direct |= cb->slot[sl].direct;
			filter |= !cb->slot[sl].is_key && !cb->slot[sl].direct;
		}
		tuples = filter ? rows : agg_rows;
		/*
		 * A test between leaves (a column compared with a column) is evaluated by PostgreSQL once per distinct combination of the leaves: a few for
		 * dictionary and small integer leaves, which keep the price they had, one per row for two dates that vary (545 instructions a row measured,
		 * 124 ms for 400,000 rows, against the 36 of a test on a constant); so is an expression of a key, for the values the groups have
		 */
		if (wide == 0 && !direct)
		{
			double		entries = !lookup_ref && (filter ? cb->nleaves > 1 : cb->nleaves > 0) ? combo_entries(c, s, cb, filter ? kept_rows : agg_rows, false) : 0;

			if (entries > AGG_KEY_IDS)
				cost += entries * cb->nslots * AGG_EVAL_SLOT;
			continue;
		}
		if (combo_wsum_only(s, ci))
			continue;
		cost += tuples * wide * AGG_WIDE_LEAF + combo_entries(c, s, cb, tuples, false) * cb->nslots * AGG_EVAL_SLOT;
	}
	return cost + *entry_cost;
}

/* the buckets of a stretch of the leaf of a key, by the statistics: the tuples when there are none */
static double
bucket_count(AggCtx *c, const SiftAggKey *k, double tuples)
{
	int64		lo,
				hi;

	return leaf_span(c, k->leaf, &lo, &hi) ? Min(tuples, (double) (hi - lo) / USECS_PER_SEC / k->bucket + 1) : tuples;
}

/* a lookup per tuple of a bucket key, and PostgreSQL evaluating the expression once per bucket */
double
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
double
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
		for (int i = 0; i < s->naggs; i++)
			ops += num_ops(&s->agg[i].num) + pred_num_ops(s->agg[i].filter);
		excess += ops * (AGG_REAL_NUMERIC - 2.0);
		/* the key of an integer or float program is an fmgr call, not the allocation and digits of a numeric one */
		for (int i = 0; i < s->nkeys; i++)
			excess += num_ops(&s->key[i].num) * ((s->key[i].typid == NUMERICOID ? AGG_REAL_NUMERIC : AGG_REAL_CALL) - 2.0);
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
double
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
