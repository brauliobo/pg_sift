/*
 * agg/cte/rewrite.c - the shared CTEs of a statement that its aggregates read
 * Which tables have a column index, which CTEs are read twice or more, and the statement
 * with those CTEs inlined.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

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
	bool		reads_known;
	bool			all_read;	/* a sift_table: the scan reads every column */
	Bitmapset  *reads;			/* the columns of the table that some column index holds, as pull_varattnos numbers them */
} SourceEntry;

static HTAB *source_cache;
static MemoryContext source_cx;	/* the hash and the bitmaps of its entries: deleted together */

#ifdef SIFT_TESTING
int64		sift_prepass_nodes;	/* the nodes the rewrite passes visited (sift._test_prepass_nodes) */
int64		sift_source_lookups;	/* the asks the cache could not answer (sift._test_source_lookups) */
#endif

static void
source_cache_inval(Datum arg, Oid relid)
{
	if (source_cache != NULL)
	{
		MemoryContextDelete(source_cx);
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
		HASHCTL		ctl = {.keysize = sizeof(Oid), .entrysize = sizeof(SourceEntry)};
		static bool registered;

		if (!registered)
		{
			CacheRegisterRelcacheCallback(source_cache_inval, (Datum) 0);
			registered = true;
		}
		source_cx = AllocSetContextCreate(TopMemoryContext, "sift cache column sources", ALLOCSET_SMALL_SIZES);
		ctl.hcxt = source_cx;
		source_cache = hash_create("sift cache column sources", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	e = hash_search(source_cache, &relid, HASH_ENTER, &seen);
	e->found = found;
	e->reads_known = false;
	e->reads = NULL;
	return found;
}

/*
 * Can the scan read these columns of the table (as pull_varattnos numbers them): does some column index of the table hold each of them? A column no
 * index holds is read from the heap only by a top-N for its page, never by an aggregate or a filter. Kept with the answer of has_column_source.
 */
bool
column_source_reads(Oid relid, Bitmapset *attnos)
{
	SourceEntry *e;

	if (!has_column_source(relid))
		return false;
	e = hash_search(source_cache, &relid, HASH_FIND, NULL);
	if (!e->reads_known)
	{
		Relation	rel = relation_open(relid, AccessShareLock);
		MemoryContext old = MemoryContextSwitchTo(source_cx);

		e->all_read = sift_colsrc_is_table(rel->rd_rel->relam);
		foreach_oid(ix, RelationGetIndexList(rel))
		{
			Relation	idx;

			if (!sift_colsrc_is_index(get_rel_relam(ix)))
				continue;
			idx = index_open(ix, AccessShareLock);
			pull_varattnos((Node *) sift_colsrc_layout(idx)->attexprs, 1, &e->reads);
			index_close(idx, AccessShareLock);
		}
		MemoryContextSwitchTo(old);
		relation_close(rel, AccessShareLock);
		e->reads_known = true;
	}
	return e->all_read || bms_is_subset(attnos, e->reads);
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

/* the types whose equal values are the same value: a condition on one holds for the other */
static bool
exact_key_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID || t == UUIDOID;
}

/* does the query, or a sub-select of it, read a CTE? */
SIFT_WALKER(cte_ref_walker, void *, context,
	if (IsA(n, RangeTblEntry))
		return ((RangeTblEntry *) n)->rtekind == RTE_CTE;
	if (IsA(n, Query))
		return query_tree_walker((Query *) n, cte_ref_walker, context, QTW_EXAMINE_RTES_BEFORE);)

/* a filter of rows, nothing more: the output rows are rows of the FROM kept by the WHERE (a plain DISTINCT drops duplicates only) */
static bool
plain_filter(Query *q)
{
	return q->commandType == CMD_SELECT && q->setOperations == NULL && q->cteList == NIL && q->jointree != NULL && !q->hasAggs && !q->hasWindowFuncs &&
		!q->hasTargetSRFs && !q->hasDistinctOn && q->groupClause == NIL && q->groupingSets == NIL && q->limitCount == NULL && q->limitOffset == NULL &&
		q->rowMarks == NIL;
}

typedef struct KeyWalk
{
	Var		   *key;			/* the one column the condition reads */
	bool		ok;
} KeyWalk;

static Node *
key_mutator(Node *n, KeyWalk *w)
{
	int			depth = 0;

	if (n == NULL)
		return NULL;
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;
		Var		   *placeholder = copyObject(v);

		if (v->varlevelsup != 0 || (w->key != NULL && (v->varno != w->key->varno || v->varattno != w->key->varattno)))
			w->ok = false;
		else if (w->key == NULL)
			w->key = v;
		placeholder->varno = 1;
		placeholder->varattno = 1;
		return (Node *) placeholder;
	}
	if (IsA(n, Query))
	{
		/* a sub-select is the same condition when it reads the same tables wherever it stands: no column of the query around it, no CTE (the name could be another CTE there) */
		if (query_tree_walker((Query *) n, outer_var_walker, &depth, 0) || cte_ref_walker(n, NULL))
			w->ok = false;
		return n;
	}
	return expression_tree_mutator(n, key_mutator, w);
}

/* the condition as a function of its one column, to compare with the same condition on another column; NULL when it reads other columns or is volatile */
static Node *
key_condition(Node *cond, Var **key)
{
	KeyWalk		w = {*key, true};
	Node	   *norm;

	if (contain_volatile_functions(cond))
		return NULL;
	norm = key_mutator(cond, &w);
	if (!w.ok || w.key == NULL)
		return NULL;
	*key = w.key;
	return norm;
}

/* the conditions an inner join tree applies to its rows; false with an outer join in it (its null-extended rows pass no condition) */
static bool
inner_quals(Node *jt, List **quals)
{
	if (IsA(jt, RangeTblRef))
		return true;
	if (IsA(jt, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jt;

		foreach_ptr(Node, item, f->fromlist)
			if (!inner_quals(item, quals))
				return false;
		*quals = list_concat(*quals, make_ands_implicit((Expr *) f->quals));
		return true;
	}
	if (IsA(jt, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jt;

		if (j->jointype != JOIN_INNER || !inner_quals(j->larg, quals) || !inner_quals(j->rarg, quals))
			return false;
		*quals = list_concat(*quals, make_ands_implicit((Expr *) j->quals));
		return true;
	}
	return false;
}

/* the columns that an equality of the quals ties to the columns of same, every row long */
static void
equal_columns(List **same, List *quals)
{
	for (bool grown = true; grown;)
	{
		grown = false;
		foreach_ptr(Node, qual, quals)
		{
			OpExpr	   *op = (OpExpr *) qual;
			Var		   *a,
					   *b;

			if (!IsA(qual, OpExpr) || list_length(op->args) != 2 || !op_mergejoinable(op->opno, exprType(linitial(op->args))))
				continue;
			a = (Var *) linitial(op->args);
			b = (Var *) lsecond(op->args);
			if (!IsA(a, Var) || !IsA(b, Var) || a->varlevelsup != 0 || b->varlevelsup != 0 || !exact_key_type(a->vartype) || !exact_key_type(b->vartype))
				continue;
			if (list_member(*same, a) != list_member(*same, b))
			{
				*same = lappend(*same, list_member(*same, a) ? b : a);
				grown = true;
			}
		}
	}
}

static bool key_filtered(List *ctes, Query *q, int level, AttrNumber col, Node *want, int depth);

/*
 * Does every row of the column col of the entry satisfy want? level: the ctelevelsup that the CTEs of ctes (the WITH the entry is read in, those
 * before the reading CTE) have for the query that holds the entry. The entry is a CTE of ctes, or a sub-select (a CTE that PostgreSQL would inline is
 * one already).
 */
static bool
rte_filtered(List *ctes, RangeTblEntry *rte, int level, AttrNumber col, Node *want, int depth)
{
	int			pos = 0;

	if (rte->rtekind == RTE_SUBQUERY)
		return key_filtered(ctes, rte->subquery, level + 1, col, want, depth);
	if (rte->rtekind != RTE_CTE || rte->ctelevelsup != level)
		return false;
	foreach_node(CommonTableExpr, p, ctes)
	{
		if (strcmp(p->ctename, rte->ctename) == 0)
			return IsA(p->ctequery, Query) && key_filtered(list_copy_head(ctes, pos), castNode(Query, p->ctequery), 1, col, want, depth);
		pos++;
	}
	return false;
}

/*
 * Does every row of the column col of the query satisfy want (a key_condition)? It does when the query keeps its rows by that condition on the
 * column, or on a column equal to it, or by an equality with a column of an entry whose rows satisfy it. depth bounds the entries followed.
 */
static bool
key_filtered(List *ctes, Query *q, int level, AttrNumber col, Node *want, int depth)
{
	TargetEntry *tle;
	List	   *quals = NIL;
	List	   *same;

	if (depth <= 0 || !plain_filter(q) || col > list_length(q->targetList) || !inner_quals((Node *) q->jointree, &quals))
		return false;
	tle = list_nth_node(TargetEntry, q->targetList, col - 1);
	if (tle->resjunk || !IsA(tle->expr, Var) || ((Var *) tle->expr)->varlevelsup != 0)
		return false;
	same = list_make1(tle->expr);
	equal_columns(&same, quals);
	foreach_ptr(Var, v, same)
	{
		foreach_ptr(Node, qual, quals)
		{
			Var		   *key = v;
			Node	   *have = key_condition(qual, &key);

			if (have != NULL && equal(have, want))
				return true;
		}
		if (rte_filtered(ctes, rt_fetch(v->varno, q->rtable), level, v->varattno, want, depth - 1))
			return true;
	}
	return false;
}

/*
 * A shared CTE written inline may restate, in an IN sub-select over a CTE, the condition that CTE already keeps its rows by: "entities" is the table
 * joined to the key list "ids" (a DISTINCT of the ids of the table that satisfy Q), and the sub-select reads "entities" again with the same Q. Each
 * Q conjunct that the rows of the CTE satisfy by their lineage (key_filtered) is dropped from the sub-select, which then reads the CTE alone instead
 * of running Q once more. Only top-level conjuncts of a WHERE, a sub-select that is a plain filter of the one CTE it reads, and a Q that is
 * stable, uncorrelated and reads no CTE: a CTE found by its level is the one of this WITH, one of the same name inside Q could be another.
 */
static void
drop_restated(Node *quals, List *ctes)
{
	SubLink    *sl;
	Query	   *sub;
	RangeTblRef *ref;
	RangeTblEntry *rte;
	List	   *keep = NIL;
	bool		dropped = false;

	if (quals != NULL && IsA(quals, BoolExpr) && ((BoolExpr *) quals)->boolop == AND_EXPR)
	{
		foreach_ptr(Node, arg, ((BoolExpr *) quals)->args)
			drop_restated(arg, ctes);
		return;
	}
	if (quals == NULL || !IsA(quals, SubLink) || ((SubLink *) quals)->subLinkType != ANY_SUBLINK || !IsA(((SubLink *) quals)->subselect, Query))
		return;
	sl = (SubLink *) quals;
	sub = castNode(Query, sl->subselect);
	if (!plain_filter(sub) || list_length(sub->jointree->fromlist) != 1 || !IsA(linitial(sub->jointree->fromlist), RangeTblRef))
		return;
	ref = linitial_node(RangeTblRef, sub->jointree->fromlist);
	rte = rt_fetch(ref->rtindex, sub->rtable);
	foreach_ptr(Node, cond, make_ands_implicit((Expr *) sub->jointree->quals))
	{
		Var		   *key = NULL;
		Node	   *want = key_condition(cond, &key);

		if (want != NULL && key->varno == ref->rtindex && rte_filtered(ctes, rte, 2, key->varattno, want, 4))
			dropped = true;
		else
			keep = lappend(keep, cond);
	}
	if (dropped)
		sub->jointree->quals = keep == NIL ? NULL : (Node *) make_ands_explicit(keep);
}

/* does the query, or a sub-select or CTE of it, hold a shared CTE over a column index? With inline_them the walk goes on (it is of a copy of the query) */
bool
shared_cte_walker(Node *n, SharedCteWalk *w)
{
	List	   *ctes = NIL;

	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
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
				{
					drop_restated(body->jointree->quals, ctes);
					hoist_sublinks(body->jointree->quals, &ctes, &w->hoisted);
				}
			}
		}
		ctes = lappend(ctes, cte);
	}
	if (w->inline_them)
		((Query *) n)->cteList = ctes;
	return query_tree_walker((Query *) n, shared_cte_walker, w, 0);
}

/* can a CTE, sub-select or sub-link be anywhere below the query? Only through its WITH, a sub-select or CTE of its FROM or a sub-link of its own */
bool
query_nests(Query *q)
{
	if (q->cteList != NIL || q->hasSubLinks)
		return true;
	foreach_node(RangeTblEntry, rte, q->rtable)
		if (rte->rtekind == RTE_SUBQUERY || rte->rtekind == RTE_CTE)
			return true;
	return false;
}

bool
has_shared_cte(Query *parse)
{
	SharedCteWalk w = {0};

	return parse->commandType == CMD_SELECT && parse->rowMarks == NIL && query_nests(parse) && shared_cte_walker((Node *) parse, &w);
}
