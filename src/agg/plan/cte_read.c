/*
 * agg/plan/cte_read.c - the readers of a shared CTE evaluate the columns of its body that may raise on every row (agg/plan/cte_read.h).
 *
 * The columns a query level keeps are those its parent reads (all of them at the top, and of a UNION ALL arm those of the union): PostgreSQL replaces an output no level above reads
 * with a NULL, and an aggregate there is not computed at all. A reader evaluates a column on every row when it has no WHERE, no LIMIT and no other relation (it takes every row of the
 * CTE) and the column is a key of its GROUP BY or the argument of an aggregate without FILTER that it keeps: an aggregate evaluates its arguments on each row it folds, wherever its
 * result is used. A reader of any other shape takes a column on every row when the queries around the body keep every row they read and the reader computes the column, as an
 * output or an aggregate argument without FILTER, in a part of its statement the planner keeps.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"

#include "agg/plan/cte_read.h"
#include "agg/safety.h"

typedef struct ColRefs
{
	Index		varno;
	int			depth;
	bool		all;
	Bitmapset  *cols;
} ColRefs;

static bool
col_refs_walker(Node *n, ColRefs *c)
{
	bool		r;

	if (n == NULL)
		return false;
	if (IsA(n, Var) && ((Var *) n)->varno == c->varno && ((Var *) n)->varlevelsup == c->depth)
	{
		if (((Var *) n)->varattno <= 0)
			c->all = true;
		else
			c->cols = bms_add_member(c->cols, ((Var *) n)->varattno);
		return false;
	}
	if (!IsA(n, Query))
		return expression_tree_walker(n, col_refs_walker, c);
	c->depth++;
	r = query_tree_walker((Query *) n, col_refs_walker, c, 0);
	c->depth--;
	return r;
}

static bool
kept(const TargetEntry *te, bool all, const Bitmapset *used)
{
	return all || te->ressortgroupref != 0 || bms_is_member(te->resno, used);
}

/* the output columns of the subquery at rti that q reads in the parts of it the planner keeps (*all: the whole row) */
static Bitmapset *
cols_read(Query *q, Index rti, bool all, const Bitmapset *used, bool *all_out)
{
	ColRefs		c = {.varno = rti};
	Query		rest = *q;

	foreach_node(TargetEntry, te, q->targetList)
		if (kept(te, all, used))
			col_refs_walker((Node *) te->expr, &c);
	rest.targetList = NIL;
	query_tree_walker(&rest, col_refs_walker, &c, 0);
	*all_out = c.all;
	return c.cols;
}

static bool
union_all(const Node *n)
{
	const SetOperationStmt *s = (const SetOperationStmt *) n;

	return IsA(n, RangeTblRef) || (s->op == SETOP_UNION && s->all && union_all(s->larg) && union_all(s->rarg));
}

static bool
is_col(const Node *e, Index rti, AttrNumber col)
{
	return IsA(e, Var) && ((const Var *) e)->varno == rti && ((const Var *) e)->varattno == col && ((const Var *) e)->varlevelsup == 0;
}

typedef struct AggArg
{
	Index		rti;
	AttrNumber	col;
} AggArg;

/* an aggregate of this level without FILTER that has the column as an argument */
static bool
agg_arg_walker(Node *n, AggArg *a)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref) && ((Aggref *) n)->agglevelsup == 0 && ((Aggref *) n)->aggfilter == NULL)
		foreach_node(TargetEntry, arg, ((Aggref *) n)->args)
			if (is_col((Node *) arg->expr, a->rti, a->col))
				return true;
	return !IsA(n, Query) && expression_tree_walker(n, agg_arg_walker, a);
}

static bool
col_evaluated(const Query *q, Index rti, AttrNumber col, bool all, const Bitmapset *used)
{
	AggArg		a = {rti, col};

	foreach_node(SortGroupClause, sgc, q->groupClause)
		if (is_col((Node *) get_sortgroupclause_expr(sgc, q->targetList), rti, col))
			return true;
	foreach_node(TargetEntry, te, q->targetList)
		if (kept(te, all, used) && agg_arg_walker((Node *) te->expr, &a))
			return true;
	return agg_arg_walker(q->havingQual, &a);
}

/* the reader at rti takes every row of the CTE and evaluates each raising column on it */
static bool
reader_covers(const Query *q, Index rti, bool all, const Bitmapset *used, const Bitmapset *raising)
{
	const List *from = q->jointree->fromlist;
	int			col = -1;

	if (!(q->hasAggs || q->groupClause != NIL) || q->groupingSets != NIL || q->setOperations != NULL || q->limitCount != NULL || q->limitOffset != NULL ||
		q->rowMarks != NIL || q->jointree->quals != NULL || list_length(from) != 1 || !IsA(linitial(from), RangeTblRef) || linitial_node(RangeTblRef, from)->rtindex != (int) rti)
		return false;
	while ((col = bms_next_member(raising, col)) >= 0)
		if (!col_evaluated(q, rti, col, all, used))
			return false;
	return true;
}

