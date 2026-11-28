/*
 * agg/plan/lateral_union.c - a LATERAL UNION [ALL] of sub-selects without FROM as the sub-select of its arms' numbers (agg/plan/lateral_union.h).
 */
#include "postgres.h"

#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"

#include "agg/agg.h"
#include "agg/plan/lateral_union.h"

typedef struct Arms
{
	const SetOperationStmt *top;
	List	   *arms;			/* the Query of each arm, in the order of the set operation */
} Arms;

/* a sub-select of one row of expressions: no FROM, condition, aggregate, set-returning function, sub-select, sort or limit */
static bool
one_row_arm(const Query *a)
{
	return a->commandType == CMD_SELECT && a->setOperations == NULL && a->jointree->fromlist == NIL && a->jointree->quals == NULL && !a->hasAggs && !a->hasWindowFuncs &&
		!a->hasTargetSRFs && !a->hasSubLinks && a->groupClause == NIL && a->groupingSets == NIL && a->havingQual == NULL && a->sortClause == NIL && a->distinctClause == NIL &&
		a->limitCount == NULL && a->limitOffset == NULL && a->cteList == NIL && a->rowMarks == NIL && !contain_volatile_functions((Node *) a->targetList);
}

/* the arms of the tree n, each a one-row sub-select, under set operations of one kind */
static bool
collect_arms(const Query *q, const Node *n, Arms *s)
{
	const SetOperationStmt *op = (const SetOperationStmt *) n;

	if (IsA(n, RangeTblRef))
	{
		const RangeTblEntry *rte = rt_fetch(((const RangeTblRef *) n)->rtindex, q->rtable);

		if (rte->rtekind != RTE_SUBQUERY || !one_row_arm(rte->subquery))
			return false;
		s->arms = lappend(s->arms, rte->subquery);
		return true;
	}
	return IsA(n, SetOperationStmt) && op->op == SETOP_UNION && op->all == s->top->all && collect_arms(q, op->larg, s) && collect_arms(q, op->rarg, s);
}

/* the expressions of the output columns of arm, at the level of the sub-select that replaces the set operation, of the types of the set operation's columns; NIL when one cannot be */
static List *
arm_columns(const Query *arm, const SetOperationStmt *top)
{
	List	   *cols = NIL;
	ListCell   *type = list_head(top->colTypes),
			   *typmod = list_head(top->colTypmods);

	foreach_node(TargetEntry, te, arm->targetList)
	{
		Node	   *e;

		if (te->resjunk)
			continue;
		if (type == NULL)
			return NIL;
		e = copyObject((Node *) te->expr);
		IncrementVarSublevelsUp(e, -1, 1);
		e = coerce_to_target_type(NULL, e, exprType(e), lfirst_oid(type), lfirst_int(typmod), COERCION_IMPLICIT, COERCE_IMPLICIT_CAST, -1);
		if (e == NULL)
			return NIL;
		cols = lappend(cols, e);
		type = lnext(top->colTypes, type);
		typmod = lnext(top->colTypmods, typmod);
	}
	return type == NULL ? cols : NIL;
}

static Node *
arm_is(int arm)
{
	Var		   *i = makeVar(1, 1, INT4OID, -1, InvalidOid, 0);

	return (Node *) make_opclause(Int4EqualOperator, BOOLOID, false, (Expr *) i, (Expr *) makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(arm), false, true),
								  InvalidOid, InvalidOid);
}

/* CASE a.i WHEN 1 THEN v[0] WHEN 2 THEN v[1] .. ELSE v[n - 1] END */
static Expr *
by_arm(List *v, Oid type, Oid coll)
{
	CaseExpr   *c = makeNode(CaseExpr);

	c->casetype = type;
	c->casecollid = coll;
	c->location = -1;
	for (int k = 0; k < list_length(v) - 1; k++)
	{
		CaseWhen   *w = makeNode(CaseWhen);

		w->expr = (Expr *) arm_is(k + 1);
		w->result = list_nth(v, k);
		w->location = -1;
		c->args = lappend(c->args, w);
	}
	c->defresult = llast(v);
	return (Expr *) c;
}

