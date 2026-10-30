/*
 * agg/cte/rewrite.c - the shared CTEs of a statement that its aggregates read
 * Which tables have a column index, which CTEs are read twice or more, and the statement
 * with those CTEs inlined.
 */
#include "postgres.h"

#include "access/relation.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planner.h"
#include "rewrite/rewriteManip.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/cte/int.h"

/*
 * The planner asks it for the same table many times in one planning (a walk of the query for each decision, and the hook plans a statement up to four times:
 * about 2,000 asks for a statement of 20 CTEs), and each ask opens the table and looks up the access method of every index. The answer holds until the table's
 * relcache entry is invalidated (CREATE or DROP INDEX, a change of the table's access method), so it is kept per table and all of it dropped on any relcache invalidation.
 */
typedef struct SourceEntry
{
	Oid			relid;
	bool		found;
} SourceEntry;

static HTAB *source_cache;

#ifdef SIFT_TESTING
int64		sift_source_lookups;	/* the asks the cache could not answer (sift._test_source_lookups) */
#endif

static void
source_cache_inval(Datum arg, Oid relid)
{
	if (source_cache != NULL)
	{
		hash_destroy(source_cache);
		source_cache = NULL;
	}
}

/* a table the aggregate scan reads: a sift_table, or a heap table with a column index */
bool
has_column_source(Oid relid)
{
	SourceEntry *e;
	Relation	rel;
	bool		found,
				seen;

	if (source_cache != NULL && (e = hash_search(source_cache, &relid, HASH_FIND, NULL)) != NULL)
		return e->found;
#ifdef SIFT_TESTING
	sift_source_lookups++;
#endif
	rel = relation_open(relid, AccessShareLock);
	found = sift_colsrc_is_table(rel->rd_rel->relam);
	foreach_oid(ix, RelationGetIndexList(rel))
		found |= sift_colsrc_is_index(get_rel_relam(ix));
	relation_close(rel, AccessShareLock);
	if (source_cache == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid), .entrysize = sizeof(SourceEntry), .hcxt = TopMemoryContext};
		static bool registered;

		if (!registered)
		{
			CacheRegisterRelcacheCallback(source_cache_inval, (Datum) 0);
			registered = true;
		}
		source_cache = hash_create("sift cache column sources", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	e = hash_search(source_cache, &relid, HASH_ENTER, &seen);
	e->found = found;
	return found;
}

/*
 * does the query select from a table that has a column index, itself, through a sub-select or through a CTE of the same WITH (which PostgreSQL
 * inlines when one query reads it)? level: how many query levels the query is below the WITH; only: the tables of that kind it must read (NIL: any)
 */
static bool
reads_indexed_table(Query *q, List *siblings, int depth, int level, const List *only)
{
	foreach_node(RangeTblEntry, rte, q->rtable)
	{
		if (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_source(rte->relid) &&
			(only == NIL || list_member_oid(only, rte->relid)))
			return true;
		if (depth <= 0)
			continue;
		if (rte->rtekind == RTE_SUBQUERY && reads_indexed_table(rte->subquery, siblings, depth - 1, level + 1, only))
			return true;
		if (rte->rtekind != RTE_CTE || rte->ctelevelsup != level)
			continue;
		foreach_node(CommonTableExpr, sib, siblings)
			if (strcmp(sib->ctename, rte->ctename) == 0 && !sib->cterecursive && IsA(sib->ctequery, Query) &&
				reads_indexed_table(castNode(Query, sib->ctequery), siblings, depth - 1, 1, only))
				return true;
	}
	return false;
}

/*
 * does the query yield the rows of an aggregate, a window or a DISTINCT, its own or of a sub-select or CTE of the WITH that it reads? Only a CTE that one query
 * reads and that is not MATERIALIZED counts: PostgreSQL inlines it, any other is computed once. level as for reads_indexed_table
 */
