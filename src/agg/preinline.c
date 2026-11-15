/*
 * agg/preinline.c - the CTEs that one query reads, inlined once for all the plans of the statement.
 *
 * PostgreSQL inlines a CTE that is read once (SS_process_ctes) with a walk of the whole query for each of them, and the planner hook plans a
 * statement up to four times (the flat and the plain query, each with its shared CTEs and with them inlined). The same inlining, made
 * once on the tree the plans are copied from, leaves every plan the walks and copies of the others: the plans are the same, PostgreSQL finds a
 * query without these CTEs.
 */
#include "postgres.h"

#include "catalog/pg_proc.h"
#include "nodes/nodeFuncs.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"

typedef struct InlineCte
{
	const char *name;
	Query	   *body;
} InlineCte;

/* the single-reference CTEs of one WITH, and how many query levels the walk is below it */
typedef struct InlineSet
{
	InlineCte  *cte;
	int			n;
	int			levelsup;
} InlineSet;

static bool
volatile_fn(Oid funcid, void *context)
{
	return func_volatile(funcid) == PROVOLATILE_VOLATILE;
}

/* does the query write (or lock) rows or call a volatile function: contain_volatile_functions' walk, with the check of the statements in the same pass */
static bool
dml_or_volatile(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
	{
		Query	   *q = (Query *) n;

		return q->commandType != CMD_SELECT || q->rowMarks != NIL || query_tree_walker(q, dml_or_volatile, context, 0);
	}
	if (check_functions_in_node(n, volatile_fn, NULL) || IsA(n, NextValueExpr))
		return true;
	return expression_tree_walker(n, dml_or_volatile, context);
}

/* the reads of the CTEs become their sub-selects (inline_cte of PostgreSQL: the one reference takes the body, the outer references of it a level up) */
static bool
inline_walker(Node *n, InlineSet *set)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
	{
		set->levelsup++;
		/* the entries of the range table last, or the walk would go into the body just placed in one and find the references of its own */
		query_tree_walker((Query *) n, inline_walker, set, QTW_EXAMINE_RTES_AFTER);
		set->levelsup--;
		return false;
	}
	if (IsA(n, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) n;

		if (rte->rtekind == RTE_CTE && rte->ctelevelsup == set->levelsup)
		{
			for (int i = 0; i < set->n; i++)
			{
				InlineCte  *c = &set->cte[i];

				if (strcmp(rte->ctename, c->name) != 0)
					continue;
				if (set->levelsup > 0)
					IncrementVarSublevelsUp((Node *) c->body, set->levelsup, 1);
				rte->rtekind = RTE_SUBQUERY;
				rte->subquery = c->body;
				rte->security_barrier = false;
				rte->ctename = NULL;
				rte->ctelevelsup = 0;
				rte->self_reference = false;
				rte->coltypes = NIL;
				rte->coltypmods = NIL;
				rte->colcollations = NIL;
				break;
			}
		}
		return false;
	}
	return expression_tree_walker(n, inline_walker, set);
}

static bool
inlinable(CommonTableExpr *cte)
{
	Query	   *q = castNode(Query, cte->ctequery);

	return !cte->cterecursive && cte->cterefcount == 1 && cte->ctematerialized != CTEMaterializeAlways && q->commandType == CMD_SELECT &&
		!dml_or_volatile((Node *) q, NULL);
}

/*
 * The single-reference CTEs of this query level, in the order PostgreSQL takes them; an inlined CTE leaves the list, nothing reads it. One walk of the
 * query places them all, where PostgreSQL walks it once for each: a CTE reads the ones before it, so its body takes those first (a walk of the body alone).
 */
static bool
inline_level(Node *n, void *context)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Query))
	{
		Query	   *q = (Query *) n;
		InlineSet	set = {.levelsup = 0};

		for (int i = 0; i < list_length(q->cteList);)
		{
			CommonTableExpr *cte = list_nth_node(CommonTableExpr, q->cteList, i);
			InlineCte	c = {cte->ctename, castNode(Query, cte->ctequery)};

			if (!inlinable(cte))
			{
				i++;
				continue;
			}
			if (set.cte == NULL)
				set.cte = palloc(sizeof(InlineCte) * list_length(q->cteList));
			if (set.n > 0)
			{
				set.levelsup = 0;
				inline_walker((Node *) c.body, &set);
			}
			set.cte[set.n++] = c;
			q->cteList = list_delete_nth_cell(q->cteList, i);
		}
		if (set.n > 0)
		{
			set.levelsup = -1;
			inline_walker((Node *) q, &set);
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
