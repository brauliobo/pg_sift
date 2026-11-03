/*
 * agg/flat/fence.c - a scope sublink as a key source
 * IN, EXISTS and NOT EXISTS over a join of tables (`k IN (SELECT coalesce(a.k, g.k) FROM a LEFT JOIN g ON ..)`), written so that PostgreSQL plans the
 * sub-select by itself and the scan reads its keys.
 */

/*
 * PostgreSQL pulls the sublink up into a semi or anti join and the sub-select into the FROM tree of the query, where the key is a function of columns of two
 * tables of the tree, which the scan does not read as a key. The sub-select with OFFSET 0 is not pulled up: its plan is a table of one column, the key, and
 * the join to it is a plain equality, the lookup of a table that is a plan. The rows of the query do not change (OFFSET 0 limits nothing). For EXISTS and
 * NOT EXISTS the equality that ties the sub-select to the query (`coalesce(a.k, g.k) = x.k`) moves out of it, so that the sub-select is uncorrelated.
 * The planner hook plans the query both ways and keeps this one if it runs in the scan and costs less.
 */
#include "postgres.h"

#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

/* a SELECT whose rows are those of its FROM list kept by its WHERE: the form PostgreSQL pulls up into the query around it */
static bool
simple_select(Query *q)
{
	return q->commandType == CMD_SELECT && q->jointree != NULL && !q->hasAggs && !q->hasWindowFuncs && !q->hasTargetSRFs && !q->hasDistinctOn &&
		q->groupClause == NIL && q->groupingSets == NIL && q->distinctClause == NIL && q->havingQual == NULL && q->limitCount == NULL &&
		q->limitOffset == NULL && q->setOperations == NULL && q->cteList == NIL && q->rowMarks == NIL;
}

/* does the FROM tree hold a join, or a sub-select that PostgreSQL would pull up and that holds one? */
static bool
from_has_join(Query *q, Node *n)
{
	if (n == NULL)
		return false;
	if (IsA(n, JoinExpr))
		return true;
	if (IsA(n, FromExpr))
	{
		FromExpr   *f = (FromExpr *) n;

		if (list_length(f->fromlist) > 1)
			return true;
		foreach_ptr(Node, x, f->fromlist)
			if (from_has_join(q, x))
				return true;
		return false;
	}
	if (IsA(n, RangeTblRef))
	{
		RangeTblEntry *rte = rt_fetch(((RangeTblRef *) n)->rtindex, q->rtable);

		return rte->rtekind == RTE_SUBQUERY && simple_select(rte->subquery) && from_has_join(rte->subquery, (Node *) rte->subquery->jointree);
	}
	return false;
}

static Node *
offset_zero(void)
{
	return (Node *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, FLOAT8PASSBYVAL);
}

/* the equality that ties an EXISTS sub-select to the query around it: its outer side reads only that query, its inner side only the sub-select */
static OpExpr *
tie_of(Query *sub, Node **outer, Node **inner)
{
	OpExpr	   *tie = NULL;

	foreach_ptr(Node, q, make_ands_implicit((Expr *) sub->jointree->quals))
	{
		OpExpr	   *op = (OpExpr *) q;
		Node	   *l,
				   *r;

		if (!contain_vars_of_level(q, 1))
			continue;
		if (tie != NULL || !IsA(q, OpExpr) || list_length(op->args) != 2 || !op_mergejoinable(op->opno, exprType(linitial(op->args))))
			return NULL;
		l = linitial(op->args);
		r = lsecond(op->args);
		if (contain_vars_of_level(r, 1) && !contain_vars_of_level(l, 1))
		{
			Node	   *t = l;

			l = r;
			r = t;
		}
		if (contain_vars_of_level(r, 1) || contain_vars_of_level(l, 0) || checkExprHasSubLink(l) || checkExprHasSubLink(r))
			return NULL;
		*outer = l;
		*inner = r;
		tie = op;
	}
	return tie;
}