static bool
yields_computed_rows(Query *q, List *siblings, int depth, int level)
{
	if (q->hasAggs || q->hasWindowFuncs || q->groupClause != NIL || q->distinctClause != NIL)
		return true;
	if (depth <= 0)
		return false;
	foreach_node(RangeTblEntry, rte, q->rtable)
	{
		if (rte->rtekind == RTE_SUBQUERY && yields_computed_rows(rte->subquery, siblings, depth - 1, level + 1))
			return true;
		if (rte->rtekind != RTE_CTE || rte->ctelevelsup != level)
			continue;
		foreach_node(CommonTableExpr, sib, siblings)
			if (strcmp(sib->ctename, rte->ctename) == 0 && !sib->cterecursive && sib->cterefcount < 2 && sib->ctematerialized != CTEMaterializeAlways && IsA(sib->ctequery, Query) &&
				yields_computed_rows(castNode(Query, sib->ctequery), siblings, depth - 1, 1))
				return true;
	}
	return false;
}

/*
 * A CTE that is read twice or more and selects from a table that has a column index; siblings: the CTEs of its WITH. One that yields computed rows
 * stays a CTE: its aggregates are scans inside it, run once, where a copy in each reader would run them once per reader.
 */
static bool
shared_cte_over_column_index(CommonTableExpr *cte, List *siblings)
{
	Query	   *q = castNode(Query, cte->ctequery);

	if (cte->cterecursive || cte->cterefcount < 2 || cte->ctematerialized == CTEMaterializeNever || q->commandType != CMD_SELECT)
		return false;
	return reads_indexed_table(q, siblings, 4, 1, NIL);
}

/* does the query hold a Var of a level above the sub-select it is walked as? depth: the sub-selects below it */
static bool
outer_var_walker(Node *n, int *depth)
{
	bool		found;

	if (n == NULL)
		return false;
	if (IsA(n, Var))
		return ((Var *) n)->varlevelsup > *depth;
	if (!IsA(n, Query))
		return expression_tree_walker(n, outer_var_walker, depth);
	(*depth)++;
	found = query_tree_walker((Query *) n, outer_var_walker, depth, 0);
	(*depth)--;
	return found;
}

/* does the query, or a sub-select of it, aggregate, window or combine rows (work, not a lookup of a table)? */
static bool
computes_walker(Node *n, void *context)
{
	Query	   *q;

	if (n == NULL)
		return false;
	if (!IsA(n, Query))
		return expression_tree_walker(n, computes_walker, context);
	q = (Query *) n;
	return q->hasAggs || q->hasWindowFuncs || q->groupClause != NIL || q->distinctClause != NIL || q->setOperations != NULL ||
		query_tree_walker(q, computes_walker, context, 0);
}

/*
 * A shared CTE written inline is copied into every query that reads it, with the sub-selects of its WHERE: one that computes something (the ranking
 * of the keys with the most rows, two aggregates of the table) is then planned and run once per copy. It becomes a CTE of the same WITH, read
 * by the copies, which PostgreSQL computes once. Only an IN sub-select that reads nothing of the query around it moves.
 */