/* the reader at rti computes each raising column, for every row it reads, in an output or an aggregate argument the planner keeps */
static bool
reader_takes(const Query *q, Index rti, bool all, const Bitmapset *used, const Bitmapset *raising)
{
	Bitmapset  *cols = NULL;

	foreach_node(TargetEntry, te, q->targetList)
	{
		const Aggref *a = IsA(te->expr, Aggref) ? (const Aggref *) te->expr : NULL;
		List	   *args = a != NULL && a->aggfilter == NULL && a->agglevelsup == 0 ? a->args : list_make1(te);

		if (kept(te, all, used))
			foreach_node(TargetEntry, arg, args)
				if (IsA(arg->expr, Var) && ((Var *) arg->expr)->varno == (int) rti && ((Var *) arg->expr)->varlevelsup == 0)
					cols = bms_add_member(cols, ((Var *) arg->expr)->varattno);
	}
	return bms_is_subset(raising, cols);
}

/* the query passes on every row it reads: no condition, join or limit, and no HAVING without an aggregate (PostgreSQL makes those a WHERE) */
static bool
keeps_rows(const Query *q)
{
	const FromExpr *from = q->jointree;
	bool		keeps = q->limitCount == NULL && q->limitOffset == NULL &&
		(from == NULL || (from->quals == NULL && list_length(from->fromlist) <= 1 && (from->fromlist == NIL || IsA(linitial(from->fromlist), RangeTblRef))));

	foreach_ptr(Node, clause, make_ands_implicit((Expr *) q->havingQual))
		keeps &= contain_agg_clause(clause);
	return keeps;
}

/* a query of the tree, the body aside, drops some of the rows it reads */
static bool
drops_rows(Node *n, const Query *body)
{
	if (n == NULL || n == (const Node *) body)
		return false;
	if (!IsA(n, Query))
		return expression_tree_walker(n, drops_rows, (void *) body);
	return !keeps_rows((Query *) n) || query_tree_walker((Query *) n, drops_rows, (void *) body, 0);
}

typedef struct ReadWalk
{
	const Query *owner;
	const char *name;
	const Bitmapset *raising;
	SiftCteReaders how;
	List	   *stack;			/* the query levels from the one walked up to the top */
	int			readers;
	bool		ok;
} ReadWalk;

static void walk_level(Query *q, bool all, const Bitmapset *used, ReadWalk *rw);

/* a sub-select of an expression keeps every output it has */
static bool
sublink_walker(Node *n, ReadWalk *rw)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
	{
		walk_level((Query *) n, true, NULL, rw);
		return false;
	}
	return expression_tree_walker(n, sublink_walker, rw);
}

static void
walk_level(Query *q, bool all, const Bitmapset *used, ReadWalk *rw)
{
	Index		rti = 0;

	rw->stack = lcons(q, rw->stack);
	foreach_node(RangeTblEntry, rte, q->rtable)
	{
		bool		sall;
		const Bitmapset *sub;

		rti++;
		if (rte->rtekind == RTE_CTE && strcmp(rte->ctename, rw->name) == 0 && rte->ctelevelsup < (Index) list_length(rw->stack) &&
			list_nth(rw->stack, rte->ctelevelsup) == rw->owner)
		{
			rw->readers++;
			rw->ok = rw->ok && (rw->how == SIFT_CTE_READERS_TAKE ? reader_takes : reader_covers) (q, rti, all, used, rw->raising);
		}
		else if (rte->rtekind == RTE_SUBQUERY && q->setOperations != NULL)
			walk_level(rte->subquery, all || !union_all(q->setOperations), used, rw);
		else if (rte->rtekind == RTE_SUBQUERY)
		{
			sub = cols_read(q, rti, all, used, &sall);
			walk_level(rte->subquery, sall, sub, rw);
		}
	}
	foreach_node(CommonTableExpr, cte, q->cteList)
		walk_level(castNode(Query, cte->ctequery), true, NULL, rw);
	query_tree_walker(q, sublink_walker, rw, QTW_IGNORE_RANGE_TABLE | QTW_IGNORE_CTE_SUBQUERIES);
	rw->stack = list_delete_first(rw->stack);
}

bool
sift_cte_readers_raise_alike(Query *top, const Query *owner, const CommonTableExpr *cte, SiftCteReaders how)
{
	Query	   *body = castNode(Query, cte->ctequery);
	Query		rest = *body;
	Bitmapset  *raising = NULL;
	ReadWalk	rw = {.owner = owner,.name = cte->ctename,.how = how,.ok = true};

	rest.targetList = NIL;
	if (agg_node_may_raise((Node *) &rest))
		return false;
	foreach_node(TargetEntry, te, body->targetList)
		if (!te->resjunk && agg_node_may_raise((Node *) te->expr))
			raising = bms_add_member(raising, te->resno);
	if (raising == NULL)
		return true;
	if (how == SIFT_CTE_READERS_TAKE && drops_rows((Node *) owner, body))
		return false;
	rw.raising = raising;
	walk_level(top, true, NULL, &rw);
	return rw.ok && rw.readers == cte->cterefcount;
}
