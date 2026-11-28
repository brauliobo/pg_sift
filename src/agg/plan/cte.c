/*
 * agg/plan/cte.c - a CTE that several readers share, or that the statement writes MATERIALIZED, planned inline when that puts the readers in the aggregate scan: PostgreSQL keeps such a
 * CTE as one shared copy (a CTE Scan), which no scan of the index reads. The statement is planned both ways and the inline plan stays when it runs the scan and costs less. An IN
 * sub-select of the inlined body that aggregates and reads nothing of the query around it would run once per copy: it becomes a MATERIALIZED CTE of the same WITH, computed once;
 * one that restates the condition the CTE it reads keeps its rows by drops it (agg/plan/lineage.h).
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/planner.h"
#include "rewrite/rewriteManip.h"
#include "utils/memutils.h"

#include "agg/agg.h"
#include "agg/plan/cte_read.h"
#include "agg/plan/flat_rewrite.h"
#include "agg/plan/lineage.h"
#include "agg/refuse.h"
#include "agg/safety.h"
#include "agg/scope/scope.h"
#include "agg/settings.h"
#include "col/am.h"
#include "core/catalog.h"
#include "core/compat.h"
#include "core/memtrack.h"
#include "core/sql/sql.h"

static planner_hook_type prev_planner_hook;

/* does the relation hold the rows of an aggregate scan: a sift_table, or a table with a sift_col index */
static bool
scannable_rel(Oid relid)
{
	Relation	rel;
	bool		found = false;

	if (sift_relid_is_sift_table(relid))
		return true;
	rel = RelationIdGetRelation(relid);
	if (!RelationIsValid(rel))
		return false;
	if (rel->rd_rel->relhasindex)
		foreach_oid(ix, RelationGetIndexList(rel))
			found |= sift_relid_relam(ix) == sift_catalog()->col_am;
	RelationClose(rel);
	return found;
}

static bool reads_scannable_walker(Node *n, List **outer);

/* the body of the CTE an entry of the innermost query of outer (the queries around it, innermost last) names reads such a table: walked below the query of its WITH */
static bool
cte_reads_scannable(const RangeTblEntry *rte, List *outer)
{
	const int	at = list_length(outer) - 1 - (int) rte->ctelevelsup;
	List	   *with;

	if (at < 0)
		return false;
	with = list_copy_head(outer, at + 1);
	foreach_node(CommonTableExpr, cte, ((Query *) list_nth(outer, at))->cteList)
		if (strcmp(cte->ctename, rte->ctename) == 0)
			return !cte->cterecursive && reads_scannable_walker(cte->ctequery, &with);
	return false;
}

/* does the tree read a table an aggregate scan reads, directly or through a CTE it names; outer: the queries around n, innermost last */
static bool
reads_scannable_walker(Node *n, List **outer)
{
	bool		found;

	if (n == NULL)
		return false;
	if (IsA(n, Query))
	{
		*outer = lappend(*outer, n);
		found = query_tree_walker((Query *) n, reads_scannable_walker, outer, QTW_EXAMINE_RTES_BEFORE);
		*outer = list_delete_last(*outer);
		return found;
	}
	if (IsA(n, RangeTblEntry) && ((RangeTblEntry *) n)->rtekind == RTE_CTE)
		return cte_reads_scannable((RangeTblEntry *) n, *outer);
	if (IsA(n, RangeTblEntry))
		return ((RangeTblEntry *) n)->rtekind == RTE_RELATION && scannable_rel(((RangeTblEntry *) n)->relid);
	return expression_tree_walker(n, reads_scannable_walker, outer);
}

static bool
reads_scannable(Node *n, const Query *with)
{
	List	   *outer = with != NULL ? list_make1((Query *) with) : NIL;

	return reads_scannable_walker(n, &outer);
}

/* the readers of one CTE, the queries at `depth` below its WITH that name it */
typedef struct CteReads
{
	const CommonTableExpr *cte;
	Bitmapset  *raising;		/* the columns of the body that may raise */
	int			depth;
	bool		full;			/* every query around the body keeps its rows, and every reader takes every column of raising */
} CteReads;

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

