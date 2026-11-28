/*
 * agg/plan/lateral_inline.c - a one-row LATERAL sub-select the aggregate scan takes once the query computes its outputs (agg/plan/flat_rewrite.h):
 * - a LATERAL (SELECT exprs [WHERE gate] LIMIT 1) without FROM gives one row of exprs for each row of its left side (none or nulls where the gate fails): its columns are the exprs, the gate a
 *   condition of the query (CASE WHEN gate THEN expr END under a LEFT JOIN). An expr that may raise moves only where it runs for the same rows as the sub-select's;
 * - an output that may raise of a LATERAL (.. LIMIT 1) with FROM is computed by the query from the columns it reads, for the rows the sub-select gave one (the aggregate scan plans a
 *   lookup that computes its outputs for every key), under the same rule.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"

#include "agg/plan/flat_rewrite.h"
#include "agg/safety.h"

/* a sub-select of one row without FROM: its select list reads the query a level up, its WHERE (a gate) too */
static bool
fromless_row(RangeTblEntry *rte)
{
	Query	   *s = rte->subquery;

	return rte->rtekind == RTE_SUBQUERY && sift_agg_one_row_query(s) && s->rtable == NIL && s->jointree->fromlist == NIL && !s->hasAggs && !s->hasWindowFuncs &&
		!s->hasTargetSRFs && !s->hasSubLinks && s->groupClause == NIL && s->havingQual == NULL && s->distinctClause == NIL && s->limitOffset == NULL &&
		s->setOperations == NULL && s->cteList == NIL && !agg_node_may_raise(s->jointree->quals);
}

typedef struct Inline
{
	Index		rti;			/* the sub-select's entry, and the join's (0: none), whose columns name the sub-select's */
	Index		jrti;
	List	   *cols;			/* the expression each column of it becomes, in the terms of the query; NULL: the column stays */
	int			depth;
	bool		whole;			/* a whole-row reference of the entry, or a column of the join's */
	Bitmapset  *raising;		/* the columns that may raise */
	Bitmapset  *read;			/* the raising columns read, and those read where every row reaching the read evaluates them */
	Bitmapset  *always;
} Inline;

static Node *
inline_mutator(Node *n, Inline *in)
{
	Node	   *r;

	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == in->depth && ((Var *) n)->varno == in->rti && list_nth(in->cols, ((Var *) n)->varattno - 1) != NULL)
	{
		r = add_nulling_relids(copyObject(list_nth(in->cols, ((Var *) n)->varattno - 1)), bms_make_singleton(in->rti), ((Var *) n)->varnullingrels);
		IncrementVarSublevelsUp(r, in->depth, 0);
		return r;
	}
	if (!IsA(n, Query))
		return expression_tree_mutator(n, inline_mutator, in);
	in->depth++;
	r = (Node *) query_tree_mutator((Query *) n, inline_mutator, in, 0);
	in->depth--;
	return r;
}

/* the columns of the entry that n reads (whole: a whole-row reference), at any level below the query */
static bool
cols_walker(Node *n, Inline *in)
{
	bool		r;

	if (n == NULL)
		return false;
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == in->depth && in->jrti != 0 && ((Var *) n)->varno == in->jrti)
		in->whole = true;
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == in->depth && ((Var *) n)->varno == in->rti)
	{
		in->whole |= ((Var *) n)->varattno <= 0;
		if (((Var *) n)->varattno > 0 && bms_is_member(((Var *) n)->varattno, in->raising))
			in->read = bms_add_member(in->read, ((Var *) n)->varattno);
		return false;
	}
	if (!IsA(n, Query))
		return expression_tree_walker(n, cols_walker, in);
	in->depth++;
	r = query_tree_walker((Query *) n, cols_walker, in, 0);
	in->depth--;
	return r;
}

/* the columns of the entry read where every input row evaluates them: down calls that evaluate each argument (no CASE, COALESCE, AND or OR, which may skip one) */
static void
always_read(Node *n, Inline *in)
{
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == 0 && ((Var *) n)->varno == in->rti && ((Var *) n)->varattno > 0)
		in->always = bms_add_member(in->always, ((Var *) n)->varattno);
	else if (IsA(n, FuncExpr) || IsA(n, OpExpr))
		foreach_ptr(Node, a, IsA(n, FuncExpr) ? ((FuncExpr *) n)->args : ((OpExpr *) n)->args)
			always_read(a, in);
	else if (IsA(n, RelabelType))
		always_read((Node *) ((RelabelType *) n)->arg, in);
	else if (IsA(n, CoerceViaIO))
		always_read((Node *) ((CoerceViaIO *) n)->arg, in);
}

