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
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/cost/int.h"

static bool real_cost_walker(Node *n, double *excess);
static bool hashed_subplan_walker(Node *n, double *ops);
static double combo_subplan_ops(const SiftAggCombo *cb);
static bool calendar_cost_walker(Node *n, double *ops);
static double reached_rows(AggCtx *c, const SiftAggSpec *s, const JoinCtx *jc, double tuples, bool counted);

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
 * The keys the inner and semi lookup joins of the query leave of a leaf (the join key filters, agg/init.c), narrowed by the join that is keyed by one of
 * their columns (a scope list joined to the table's rows): the tuples that reach a combo hold no other value of it. A lookup PostgreSQL has no statistics
 * for is its floor of one row whatever it holds (lookup_guessed), which bounds nothing. HUGE_VAL: no such join.
 */
static double
filtered_keys(AggCtx *c, const SiftAggSpec *s, int leaf)
{
	double		keys = HUGE_VAL;

	foreach_ptr(JoinCtx, jc, c->joins)
	{
		const SiftAggCombo *cb = &s->combo[jc->combo];
		const SiftAggJoin *j = &s->join[cb->join];
		int			dkey;
		const JoinCtx *dj = dependent_join(c, s, jc, &dkey);

		if ((j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || cb->level != 0 || lookup_guessed(c, jc) || (dj && lookup_guessed(c, dj)))
			continue;
		for (int k = 0; k < j->nkey; k++)
			if (j->keypos[k] >= 0 && cb->leaf[j->keypos[k]] == leaf)
				keys = Min(keys, Min(lookup_sure(c, jc), dj ? lookup_sure(c, dj) : HUGE_VAL));
	}
	return keys;
}

/* the rows of a lookup the tuples reach: those of the keys that reach it, the rows a key has (the table's, when it has no statistics or is a plan) */
static double
reached_rows(AggCtx *c, const SiftAggSpec *s, const JoinCtx *jc, double tuples, bool counted)
{
	const SiftAggCombo *cb = &s->combo[jc->combo];
	const SiftAggJoin *j = &s->join[cb->join];
	RelOptInfo *rel = c->root->simple_rel_array[jc->rti];
	double		rows = Max(rel->rows, 1.0),
				rpk;

	if (jc->cpath != NULL || j->params != NIL || j->nkey != 1 || j->tkey[0] != NULL || !OidIsValid(j->relid) ||
		(rpk = sift_agg_rows_per_key(j->relid, j->keyatt[0], rel->tuples)) <= 0)
		return rows;
	return Min(rows, combo_entries(c, s, cb, tuples, counted) * rpk);
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
	bool		tested = false;
	Bitmapset  *lookups = NULL;
	bool		scoped = cb->join >= 0 && ((JoinCtx *) list_nth(c->joins, cb->join))->j.scoped;
	int			parents[SIFT_AGG_MAX_CLEAVES],
				nparents = 0;
	double		parent_nd[SIFT_AGG_MAX_CLEAVES] = {0};

	/* the entries of a filter of the table are made for the values it rejects too; the groups of a count are those it keeps */
	for (int sl = 0; sl < cb->nslots && cb->join < 0; sl++)
		tested |= !cb->slot[sl].is_key;
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
						double		rows = reached_rows(c, s, jc, tuples, counted);

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
		if (IsA(e, Var) && (nd = column_distinct(c, (Var *) e, counted || !tested)) > 0)
			entries *= Min(nd, filtered_keys(c, s, cb->leaf[p]));
		else
			exprs = lappend(exprs, e);
	}
	for (int i = 0; i < nparents; i++)
		entries *= parent_nd[i];
	if (exprs != NIL)
		entries *= estimate_num_groups(c->root, exprs, tuples, NULL, NULL);
	/* the keys of a dense table share the entry of their class, and the keys it has no row for one */
	if (cb->join >= 0 && join_dense(c, s, list_nth(c->joins, cb->join)))
		entries = ((JoinCtx *) list_nth(c->joins, cb->join))->classes + 1;
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
			if (saa_x(s->agg[ai].kind) && s->agg[ai].combo == ci && s->agg[ai].slot == sl)
			{
				only &= s->agg[ai].wsum != 0;
				used = true;
			}
		only &= used;
	}
	return only;
}