/* the columns of the range table entry rti that the query computes for every row: an output that is the column, or the argument of an aggregate without FILTER */
static Bitmapset *
row_reads(const Query *q, Index rti)
{
	Bitmapset  *cols = NULL;

	foreach_node(TargetEntry, te, q->targetList)
	{
		List	   *args = IsA(te->expr, Aggref) && ((Aggref *) te->expr)->aggfilter == NULL && ((Aggref *) te->expr)->agglevelsup == 0 ? ((Aggref *) te->expr)->args : list_make1(te);

		foreach_node(TargetEntry, a, args)
			if (IsA(a->expr, Var) && ((Var *) a->expr)->varno == (int) rti && ((Var *) a->expr)->varlevelsup == 0)
				cols = bms_add_member(cols, ((Var *) a->expr)->varattno);
	}
	return cols;
}

static bool
reads_walker(Node *n, CteReads *r)
{
	Query	   *q;
	bool		found;

	if (n == NULL || n == r->cte->ctequery)
		return false;
	if (!IsA(n, Query))
		return expression_tree_walker(n, reads_walker, r);
	q = (Query *) n;
	r->full &= keeps_rows(q);
	foreach_node(RangeTblEntry, rte, q->rtable)
		if (rte->rtekind == RTE_CTE && rte->ctelevelsup == (Index) r->depth && strcmp(rte->ctename, r->cte->ctename) == 0)
			r->full &= bms_is_subset(r->raising, row_reads(q, foreach_current_index(rte) + 1));
	r->depth++;
	found = query_tree_walker(q, reads_walker, r, 0);
	r->depth--;
	return found;
}

/*
 * Inline, the plan computes only what the readers take, the shared copy every row and column of the body: the two raise the same errors when nothing of the body but its columns may raise
 * and the statement takes every row and every column that may (agg/safety.h)
 */
static bool
inline_raises_alike(const Query *q, const CommonTableExpr *cte)
{
	const Query *body = castNode(Query, cte->ctequery);
	Query		rest = *body;
	CteReads	r = {.cte = cte,.full = true};

	rest.targetList = NIL;
	foreach_node(TargetEntry, te, body->targetList)
		if (!te->resjunk && agg_node_may_raise((Node *) te->expr))
			r.raising = bms_add_member(r.raising, te->resno);
	if (r.raising != NULL)
		reads_walker((Node *) q, &r);
	return !agg_node_may_raise((Node *) &rest) && r.full;
}

/*
 * a CTE that PostgreSQL keeps as a shared copy, over a table an aggregate scan reads, that locks no row and whose inline plan raises what the copy raises (inline_raises_alike, or every
 * aggregate reader evaluates what may raise: agg/plan/cte_read.h); PostgreSQL itself refuses to inline the volatile, writing and self-referencing ones
 */
static bool
shared_cte(const CommonTableExpr *cte, const Query *owner, Query *top)
{
	const Query *body = castNode(Query, cte->ctequery);

	return !cte->cterecursive && cte->ctematerialized != CTEMaterializeNever && (cte->cterefcount > 1 || cte->ctematerialized == CTEMaterializeAlways) &&
		body->commandType == CMD_SELECT && body->rowMarks == NIL && reads_scannable(cte->ctequery, owner) &&
		(inline_raises_alike(owner, cte) || sift_cte_readers_evaluate_raising(top, owner, cte));
}

/* does a node of the sub-select read a level above it (depth is how far the walk went below the sub-select) */
static bool
outer_ref_walker(Node *n, int *depth)
{
	bool		r;

	if (n == NULL)
		return false;
	if (IsA(n, Var) && ((Var *) n)->varlevelsup > *depth)
		return true;
	if (IsA(n, PlaceHolderVar) && ((PlaceHolderVar *) n)->phlevelsup > *depth)
		return true;
	if (IsA(n, Aggref) && ((Aggref *) n)->agglevelsup > *depth)
		return true;
	if (IsA(n, GroupingFunc) && ((GroupingFunc *) n)->agglevelsup > *depth)
		return true;
	if (IsA(n, RangeTblEntry))
		return ((RangeTblEntry *) n)->rtekind == RTE_CTE && ((RangeTblEntry *) n)->ctelevelsup == *depth + 1;
	if (!IsA(n, Query))
		return expression_tree_walker(n, outer_ref_walker, depth);
	(*depth)++;
	r = query_tree_walker((Query *) n, outer_ref_walker, depth, QTW_EXAMINE_RTES_BEFORE);
	(*depth)--;
	return r;
}

/* an IN sub-select worth computing once: it aggregates, and reads nothing of the body it is in (CTEs of the WITH around the body are read where they are) */
static bool
hoistable(const SubLink *sl)
{
	Query	   *sub = castNode(Query, sl->subselect);
	int			depth = -1;

	return sl->subLinkType == ANY_SUBLINK && (sub->hasAggs || sub->groupClause != NIL) && sub->cteList == NIL && !outer_ref_walker((Node *) sub, &depth);
}