/* the row of arm k is distinct from the row of each arm before it, by the equalities of the UNION (a NULL equals a NULL) */
static Expr *
distinct_from_earlier(const List *cols, int k, const SetOperationStmt *top)
{
	List	   *all = NIL;

	for (int j = 0; j < k; j++)
	{
		List	   *any = NIL;
		ListCell   *coll = list_head(top->colCollations);
		int			c = 0;

		foreach_node(SortGroupClause, g, top->groupClauses)
		{
			DistinctExpr *d = (DistinctExpr *) make_opclause(g->eqop, BOOLOID, false, list_nth(list_nth(cols, k), c), list_nth(list_nth(cols, j), c), InvalidOid,
															 lfirst_oid(coll));

			NodeSetTag(d, T_DistinctExpr);
			any = lappend(any, d);
			coll = lnext(top->colCollations, coll);
			c++;
		}
		all = lappend(all, make_orclause(any));
	}
	return make_andclause(all);
}

/* SELECT CASE a.i .. END, .. FROM (VALUES (1), (2) ..) a(i) [WHERE CASE a.i WHEN 2 THEN <row 2 is new> .. ELSE true END] */
static Query *
arm_numbers(const Query *setop, const Arms *s)
{
	Query	   *q = makeNode(Query);
	RangeTblEntry *values = makeNode(RangeTblEntry);
	RangeTblRef *ref = makeNode(RangeTblRef);
	List	   *cols = NIL;
	List	   *keep = NIL;
	int			n = list_length(s->arms);
	ListCell   *type = list_head(s->top->colTypes),
			   *coll = list_head(s->top->colCollations);
	AttrNumber	resno = 0;

	foreach_node(Query, arm, s->arms)
	{
		List	   *c = arm_columns(arm, s->top);

		if (c == NIL)
			return NULL;
		cols = lappend(cols, c);
	}
	values->rtekind = RTE_VALUES;
	for (int k = 1; k <= n; k++)
		values->values_lists = lappend(values->values_lists, list_make1(makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(k), false, true)));
	values->coltypes = list_make1_oid(INT4OID);
	values->coltypmods = list_make1_int(-1);
	values->colcollations = list_make1_oid(InvalidOid);
	values->eref = makeAlias("arms", list_make1(makeString("i")));
	values->inFromCl = true;
	q->commandType = CMD_SELECT;
	q->querySource = setop->querySource;
	q->canSetTag = true;
	q->rtable = list_make1(values);
	foreach_node(TargetEntry, te, setop->targetList)
	{
		List	   *v = NIL;

		if (te->resjunk)
			continue;
		foreach_ptr(List, c, cols)
			v = lappend(v, list_nth(c, resno));
		q->targetList = lappend(q->targetList, makeTargetEntry(by_arm(v, lfirst_oid(type), lfirst_oid(coll)), ++resno, te->resname, false));
		type = lnext(s->top->colTypes, type);
		coll = lnext(s->top->colCollations, coll);
	}
	if (!s->top->all)
	{
		keep = lappend(keep, makeBoolConst(true, false));
		for (int k = 1; k < n; k++)
			keep = lappend(keep, distinct_from_earlier(cols, k, s->top));
	}
	ref->rtindex = 1;
	q->jointree = makeFromExpr(list_make1(ref), keep != NIL ? (Node *) by_arm(keep, BOOLOID, InvalidOid) : NULL);
	return q;
}

/* the sub-select of the arms' numbers that the lateral set operation sub of rte stands for; NULL when it is not one */
static Query *
union_of_rows(const RangeTblEntry *rte)
{
	const Query *sub = rte->subquery;
	Arms		s = {0};

	if (rte->rtekind != RTE_SUBQUERY || !rte->lateral || sub->setOperations == NULL || sub->sortClause != NIL || sub->limitCount != NULL || sub->limitOffset != NULL ||
		sub->cteList != NIL || sub->rowMarks != NIL)
		return NULL;
	s.top = castNode(SetOperationStmt, sub->setOperations);
	if (!collect_arms(sub, sub->setOperations, &s) || !contain_vars_of_level((Node *) s.arms, 1))
		return NULL;
	return arm_numbers(sub, &s);
}

typedef struct Walk
{
	bool		apply;
	bool		found;
} Walk;

static bool
walk(Node *n, Walk *w)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (!IsA(n, Query))
		return expression_tree_walker(n, walk, w);
	foreach_node(RangeTblEntry, rte, ((Query *) n)->rtable)
	{
		Query	   *arms = union_of_rows(rte);

		if (arms == NULL)
			continue;
		w->found = true;
		if (w->apply)
			rte->subquery = arms;
	}
	return query_tree_walker((Query *) n, walk, w, 0);
}

bool
sift_agg_lateral_union(Query *q, bool apply)
{
	Walk		w = {.apply = apply};

	if (q->commandType != CMD_SELECT)
		return false;
	walk((Node *) q, &w);
	return w.found;
}