/* does a slot of the combo test the elements of an array: &&, @>, <@ or = ANY? */
static bool
elements_walker(Node *n, void *ctx)
{
	if (n == NULL)
		return false;
	if (IsA(n, OpExpr))
	{
		Oid			fn = get_opcode(((OpExpr *) n)->opno);

		if (fn == F_ARRAYOVERLAP || fn == F_ARRAYCONTAINS || fn == F_ARRAYCONTAINED)
			return true;
	}
	if (IsA(n, ScalarArrayOpExpr))
		return true;
	return expression_tree_walker(n, elements_walker, ctx);
}

static bool
combo_tests_elements(const SiftAggCombo *cb)
{
	for (int sl = 0; sl < cb->nslots; sl++)
		if (elements_walker((Node *) cb->slot[sl].expr, NULL))
			return true;
	return false;
}

/*
 * The evaluation PostgreSQL does for the combos with a 64-bit leaf or a direct slot: a hash probe per tuple for the id of each wide
 * leaf, and the expressions once per distinct combination of the leaves (every tuple when the column has as many values). Combos of
 * dictionary and small integer leaves have few combinations and keep the price they had.
 */
double
combo_eval_cost(AggCtx *c, const SiftAggSpec *s, double rows, double agg_rows, double scan_rows, double kept_rows, double alive, double *entry_cost, CostAcc *k)
{
	double		cost = 0;

	*entry_cost = 0;

	for (int ci = 0; ci < s->ncombo; ci++)
	{
		const SiftAggCombo *cb = &s->combo[ci];
		int			wide = 0,
					arrays = 0;
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
				if (join_lut(c, s, jc) || join_dense(c, s, jc))
					cost += probes * AGG_JOIN_LUT;
				else
				{
					double		probe = probes * AGG_JOIN_PROBE * sqrt(Max(c->root->simple_rel_array[jc->rti]->rows, 1.0) / 250000.0);

					/* what of the probes the entries are: each process that takes rows of a key makes its entry (cost_work) */
					k->entry_ops += combo_entries(c, s, cb, probes, false) * AGG_ENTRY_ROW / AGG_SCAN_SPEED;
					k->entry_all += probes * AGG_ENTRY_ROW / AGG_SCAN_SPEED;
					cost += probe;
				}
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
			/* Without equality keys, every cached input tests every child row, and the inputs are the tuples the scan keeps: the join that follows narrows them to scan_rows only after the test */
			if (j->nkey == 0 && j->params == NIL)
			{
				double		candidates = jc->cpath ? child_rows(jc->cpath) : c->root->simple_rel_array[jc->rti]->rows,
							built = 0;

				/* agg_cost scales the scan by AGG_SCAN_SPEED, which the executor's evaluation of the clause for a pair (PostgreSQL's expression: jsonb builds, calendar functions) does not get; a semi join stops at the first match: half the candidates for a key that has one (in_expr_key measured 105 ns a pair at that count) */
				if (j->fslot >= 0)
				{
					Node	   *on = (Node *) cb->slot[j->fslot].expr;

					json_build_walker(on, &built);
					real_cost_walker(on, &built);
					calendar_cost_walker(on, &built);
				}
				cost += combo_entries(c, s, cb, alive, false) * Max(candidates, 1.0) * (j->kind == SJ_SEMI ? 0.5 : 1.0) * (AGG_PAIR_OPS + built) / AGG_SCAN_SPEED;
			}
			continue;
		}
		for (int p = 0; p < cb->nleaves; p++)
		{
			if (cb->leaf[p] >= 0)
			{
				wide += leaf_of(c, cb->leaf[p])->vtype == SCV_I64 || leaf_of(c, cb->leaf[p])->vtype == SCV_F64;
				arrays += array_image_width(c, cb->leaf[p]) > 0;
			}
			else
				lookup_ref = true;
		}
		for (int sl = 0; sl < cb->nslots; sl++)
		{
			direct |= cb->slot[sl].direct;
			filter |= !cb->slot[sl].is_key && !cb->slot[sl].direct;
		}
		tuples = filter ? rows : agg_rows;
		/* a test of the elements of a leaf that holds whole arrays only: each tuple looks its image up and resolves its entry, whatever the number of distinct arrays (a length or a null test reads lanes) */
		if (arrays > 0 && combo_tests_elements(cb))
			cost += tuples * arrays * AGG_ARR_ROW;
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
		double		entries = combo_entries(c, s, cb, tuples, false);

		cost += tuples * (wide * AGG_WIDE_LEAF + (direct ? AGG_DIRECT_ROW : 0.0)) + entries * cb->nslots * (direct ? AGG_EVAL_DIRECT : AGG_EVAL_SLOT);
		k->ms_ops += entries * combo_subplan_ops(cb) +
			wide * Max(tuples - entries, 0.0) * AGG_WIDE_MISS * entries * entries / (entries * entries + AGG_WIDE_MISS_HALF * AGG_WIDE_MISS_HALF);
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

/* what a function that breaks a timestamp into its calendar fields spends beyond the call, in operators PostgreSQL's executor runs (a cast or a comparison of dates and timestamps does not: 20 to 70 ns) */
static bool
calendar_cost_walker(Node *n, double *ops)
{
	if (n == NULL)
		return false;
	if (IsA(n, FuncExpr))
		switch (((FuncExpr *) n)->funcid)
		{
			case F_DATE_TRUNC_TEXT_TIMESTAMP:
			case F_DATE_TRUNC_TEXT_TIMESTAMPTZ:
			case F_DATE_TRUNC_TEXT_TIMESTAMPTZ_TEXT:
			case F_TIMESTAMPTZ_DATE:
			case F_DATE_PART_TEXT_DATE:
			case F_DATE_PART_TEXT_TIMESTAMP:
			case F_DATE_PART_TEXT_TIMESTAMPTZ:
			case F_EXTRACT_TEXT_DATE:
			case F_EXTRACT_TEXT_TIMESTAMP:
			case F_EXTRACT_TEXT_TIMESTAMPTZ:
			case F_TO_CHAR_TIMESTAMP_TEXT:
			case F_TO_CHAR_TIMESTAMPTZ_TEXT:
				*ops += AGG_REAL_CALENDAR;
				break;
			default:
				break;
		}
	return expression_tree_walker(n, calendar_cost_walker, ops);
}

/* a hashed SubPlan is looked up by the executor for each value combination: its call and the probe of the hash of the subplan's rows, which PostgreSQL prices as one operator */
static bool
hashed_subplan_walker(Node *n, double *ops)
{
	if (n == NULL)
		return false;
	if (IsA(n, SubPlan) && ((SubPlan *) n)->useHashTable)
		*ops += AGG_REAL_SUBPLAN;
	return expression_tree_walker(n, hashed_subplan_walker, ops);
}

/* what the hashed subplans of the slots of a combo cost for each value combination it has not seen, beyond the evaluation of the slot */
static double
combo_subplan_ops(const SiftAggCombo *cb)
{
	double		ops = 0;

	for (int sl = 0; sl < cb->nslots; sl++)
		hashed_subplan_walker((Node *) cb->slot[sl].expr, &ops);
	return ops;
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

		if (a->restype == NUMERICOID && (a->kind == SAA_SUM_NUM || a->kind == SAA_AVG_NUM || saa_minmax(a->kind) ||
										 saa_x_sumavg(a->kind) || a->kind == SAA_X_MIN || a->kind == SAA_X_MAX))
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
