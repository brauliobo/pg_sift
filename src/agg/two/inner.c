/*
 * agg/two/inner.c - the inner aggregate of each arm
 * The aggregates and HAVING an arm computes, the hidden keys it needs, and the terms
 * the outer aggregate reads from it.
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/fmgroids.h"
#include "catalog/pg_type.h"

#include "agg/two/int.h"


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
		case SAA_MAX:
		case SAA_MIN:
			/* the float8 leaf of the arm's table: the larger or smaller of a cell's values, a NaN the largest */
			if (ia->restype != FLOAT8OID || ia->num.n || ia->nvx || ia->leaf < 0)
				return -1;
			break;
		case SAA_X_SUM:
		case SAA_X_AVG:
		case SAA_X_MAX:
		case SAA_X_MIN:
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
bool
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

/* the inner aggregates of a HAVING conjunct become Params; a column (a group expression) or a volatile function is not allowed */
static Node *
having_mutator(Node *n, HavingSub *h)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Aggref))
	{
		int			ai = h->bad || list_length(h->src) >= SIFT_AGG_MAX_PARAMS ? -1 : arm_inner_agg(h->c, h->in, n, exprType(n));
		Param	   *p = exec_param(list_length(h->src), exprType(n), -1, InvalidOid);

		if (ai < 0)
			h->bad = true;
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

/* the key of the scan that group expression x of the arm is; -1 if it is none */
int
arm_key_of(const ArmIn *arm, Node *x)
{
	ListCell   *le,
			   *li;

	forboth(le, arm->kexpr, li, arm->kidx)
		if (equal(lfirst(le), x))
			return lfirst_int(li);
	return -1;
}

/*
 * count(DISTINCT x) of a column that every arm groups by (or has as NULL): the distinct ids of that key among the cells of a group. The
 * key is one of the scan, with the ids of the grouping, so the distinct operator must be the key's.
 */
static int
distinct_key(SiftAggSpec *s, Aggref *oa, ArmIn *arms, int narms, int col)
{
	int			key = -1;
	SortGroupClause *sgc;

	for (int k = 0; k < narms; k++)
	{
		Node	   *x = list_nth(arms[k].cols, col);
		int			j;

		if (null_const(x))
			continue;
		j = arm_key_of(&arms[k], x);
		if (j < 0 || (key >= 0 && j != key))
			return -1;
		key = j;
	}
	if (key < 0 || list_length(oa->aggdistinct) != 1)
		return -1;
	sgc = linitial_node(SortGroupClause, oa->aggdistinct);
	return sgc->eqop == s->key[key].eqop && oa->inputcollid == s->key[key].collid ? key : -1;
}

/* The outer aggregate: avg/sum of a float8 or int8 column of the arms, count(DISTINCT id or a key), count(*) and count(id), with a FILTER of comparisons of columns */
bool
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
		arg = strip_relabel(strip_ph((Node *) linitial_node(TargetEntry, oa->args)->expr));
		if (!IsA(arg, Var) || ((Var *) arg)->varno != srti || ((Var *) arg)->varlevelsup != 0)
			return false;
		v = (Var *) arg;
		out->col = v->varattno - 1;
		if (out->col < 0 || out->col >= list_length(arms[0].cols))
			return false;
	}
	if (fid == F_COUNT_ && oa->aggdistinct == NIL)
		out->kind = SAA_O_COUNT_STAR;
	else if (fid == F_COUNT_ANY)
	{
		bool		ids = true;

		for (int k = 0; k < narms; k++)
			ids &= equal(list_nth(arms[k].cols, out->col), arms[k].id);
		/* the id of a row is never NULL: count(id) counts the inner groups, as count(*) does */
		if (oa->aggdistinct == NIL)
		{
			if (!ids)
				return false;
			out->kind = SAA_O_COUNT_STAR;
		}
		else if (ids)
			out->kind = SAA_O_COUNT_ROWS;
		else if ((out->slot = distinct_key(s, oa, arms, narms, out->col)) >= 0)
			out->kind = SAA_O_COUNT_KEY;
		else
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
						 out->kind == SAA_O_COUNT_KEY ? "count(DISTINCT key)" :
						 out->kind == SAA_O_AVG_F64 ? "avg(inner)" : out->kind == SAA_O_SUM_F64 ? "sum(inner)" : "sum(inner int8)");
	return true;
}

/*
 * A cell is an inner group, the share of one row in one group of an arm. When an arm groups by more than the outer keys and its id, one
 * row has several cells in an outer group, so the group expressions that are no outer key are keys of the cells: one key each, shared by
 * the arms that have an equal expression, after the outer keys and before the number of the set. They name no group (a group has id 0
 * for them, and the scan emits NULL).
 */