static void
hoist_sublinks(Node *quals, List **hoisted, int *nhoist)
{
	if (quals != NULL && IsA(quals, BoolExpr) && ((BoolExpr *) quals)->boolop == AND_EXPR)
	{
		foreach_ptr(Node, arg, ((BoolExpr *) quals)->args)
			hoist_sublinks(arg, hoisted, nhoist);
	}
	else if (quals != NULL && IsA(quals, SubLink))
	{
		SubLink    *sl = (SubLink *) quals;
		Query	   *sub;
		int			depth = 0;

		if (sl->subLinkType != ANY_SUBLINK || !IsA(sl->subselect, Query))
			return;
		sub = castNode(Query, sl->subselect);
		if (!computes_walker((Node *) sub, NULL) || query_tree_walker(sub, outer_var_walker, &depth, 0))
			return;
		{
			CommonTableExpr *h = makeNode(CommonTableExpr);
			Query	   *read = makeNode(Query);
			RangeTblEntry *rte = makeNode(RangeTblEntry);
			RangeTblRef *rtr = makeNode(RangeTblRef);

			/* the sub-select leaves the CTE body for the WITH: the CTEs it reads are one level nearer */
			IncrementVarSublevelsUp((Node *) sub, -1, 2);
			h->ctename = psprintf("sift_hoisted_%d", ++*nhoist);
			h->ctematerialized = CTEMaterializeAlways;
			h->ctequery = (Node *) sub;
			h->cterefcount = 1;
			h->location = -1;
			foreach_node(TargetEntry, tle, sub->targetList)
			{
				if (tle->resjunk)
					continue;
				h->ctecolnames = lappend(h->ctecolnames, makeString(tle->resname));
				h->ctecoltypes = lappend_oid(h->ctecoltypes, exprType((Node *) tle->expr));
				h->ctecoltypmods = lappend_int(h->ctecoltypmods, exprTypmod((Node *) tle->expr));
				h->ctecolcollations = lappend_oid(h->ctecolcollations, exprCollation((Node *) tle->expr));
			}
			rte->rtekind = RTE_CTE;
			rte->ctename = h->ctename;
			rte->ctelevelsup = 2;
			rte->coltypes = h->ctecoltypes;
			rte->coltypmods = h->ctecoltypmods;
			rte->colcollations = h->ctecolcollations;
			rte->eref = makeAlias(h->ctename, h->ctecolnames);
			rte->inFromCl = true;
			rtr->rtindex = 1;
			read->commandType = CMD_SELECT;
			read->querySource = QSRC_ORIGINAL;
			read->canSetTag = true;
			read->rtable = list_make1(rte);
			read->jointree = makeFromExpr(list_make1(rtr), NULL);
			for (int i = 0; i < list_length(h->ctecolnames); i++)
				read->targetList = lappend(read->targetList,
										   makeTargetEntry((Expr *) makeVar(1, i + 1, list_nth_oid(h->ctecoltypes, i), list_nth_int(h->ctecoltypmods, i),
																		   list_nth_oid(h->ctecolcollations, i), 0),
														   i + 1, strVal(list_nth(h->ctecolnames, i)), false));
			sl->subselect = (Node *) read;
			*hoisted = lappend(*hoisted, h);
		}
	}
}

/* does the query, or a sub-select or CTE of it, hold a shared CTE over a column index? With inline_them the walk goes on (it is of a copy of the query) */
bool
shared_cte_walker(Node *n, SharedCteWalk *w)
{
	List	   *ctes = NIL;

	if (n == NULL)
		return false;
	if (!IsA(n, Query))
		return expression_tree_walker(n, shared_cte_walker, w);
	foreach_node(CommonTableExpr, cte, ((Query *) n)->cteList)
	{
		Query	   *body = castNode(Query, cte->ctequery);

		if (shared_cte_over_column_index(cte, ((Query *) n)->cteList))
		{
			uint32		bit = SHARED_CTE_BIT(w->pos++);

			if (w->survey)
			{
				if (reads_indexed_table(body, ((Query *) n)->cteList, 4, 1, w->reads))
					w->reading |= bit;
			}
			else if (!w->inline_them)
				return true;
			else if ((bit == 0 || (w->only & bit)) && !yields_computed_rows(body, ((Query *) n)->cteList, 4, 1))
			{
				cte->ctematerialized = CTEMaterializeNever;
				w->names = lappend(w->names, cte->ctename);
				/* ahead of the CTE: the WITH is read in order */
				if (body->cteList == NIL && body->jointree != NULL)
					hoist_sublinks(body->jointree->quals, &ctes, &w->hoisted);
			}
		}
		ctes = lappend(ctes, cte);
	}
	if (w->inline_them)
		((Query *) n)->cteList = ctes;
	return query_tree_walker((Query *) n, shared_cte_walker, w, 0);
}

bool
has_shared_cte(Query *parse)
{
	SharedCteWalk w = {0};

	return parse->commandType == CMD_SELECT && parse->rowMarks == NIL && shared_cte_walker((Node *) parse, &w);
}