/* the CTE of the WITH that computes the sub-select, and the sub-select that reads it in its place (the body the sub-link is in is one level below the WITH) */
static CommonTableExpr *
hoist(SubLink *sl, int n)
{
	CommonTableExpr *cte = makeNode(CommonTableExpr);
	Query	   *sub = castNode(Query, sl->subselect);
	Query	   *reader = makeNode(Query);
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	RangeTblRef *rtr = makeNode(RangeTblRef);

	IncrementVarSublevelsUp((Node *) sub, -1, 1);
	cte->ctename = psprintf("sift_hoisted_%d", n);
	cte->ctematerialized = CTEMaterializeAlways;
	cte->ctequery = (Node *) sub;
	cte->cterefcount = 1;
	rte->rtekind = RTE_CTE;
	rte->ctename = cte->ctename;
	rte->ctelevelsup = 2;
	rte->inFromCl = true;
	rte->eref = makeAlias(cte->ctename, NIL);
	foreach_node(TargetEntry, tle, sub->targetList)
	{
		Oid			type = exprType((Node *) tle->expr);
		int32		typmod = exprTypmod((Node *) tle->expr);
		Oid			coll = exprCollation((Node *) tle->expr);
		char	   *name = tle->resname ? tle->resname : psprintf("c%d", tle->resno);

		if (tle->resjunk)
			continue;
		cte->ctecolnames = lappend(cte->ctecolnames, makeString(name));
		cte->ctecoltypes = lappend_oid(cte->ctecoltypes, type);
		cte->ctecoltypmods = lappend_int(cte->ctecoltypmods, typmod);
		cte->ctecolcollations = lappend_oid(cte->ctecolcollations, coll);
		reader->targetList = lappend(reader->targetList, makeTargetEntry((Expr *) makeVar(1, list_length(reader->targetList) + 1, type, typmod, coll, 0),
																		 list_length(reader->targetList) + 1, name, false));
	}
	rte->eref->colnames = cte->ctecolnames;
	rte->coltypes = cte->ctecoltypes;
	rte->coltypmods = cte->ctecoltypmods;
	rte->colcollations = cte->ctecolcollations;
	rtr->rtindex = 1;
	reader->commandType = CMD_SELECT;
	reader->querySource = QSRC_ORIGINAL;
	reader->canSetTag = true;
	reader->rtable = list_make1(rte);
	reader->jointree = makeFromExpr(list_make1(rtr), NULL);
	sl->subselect = (Node *) reader;
	return cte;
}

typedef struct CteWalk
{
	Query	   *top;			/* the statement walked */
	bool		mark;			/* write the shared CTEs NOT MATERIALIZED and hoist their IN sub-selects, or only count them */
	int			found;
	int			hoisted;
} CteWalk;

static bool cte_walker(Node *n, CteWalk *w);

/* the shared CTEs of one WITH, each after the CTEs hoisted out of its body */
static void
mark_ctes(Query *q, CteWalk *w)
{
	List	   *ctes = NIL;

	foreach_node(CommonTableExpr, cte, q->cteList)
	{
		Query	   *body = castNode(Query, cte->ctequery);

		const bool	shared = shared_cte(cte, q, w->top);

		if (shared)
			w->found++;
		if (shared && w->mark)
		{
			cte->ctematerialized = CTEMaterializeNever;
			if (body->cteList == NIL)
				sift_agg_cte_drop_restated(body->jointree->quals, ctes);
			foreach_ptr(Node, qual, make_ands_implicit((Expr *) body->jointree->quals))
				if (IsA(qual, SubLink) && hoistable((SubLink *) qual))
					ctes = lappend(ctes, hoist((SubLink *) qual, ++w->hoisted));
		}
		ctes = lappend(ctes, cte);
	}
	if (w->mark)
		q->cteList = ctes;
}

static bool
cte_walker(Node *n, CteWalk *w)
{
	if (n == NULL)
		return false;
	if (!IsA(n, Query))
		return expression_tree_walker(n, cte_walker, w);
	mark_ctes((Query *) n, w);
	return query_tree_walker((Query *) n, cte_walker, w, 0);
}