/* the arguments of the aggregates without FILTER of the level, which run for each row of a group */
static bool
agg_args_walker(Node *n, Inline *in)
{
	if (n == NULL || IsA(n, Query))
		return false;
	if (IsA(n, Aggref) && ((Aggref *) n)->agglevelsup == 0)
	{
		if (((Aggref *) n)->aggfilter == NULL)
			foreach_node(TargetEntry, te, ((Aggref *) n)->args)
				always_read((Node *) te->expr, in);
		return false;
	}
	return expression_tree_walker(n, agg_args_walker, in);
}

/*
 * May the sub-select's columns that can raise move to where the query reads them: the lateral is the one other item of the FROM list of a grouped query (the filters of the table run before it
 * in both plans), no condition of the query reads them, and each one read is read by a key or an aggregate of every row, as the sub-select evaluates it for every row that reaches it
 */
static bool
raising_inline_ok(Query *q, Inline *in, JoinExpr *j)
{
	Node	   *left = j->larg;

	if (bms_is_empty(in->raising))
		return true;
	if (list_length(q->jointree->fromlist) != 1 || linitial(q->jointree->fromlist) != j || !IsA(left, RangeTblRef) ||
		rt_fetch(((RangeTblRef *) left)->rtindex, q->rtable)->rtekind != RTE_RELATION || (!q->hasAggs && q->groupClause == NIL) || q->hasWindowFuncs ||
		q->hasTargetSRFs || q->groupingSets != NIL || checkExprHasSubLink(q->jointree->quals))
		return false;
	cols_walker(q->jointree->quals, in);
	cols_walker(j->quals, in);
	if (!bms_is_empty(in->read))
		return false;
	cols_walker((Node *) q->targetList, in);
	cols_walker(q->havingQual, in);
	foreach_node(SortGroupClause, sgc, q->groupClause)
		always_read(sift_agg_ungroup_expr(q, (Node *) get_sortgroupclause_expr(sgc, q->targetList)), in);
	agg_args_walker((Node *) q->targetList, in);
	agg_args_walker(q->havingQual, in);
	return bms_is_subset(in->read, in->always);
}

/* CASE WHEN cond THEN e END */
static Node *
when(Node *cond, Node *e)
{
	CaseExpr   *c = makeNode(CaseExpr);
	CaseWhen   *w = makeNode(CaseWhen);

	w->expr = (Expr *) cond;
	w->result = (Expr *) e;
	w->location = -1;
	c->casetype = exprType(e);
	c->casecollid = exprCollation(e);
	c->args = list_make1(w);
	c->defresult = (Expr *) makeNullConst(c->casetype, exprTypmod(e), c->casecollid);
	c->location = -1;
	return (Node *) c;
}

/* an expression of a sub-select in the terms of the query a level up */
static Node *
lifted(Node *e)
{
	e = copyObject(e);
	IncrementVarSublevelsUp(e, -1, 1);
	return e;
}

/* the columns of a sub-select without FROM: the expressions of its select list, NULL where the gate fails under a LEFT JOIN */
static void
fromless_cols(const Query *s, JoinType jointype, Inline *in)
{
	foreach_node(TargetEntry, te, s->targetList)
	{
		Node	   *e = lifted((Node *) te->expr);

		if (agg_node_may_raise(e))
			in->raising = bms_add_member(in->raising, te->resno);
		in->cols = lappend(in->cols, s->jointree->quals != NULL && jointype == JOIN_LEFT ? when(lifted(s->jointree->quals), e) : e);
	}
}

/* a one-row sub-select of a FROM list whose outputs that may raise move out of it: none of them is a key of its order */
static bool
raising_lookup(RangeTblEntry *rte, Inline *in)
{
	Query	   *s = rte->subquery;

	if (rte->rtekind != RTE_SUBQUERY || !sift_agg_one_row_query(s) || s->jointree->fromlist == NIL || s->hasAggs || s->hasWindowFuncs || s->hasTargetSRFs || s->groupClause != NIL ||
		s->setOperations != NULL || s->distinctClause != NIL || s->limitOffset != NULL)
		return false;
	foreach_node(TargetEntry, te, s->targetList)
		if (!te->resjunk && agg_node_may_raise((Node *) te->expr))
		{
			if (te->ressortgroupref != 0)
				return false;
			in->raising = bms_add_member(in->raising, te->resno);
		}
	return !bms_is_empty(in->raising);
}

typedef struct Hoist
{
	Index		rti;
	Query	   *s;
	RangeTblEntry *rte;
} Hoist;

/* the output of s that is the expression e, added before the junk columns (a new column of the entry) when there is none */
static AttrNumber
output_of(Hoist *h, Expr *e, const char *name)
{
	int			at = 0;
	TargetEntry *te;

	foreach_node(TargetEntry, o, h->s->targetList)
	{
		if (!o->resjunk && equal(o->expr, e))
			return o->resno;
		at += !o->resjunk;
	}
	te = makeTargetEntry((Expr *) copyObject(e), at + 1, pstrdup(name), false);
	h->s->targetList = list_insert_nth(h->s->targetList, at, te);
	h->rte->eref->colnames = lappend(h->rte->eref->colnames, makeString(te->resname));
	foreach_node(TargetEntry, o, h->s->targetList)
		o->resno = foreach_current_index(o) + 1;
	return te->resno;
}

