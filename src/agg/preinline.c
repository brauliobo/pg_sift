/*
 * agg/preinline.c - the CTEs that one query reads, inlined once for all the plans of the statement.
 *
 * PostgreSQL inlines a CTE that is read once (SS_process_ctes) with a walk of the whole query for each of them, and the planner hook plans a
 * statement up to four times (the flat and the plain query, each with its shared CTEs and with them inlined). The same inlining, made
 * once on the tree the plans are copied from, leaves every plan the walks and copies of the others: the plans are the same, PostgreSQL finds a
 * query without these CTEs.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "rewrite/rewriteManip.h"

#include "agg/agg.h"

typedef struct InlineCte
{
	const char *name;
	Query	   *body;
	int			levelsup;
} InlineCte;

static bool
contain_dml_walker(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
	{
		Query	   *q = (Query *) n;

		return q->commandType != CMD_SELECT || q->rowMarks != NIL || query_tree_walker(q, contain_dml_walker, context, 0);
	}
	return expression_tree_walker(n, contain_dml_walker, context);
}

/* the reads of the CTE become the sub-select (inline_cte of PostgreSQL: the one reference takes the body, the outer references of it a level up) */
static bool
inline_walker(Node *n, InlineCte *c)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
	{
		c->levelsup++;
		/* the entries of the range table last, or the walk would go into the body just placed in one and find the references of its own */
		query_tree_walker((Query *) n, inline_walker, c, QTW_EXAMINE_RTES_AFTER);
		c->levelsup--;
		return false;
	}
	if (IsA(n, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) n;

		if (rte->rtekind == RTE_CTE && rte->ctelevelsup == c->levelsup && strcmp(rte->ctename, c->name) == 0)
		{
			if (c->levelsup > 0)
				IncrementVarSublevelsUp((Node *) c->body, c->levelsup, 1);
			rte->rtekind = RTE_SUBQUERY;
			rte->subquery = c->body;
			rte->security_barrier = false;
			rte->ctename = NULL;
			rte->ctelevelsup = 0;
			rte->self_reference = false;
			rte->coltypes = NIL;
			rte->coltypmods = NIL;
			rte->colcollations = NIL;
		}
		return false;
	}
	return expression_tree_walker(n, inline_walker, c);
}

static bool
inlinable(CommonTableExpr *cte)
{
	Query	   *q = castNode(Query, cte->ctequery);

	return !cte->cterecursive && cte->cterefcount == 1 && cte->ctematerialized != CTEMaterializeAlways && q->commandType == CMD_SELECT &&
		!contain_dml_walker((Node *) q, NULL) && !contain_volatile_functions((Node *) q);
}

/* the single-reference CTEs of this query level, in the order PostgreSQL takes them; an inlined CTE leaves the list, nothing reads it */
static bool
inline_level(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
	{
		Query	   *q = (Query *) n;

		for (int i = 0; i < list_length(q->cteList);)
		{
			CommonTableExpr *cte = list_nth_node(CommonTableExpr, q->cteList, i);
			InlineCte	c = {cte->ctename, castNode(Query, cte->ctequery), -1};

			if (!inlinable(cte))
			{
				i++;
				continue;
			}
			q->cteList = list_delete_nth_cell(q->cteList, i);
			inline_walker((Node *) q, &c);
		}
		return query_tree_walker(q, inline_level, context, 0);
	}
	return expression_tree_walker(n, inline_level, context);
}

void
sift_agg_preinline(Query *q)
{
	inline_level((Node *) q, NULL);
}