/* EXISTS (SELECT 1 FROM (the sub-select without the tie, giving its inner side OFFSET 0) s WHERE s.k = outer side) */
static Query *
fence_exists(Query *sub, bool apply)
{
	Node	   *outer = NULL,
			   *inner = NULL;
	OpExpr	   *tie;
	Query	   *keys,
			   *w;
	RangeTblEntry *rte;
	RangeTblRef *rtr = makeNode(RangeTblRef);
	Var		   *k;
	List	   *rest = NIL;
	bool		swapped;

	if (!simple_select(sub) || !from_has_join(sub, (Node *) sub->jointree) || (tie = tie_of(sub, &outer, &inner)) == NULL)
		return NULL;
	swapped = linitial(tie->args) != outer;
	keys = copyObject(sub);
	foreach_ptr(Node, q, make_ands_implicit((Expr *) keys->jointree->quals))
		if (!contain_vars_of_level(q, 1))
			rest = lappend(rest, q);
	keys->jointree->quals = rest == NIL ? NULL : (Node *) make_ands_explicit(rest);
	keys->targetList = list_make1(makeTargetEntry((Expr *) copyObject(inner), 1, "k", false));
	keys->limitOffset = offset_zero();
	if (contain_vars_of_level((Node *) keys, 1))
		return NULL;
	if (!apply)
		return sub;
	/* the sub-select is a level deeper */
	IncrementVarSublevelsUp((Node *) keys, 1, 1);
	k = makeVar(1, 1, exprType(inner), exprTypmod(inner), exprCollation(inner), 0);
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = keys;
	rte->eref = makeAlias("keys", list_make1(makeString("k")));
	rte->inFromCl = true;
	rtr->rtindex = 1;
	w = makeNode(Query);
	w->commandType = CMD_SELECT;
	w->querySource = QSRC_ORIGINAL;
	w->canSetTag = true;
	w->rtable = list_make1(rte);
	w->targetList = list_make1(makeTargetEntry((Expr *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(1), false, true), 1, "?column?", false));
	w->jointree = makeFromExpr(list_make1(rtr), (Node *) make_opclause(tie->opno, tie->opresulttype, tie->opretset,
																	   (Expr *) (swapped ? (Node *) outer : (Node *) k),
																	   (Expr *) (swapped ? (Node *) k : (Node *) outer), tie->opcollid, tie->inputcollid));
	return w;
}

/* the sublink as a key source: false when it is not one, or already is */
static bool
fence_sublink(SubLink *sl, bool apply)
{
	Query	   *sub,
			   *w;

	if (!IsA(sl->subselect, Query))
		return false;
	sub = castNode(Query, sl->subselect);
	if (sl->subLinkType == ANY_SUBLINK)
	{
		if (!simple_select(sub) || !from_has_join(sub, (Node *) sub->jointree) || contain_vars_of_level((Node *) sub, 1))
			return false;
		if (apply)
			sub->limitOffset = offset_zero();
		return true;
	}
	if (sl->subLinkType != EXISTS_SUBLINK || (w = fence_exists(sub, apply)) == NULL)
		return false;
	if (apply)
		sl->subselect = (Node *) w;
	return true;
}

static bool
fence_quals(Node *n, bool apply)
{
	bool		changed = false;

	if (n == NULL)
		return false;
	if (IsA(n, BoolExpr) && (((BoolExpr *) n)->boolop == AND_EXPR || ((BoolExpr *) n)->boolop == NOT_EXPR))
		foreach_ptr(Node, arg, ((BoolExpr *) n)->args)
			changed |= fence_quals(arg, apply);
	else if (IsA(n, SubLink))
		changed = fence_sublink((SubLink *) n, apply);
	return changed;
}

bool
fence_sublinks(Query *q, bool apply)
{
	if (!q->hasSubLinks || q->commandType != CMD_SELECT || q->jointree == NULL)
		return false;
	return fence_quals(q->jointree->quals, apply);
}