/* an expression of the sub-select in the terms of the query: its own columns as outputs of its entry, the columns a level up as the query's */
static Node *
hoist_mutator(Node *n, Hoist *h)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == 0)
	{
		Var		   *v = (Var *) n;

		return (Node *) makeVar(h->rti, output_of(h, (Expr *) v, "sift_col"), v->vartype, v->vartypmod, v->varcollid, 0);
	}
	if (IsA(n, Var))
	{
		Var		   *v = copyObject((Var *) n);

		v->varlevelsup--;
		return (Node *) v;
	}
	return expression_tree_mutator(n, hoist_mutator, h);
}

/* the outputs of the sub-select that may raise become the columns they read and whether it gave a row: the query computes them for the rows it joins */
static void
hoist_cols(Index rti, RangeTblEntry *rte, Inline *in)
{
	Hoist		h = {rti, rte->subquery, rte};
	const int	n = list_length(h.s->targetList);
	Node	   *found = (Node *) makeVar(rti, output_of(&h, (Expr *) makeBoolConst(true, false), "sift_found"), BOOLOID, -1, InvalidOid, 0);
	TargetEntry *te;

	for (int c = 1; c <= n; c++)
		in->cols = lappend(in->cols, NULL);
	for (int c = -1; (c = bms_next_member(in->raising, c)) >= 0;)
	{
		te = list_nth_node(TargetEntry, h.s->targetList, c - 1);
		lfirst(list_nth_cell(in->cols, c - 1)) = when(found, hoist_mutator((Node *) te->expr, &h));
		te->expr = (Expr *) makeNullConst(exprType((Node *) te->expr), exprTypmod((Node *) te->expr), exprCollation((Node *) te->expr));
	}
}

/*
 * The join at *at of the FROM list of q, or one of the inner joins and left sides below it, whose right side is a one-row sub-select: one without FROM inlined, one with outputs that may
 * raise with those outputs computed by the query (apply)
 */
static bool
inline_in(Query *q, Node **at, bool apply)
{
	JoinExpr   *j = (JoinExpr *) *at;
	Index		rti;
	RangeTblEntry *rte;
	Inline		in = {0};
	bool		fromless;

	if (!IsA(*at, JoinExpr) || (j->jointype != JOIN_INNER && j->jointype != JOIN_LEFT))
		return false;
	if (inline_in(q, &j->larg, apply) || (j->jointype == JOIN_INNER && inline_in(q, &j->rarg, apply)))
		return true;
	if (!IsA(j->rarg, RangeTblRef))
		return false;
	rte = rt_fetch(rti = ((RangeTblRef *) j->rarg)->rtindex, q->rtable);
	in.rti = rti;
	in.jrti = j->rtindex;
	fromless = fromless_row(rte);
	if (!fromless && !raising_lookup(rte, &in))
		return false;
	if (j->jointype == JOIN_LEFT && j->quals != NULL && !(IsA(j->quals, Const) && !((Const *) j->quals)->constisnull && DatumGetBool(((Const *) j->quals)->constvalue)))
		return false;
	if (fromless)
		fromless_cols(rte->subquery, j->jointype, &in);
	query_tree_walker(q, cols_walker, &in, QTW_IGNORE_JOINALIASES);
	if (in.whole)
		return false;
	in.read = NULL;
	if (!raising_inline_ok(q, &in, j))
		return false;
	if (!apply)
		return true;
	if (!fromless)
		hoist_cols(rti, rte, &in);
	else
	{
		if (j->jointype == JOIN_INNER)
		{
			List	   *conds = make_ands_implicit((Expr *) q->jointree->quals);

			if (j->quals != NULL)
				conds = lappend(conds, j->quals);
			if (rte->subquery->jointree->quals != NULL)
				conds = lappend(conds, lifted(rte->subquery->jointree->quals));
			q->jointree->quals = conds == NIL ? NULL : (Node *) make_ands_explicit(conds);
		}
		if (j->rtindex != 0)
			rt_fetch(j->rtindex, q->rtable)->jointype = JOIN_INNER;
		*at = j->larg;
	}
	*q = *(Query *) query_tree_mutator(q, inline_mutator, &in, 0);
	return true;
}

bool
sift_agg_inline_laterals(Query *q, bool apply)
{
	bool		found = false;
	ListCell   *lc;

	if (q->commandType != CMD_SELECT || q->rowMarks != NIL || q->setOperations != NULL)
		return false;
	foreach(lc, q->jointree->fromlist)
		while (inline_in(q, (Node **) &lfirst(lc), apply))
		{
			if (!apply)
				return true;
			found = true;
		}
	return found;
}