bool
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

			/* a constant splits no group */
			if (equal(g, arms[k].id) || list_member(arms[k].used, g) || IsA(g, Const))
				continue;
			pos = -1;
			foreach_ptr(Node, d, defs)
				if (equal(d, g))
					pos = foreach_current_index(d);
			if (pos < 0)
			{
				if (s->nkeys + sets >= sift_limit(group_keys, SIFT_AGG_MAX_KEYS))
					return agg_refuse_limit(TOO_MANY_KEYS, group_keys, sift_limit(group_keys, SIFT_AGG_MAX_KEYS), "%d keys with the hidden ones of the arms", s->nkeys + sets + 1);
				key = &s->key[s->nkeys];
				if (!compile_key(c, sgc, g, key))
					return agg_refuse_f(TWO_KEYS, "the group expression %d of arm %d is not a key the scan reads: %s", i + 1, k + 1, expr_text(c, g));
				combo_set_key(c, key, s->nkeys);
				*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) makeNullConst(key->typid, key->typmod, key->collid),
																   list_length(*scan_tlist) + 1, NULL, false));
				defs = lappend(defs, g);
				pos = s->nkeys++ - nouter;
			}
			arms[k].out->hmask |= (uint32) 1 << (nouter + pos);
			arms[k].kexpr = lappend(arms[k].kexpr, g);
			arms[k].kidx = lappend_int(arms[k].kidx, nouter + pos);
		}
	s->nhidden = s->nkeys - nouter;
	return true;
}

/*
 * The arms of the FROM item and the scan they share; NULL when they are not arms of one scan. Arms that read their own copy of it share
 * the scan of the first: the conditions of its WHERE that not every arm has (a NOT NULL test of a lookup in two arms and none in a third)
 * are not the scan's but the filter of the arms that have them (ArmIn.quals).
 */
PlannerInfo *
two_arms(TwoLevel *t)
{
	PlannerInfo *base = NULL;
	bool		own = true;

	t->roots = outer_arms(t->root, t->srti, &t->gates);
	if (t->roots == NIL)
	{
		agg_refused_on(TWO_ARMS, "the FROM item is not a subquery or a UNION ALL of subqueries that PostgreSQL planned as one");
		return NULL;
	}
	if (agg_width_hit(list_length(t->roots) - 1, arms, sift_limit(arms, SIFT_AGG_MAX_ARMS), "arms"))
		return NULL;
	foreach_ptr(PlannerInfo, ar, t->roots)
	{
		PlannerInfo *b = arm_base(ar, &t->arms[t->narms]);

		if (b == NULL)
		{
			agg_refused_f(TWO_ARMS, "arm %d is not a plain aggregate (it has DISTINCT, ORDER BY, LIMIT, windows, grouping sets, a CTE list or no GROUP BY)", t->narms + 1);
			return NULL;
		}
		if (base == NULL)
		{
			base = b;
			own = b == ar;
		}
		if ((b == ar) != own || (!own && b != base) || (own && b != base && !same_scan(base, b, &t->outer)))
		{
			agg_refused_f(TWO_ARMS, "arm %d does not read the scan of arm 1: its tables or joins differ", t->narms + 1);
			return NULL;
		}
		t->arms[t->narms].sub.tlist = b->parse->targetList;
		t->narms++;
	}
	t->base = base;
	if (!own)
		return base;
	foreach_ptr(Node, q, (List *) base->parse->jointree->quals)
	{
		bool		all = true;

		foreach_ptr(PlannerInfo, ar, t->roots)
			all &= list_member((List *) ar->parse->jointree->quals, q);
		t->common = all ? lappend(t->common, q) : t->common;
		t->drop = all ? t->drop : lappend(t->drop, q);
	}
	return base;
}

/* arm k in the terms of the scanned table: its columns, group expressions, filter and HAVING */
bool
two_arm_terms(TwoLevel *t, int k)
{
	AggCtx	   *c = &t->ctx;
	Query	   *aq = ((PlannerInfo *) list_nth(t->roots, k))->parse;
	ArmIn	   *in = &t->arms[k];
	int			nid = 0,
				nagg;
	List	   *rest = NIL;

	foreach_node(TargetEntry, te, aq->targetList)
		if (!te->resjunk)
			in->cols = lappend(in->cols, normalize_expr(c, cte_sub_mutator((Node *) te->expr, &in->sub)));
	if (k == 0)
		t->ncols = list_length(in->cols);
	else if (t->ncols != list_length(in->cols))
		return agg_refuse_f(TWO_ARMS, "arm %d returns %d columns, arm 1 returns %d", k + 1, list_length(in->cols), t->ncols);
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
		return agg_refuse_f(TWO_ARMS, "arm %d groups by %d unique row ids, not one", k + 1, nid);
	/* the WHERE of an arm that reads a common table expression, or the conditions of its own that other arms lack */
	foreach_ptr(Node, q, (List *) aq->jointree->quals)
		if (in->sub.rti != 0 || !list_member(t->common, q))
			in->quals = lappend(in->quals, normalize_expr(c, cte_sub_mutator(q, &in->sub)));
	in->out = palloc0(sizeof(SiftAggArm));
	in->out->colsrc = palloc(Max(t->ncols, 1) * sizeof(int));
	for (int i = 0; i < t->ncols; i++)
		in->out->colsrc[i] = -1;
	nagg = t->ncols;
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
		return agg_refuse_f(TWO_ARMS, "the HAVING of arm %d is not a condition on its aggregates", k + 1);
	in->out->filter = palloc(Max(list_length(in->quals), 1) * sizeof(SiftAggPred *));
	foreach_ptr(Node, q, in->quals)
	{
		SiftAggPred *p = compile_combo_pred(c, q);

		if (p == NULL || unsupported_clause(c, q))
			return agg_refuse_f(TWO_ARMS, "the WHERE of arm %d has a condition the scan cannot test", k + 1);
		in->out->filter[in->out->nfilter++] = p;
	}
	return true;
}
