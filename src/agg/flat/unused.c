/*
 * agg/flat/unused.c - a LEFT JOIN LATERAL (.. LIMIT 1) that nothing reads
 * A left join to a sub-select of at most one row keeps every row of its left side exactly once, whatever it matches, so when no column of the
 * sub-select is read anywhere else (not by the select list, a condition, a grouping, an order, a later lateral sub-select or a join alias) the join
 * is the left side. The planner does not see it: its join removal needs the right side unique on the join conditions, and `ON true` gives none.
 * The sub-select was a lookup run for each row (or each cell) of the scan.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"

#include "agg/agg.h"
#include "agg/flat/int.h"

typedef struct Reads
{
	Index		rti;			/* the sub-select's entry */
	Index		jrti;			/* the join's entry (0: none) and the entries of the joins around it, whose aliases hold the sub-select's columns */
	Bitmapset  *around;
	int			level;
	bool		found;
} Reads;

static bool
reads_walker(Node *n, Reads *c)
{
	if (n == NULL || c->found)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;

		if (v->varlevelsup == c->level && (v->varno == c->rti || (c->jrti != 0 && v->varno == c->jrti) || bms_is_member(v->varno, c->around)))
			c->found = true;
		return false;
	}
	if (IsA(n, Query))
	{
		bool		r;

		c->level++;
		r = query_tree_walker((Query *) n, reads_walker, c, QTW_IGNORE_JOINALIASES);
		c->level--;
		return r;
	}
	return expression_tree_walker(n, reads_walker, c);
}

/* does anything of the query but the join's own condition read the sub-select, or a join around it (whose alias holds its columns)? */
static bool
is_read(Query *q, JoinExpr *j, Index rti, Bitmapset *around)
{
	Reads		c = {rti, j->rtindex, around, 0, false};
	Node	   *quals = j->quals;

	j->quals = NULL;
	/* the range table too: a later lateral sub-select or function reads the columns from its own entry; the aliases of a join hold every column of its sides */
	query_tree_walker(q, reads_walker, &c, QTW_IGNORE_JOINALIASES);
	j->quals = quals;
	return c.found;
}

/* at most one row, and no effect but its rows: LIMIT 1 (not WITH TIES), no volatile function, no row lock, no data-modifying CTE */
static bool
one_row_query(Query *sq)
{
	Const	   *limit;

	if (sq->commandType != CMD_SELECT || sq->limitOption != LIMIT_OPTION_COUNT || sq->limitCount == NULL || sq->rowMarks != NIL || sq->hasModifyingCTE ||
		contain_volatile_functions((Node *) sq))
		return false;
	/* parse analysis leaves the int8 of `LIMIT 1` as a call of the cast */
	limit = (Const *) eval_const_expressions(NULL, sq->limitCount);
	return IsA(limit, Const) && !limit->constisnull && limit->consttype == INT8OID && DatumGetInt64(limit->constvalue) == 1;
}

static Node *
drop_in(Query *q, Node *jt, Bitmapset *around, bool *changed)
{
	if (jt == NULL)
		return NULL;
	if (IsA(jt, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jt;
		ListCell   *lc;

		foreach(lc, f->fromlist)
			lfirst(lc) = drop_in(q, lfirst(lc), around, changed);
		return jt;
	}
	if (IsA(jt, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jt;
		Bitmapset  *inner = j->rtindex != 0 ? bms_add_member(bms_copy(around), j->rtindex) : around;

		j->larg = drop_in(q, j->larg, inner, changed);
		j->rarg = drop_in(q, j->rarg, inner, changed);
		if (j->jointype == JOIN_LEFT && IsA(j->rarg, RangeTblRef))
		{
			Index		rti = ((RangeTblRef *) j->rarg)->rtindex;
			RangeTblEntry *rte = rt_fetch(rti, q->rtable);

			if (rte->rtekind == RTE_SUBQUERY && one_row_query(rte->subquery) && !is_read(q, j, rti, around))
			{
				*changed = true;
				/* the planner counts the outer joins of the range table and expects each in the join tree */
				if (j->rtindex != 0)
					rt_fetch(j->rtindex, q->rtable)->jointype = JOIN_INNER;
				return j->larg;
			}
		}
	}
	return jt;
}

static bool drop_level(Query *q);

static bool
drop_sublinks(Node *n, bool *changed)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Query))
	{
		*changed |= drop_level((Query *) n);
		return false;
	}
	return expression_tree_walker(n, drop_sublinks, changed);
}

static bool
drop_level(Query *q)
{
	bool		changed = false;

	if (q->commandType != CMD_SELECT || q->rowMarks != NIL)
		return false;
	foreach_node(CommonTableExpr, cte, q->cteList)
		changed |= drop_level(castNode(Query, cte->ctequery));
	foreach_node(RangeTblEntry, rte, q->rtable)
		if (rte->rtekind == RTE_SUBQUERY && !rte->security_barrier)
			changed |= drop_level(rte->subquery);
	if (q->hasSubLinks)
		query_tree_walker(q, drop_sublinks, &changed, QTW_IGNORE_RANGE_TABLE | QTW_IGNORE_CTE_SUBQUERIES);
	q->jointree = (FromExpr *) drop_in(q, (Node *) q->jointree, NULL, &changed);
	return changed;
}

void
sift_agg_drop_unused_laterals(Query *q)
{
	drop_level(q);
}
