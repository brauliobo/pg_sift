/*
 * agg/flat/array_in.c - the members of a scope as `x = ANY (ARRAY(SELECT member ..))`, written as `x IN (SELECT member ..)`
 * PostgreSQL never pulls an ARRAY sub-select up: it is an InitPlan that runs before the scan, which then sees an array parameter and no join to the scope relation. `x IN (q)` is the
 * same condition for a one-column `q` (an empty `q` is false for any x, a NULL in `q` gives NULL where nothing matches, the operator is the one of x against the element), and the
 * planner pulls it up into the semi join the scope read takes. The planner hook plans the rewritten copy beside the statement as written and keeps the cheaper plan
 * (plan_beside, as for the min() and max() sub-selects of flat/minmax.c); only a sub-select that reads a relation of a scope of the index of a table of its query is rewritten.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

typedef struct ArrayIn
{
	Query	   *level;			/* the query level the walk is in: its tables are those whose scopes the sub-select may read */
} ArrayIn;

/* a sub-select whose rows IN reads as the array holds them: no aggregate, grouping, DISTINCT, ORDER BY, LIMIT, CTE or lock (a union and a sub-select of its own are the rows of the IN form too) */
static bool
array_in_rows(const Query *q)
{
	return q->commandType == CMD_SELECT && !(q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->hasForUpdate || q->groupClause != NIL || q->groupingSets != NIL ||
											 q->havingQual != NULL || q->distinctClause != NIL || q->sortClause != NIL || q->limitCount != NULL || q->limitOffset != NULL ||
											 q->cteList != NIL || q->rowMarks != NIL);
}

/* the sub-select of `x = ANY (ARRAY(SELECT ..))` that the IN form reads as it is, NULL for any other expression */
static SubLink *
array_in_sublink(Query *level, Node *n)
{
	ScalarArrayOpExpr *op;
	SubLink    *sl;
	Query	   *sub;

	if (!IsA(n, ScalarArrayOpExpr))
		return NULL;
	op = (ScalarArrayOpExpr *) n;
	if (!op->useOr || list_length(op->args) != 2 || !IsA(lsecond(op->args), SubLink))
		return NULL;
	sl = (SubLink *) lsecond(op->args);
	if (sl->subLinkType != ARRAY_SUBLINK)
		return NULL;
	sub = castNode(Query, sl->subselect);
	if (list_length(sub->targetList) != 1 || !array_in_rows(sub) || contain_volatile_functions((Node *) sub) || !sift_agg_reads_scope(level, (Node *) sub))
		return NULL;
	return sl;
}

static Node *
array_as_in(ScalarArrayOpExpr *op, SubLink *array)
{
	Query	   *sub = castNode(Query, array->subselect);
	Expr	   *col = linitial_node(TargetEntry, sub->targetList)->expr;
	Param	   *elem = makeNode(Param);
	SubLink    *in = makeNode(SubLink);

	elem->paramkind = PARAM_SUBLINK;
	elem->paramid = 1;
	elem->paramtype = exprType((Node *) col);
	elem->paramtypmod = exprTypmod((Node *) col);
	elem->paramcollid = exprCollation((Node *) col);
	elem->location = -1;
	in->subLinkType = ANY_SUBLINK;
	in->testexpr = (Node *) make_opclause(op->opno, BOOLOID, false, linitial(op->args), (Expr *) elem, InvalidOid, op->inputcollid);
	((OpExpr *) in->testexpr)->opfuncid = op->opfuncid;
	in->operName = list_make1(makeString("="));
	in->subselect = array->subselect;
	in->location = array->location;
	return (Node *) in;
}

static bool
array_in_walker(Node *n, ArrayIn *c)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Query))
	{
		Query	   *outer = c->level;
		bool		found;

		c->level = (Query *) n;
		found = query_tree_walker((Query *) n, array_in_walker, c, QTW_IGNORE_JOINALIASES);
		c->level = outer;
		return found;
	}
	if (array_in_sublink(c->level, n) != NULL)
		return true;
	return expression_tree_walker(n, array_in_walker, c);
}

static Node *
array_in_mutator(Node *n, ArrayIn *c)
{
	SubLink    *array;

	if (n == NULL)
		return NULL;
	SIFT_SKIP_CONST_LIST(n, n);
	if (IsA(n, Query))
	{
		Query	   *outer = c->level;

		c->level = (Query *) n;
		query_tree_mutator((Query *) n, array_in_mutator, c, QTW_DONT_COPY_QUERY | QTW_IGNORE_JOINALIASES);
		c->level = outer;
		return n;
	}
	if ((array = array_in_sublink(c->level, n)) != NULL)
		return array_as_in((ScalarArrayOpExpr *) n, array);
	return expression_tree_mutator(n, array_in_mutator, c);
}

/* a copy of the query with its `= ANY (ARRAY(SELECT ..))` over the relations of a scope as `IN (SELECT ..)`; NULL where there is none */
Query *
sift_agg_array_sublinks(Query *q)
{
	ArrayIn		c = {.level = q};
	Query	   *copy;

	if (!q->hasSubLinks || !query_tree_walker(q, array_in_walker, &c, QTW_IGNORE_JOINALIASES))
		return NULL;
	copy = copyObject(q);
	c.level = copy;
	query_tree_mutator(copy, array_in_mutator, &c, QTW_DONT_COPY_QUERY | QTW_IGNORE_JOINALIASES);
	return copy;
}