static bool
agg_scan_in(const Plan *p)
{
	if (p == NULL)
		return false;
	if (IsA(p, CustomScan) && strncmp(((const CustomScan *) p)->methods->CustomName, "SiftAgg", 7) == 0)
		return true;
	if (IsA(p, CustomScan))
		foreach_ptr(Plan, c, ((const CustomScan *) p)->custom_plans)
			if (agg_scan_in(c))
				return true;
	if (IsA(p, Append))
		foreach_ptr(Plan, c, ((const Append *) p)->appendplans)
			if (agg_scan_in(c))
				return true;
	if (IsA(p, MergeAppend))
		foreach_ptr(Plan, c, ((const MergeAppend *) p)->mergeplans)
			if (agg_scan_in(c))
				return true;
	if (IsA(p, SubqueryScan) && agg_scan_in(((const SubqueryScan *) p)->subplan))
		return true;
	return agg_scan_in(p->lefttree) || agg_scan_in(p->righttree);
}

/* does the statement run an aggregate scan, in its plan or in a subplan (a CTE, an InitPlan) */
static bool
runs_agg_scan(const PlannedStmt *stmt)
{
	if (agg_scan_in(stmt->planTree))
		return true;
	foreach_ptr(Plan, sp, stmt->subplans)
		if (agg_scan_in(sp))
			return true;
	return false;
}

static PlannedStmt *
plan_query(SIFT_PLANNER_PARAMS)
{
	return prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(parse)) : standard_planner(SIFT_PLANNER_ARGS(parse));
}

/*
 * The statement with its unread laterals dropped and its json_build_object calls on sift.json_build_object planned, or, when it has shared CTEs or the rewrites of flat_rewrite.c or
 * of agg/scope/rewrite.c apply, the plan with the CTEs inline and the rewrites made if that one runs the aggregate scan (a scope read, for the rewrites of a scope alone), and costs less
 * unless it only moved HAVING conditions to WHERE; the loser's memory goes with its context
 */
static PlannedStmt *
plan_statement(SIFT_PLANNER_PARAMS)
{
	CteWalk		w = {.top = parse};
	MemoryContext caller = CurrentMemoryContext;
	MemoryContext cx;
	Query	   *alt;
	PlannedStmt *plain,
			   *rewritten;
	bool		priced,
				scoped,
				rows;

	if (!sift_enable_agg || !reads_scannable((Node *) parse, NULL))
		return plan_query(SIFT_PLANNER_ARGS(parse));
	if (parse->commandType == CMD_SELECT)
		sift_agg_drop_unused_laterals(parse);
	if (parse->commandType == CMD_SELECT && sift_json_build_object_calls(parse))
		sift_json_build_object_swap(parse);
	cte_walker((Node *) parse, &w);
	priced = w.found > 0 || sift_agg_flat_decorrelate(parse, false);
	scoped = !priced && sift_agg_scope && sift_v6_scope_rewrite(parse, false);
	priced |= scoped;
	rows = sift_agg_having_to_where(parse, false);
	if (!priced && !rows)
		return plan_query(SIFT_PLANNER_ARGS(parse));
	alt = copyObject(parse);
	w.mark = true;
	w.top = alt;
	cte_walker((Node *) alt, &w);
	sift_agg_having_to_where(alt, true);
	sift_agg_flat_decorrelate(alt, true);
	if (sift_agg_scope)
		sift_v6_scope_rewrite(alt, true);
	plain = plan_query(SIFT_PLANNER_ARGS(parse));
	cx = sift_mem_allocset_create(caller, "sift rewritten plan");
	MemoryContextSwitchTo(cx);
	rewritten = plan_query(SIFT_PLANNER_ARGS(alt));
	MemoryContextSwitchTo(caller);
	if (runs_agg_scan(rewritten) && (!scoped || sift_v6_scope_planned(rewritten)) &&
		(!priced || sift_agg_force || rewritten->planTree->total_cost < plain->planTree->total_cost))
		return rewritten;
	MemoryContextDelete(cx);
	return plain;
}

/* a planning validates the statistics of each column index once, and reads its directory only when it changed (sift_col_plan_begin) */
static PlannedStmt *
agg_planner(SIFT_PLANNER_PARAMS)
{
	const bool	col_plan = sift_col_plan_begin();
	PlannedStmt *stmt;

	PG_TRY();
	{
		stmt = plan_statement(SIFT_PLANNER_ARGS(parse));
		if (col_plan)
			agg_explain_planned();
	}
	PG_FINALLY();
	{
		sift_col_plan_end(col_plan);
	}
	PG_END_TRY();
	return stmt;
}

void
sift_agg_cte_init(void)
{
	prev_planner_hook = planner_hook;
	planner_hook = agg_planner;
}
