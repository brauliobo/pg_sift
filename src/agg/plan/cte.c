/*
 * agg/plan/cte.c - a CTE that several readers share, or that the statement writes MATERIALIZED, planned inline when that puts the readers in the aggregate scan: PostgreSQL keeps such a
 * CTE as one shared copy (a CTE Scan), which no scan of the index reads. The statement is planned both ways and the inline plan stays when it runs the scan and costs less. An IN
 * sub-select of the inlined body that aggregates and reads nothing of the query around it would run once per copy: it becomes a MATERIALIZED CTE of the same WITH, computed once.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planner.h"
#include "rewrite/rewriteManip.h"
#include "utils/memutils.h"

#include "agg/agg.h"
#include "agg/plan/flat_rewrite.h"
#include "agg/settings.h"
#include "core/catalog.h"
#include "core/compat.h"
#include "core/memtrack.h"

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

static bool
reads_scannable_walker(Node *n, void *cx)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
		return query_tree_walker((Query *) n, reads_scannable_walker, cx, QTW_EXAMINE_RTES_BEFORE);
	if (IsA(n, RangeTblEntry))
		return ((RangeTblEntry *) n)->rtekind == RTE_RELATION && scannable_rel(((RangeTblEntry *) n)->relid);
	return expression_tree_walker(n, reads_scannable_walker, cx);
}

/* a CTE that PostgreSQL keeps as a shared copy, over a table an aggregate scan reads, that locks no row; PostgreSQL itself refuses to inline the volatile, writing and self-referencing ones */
static bool
shared_cte(const CommonTableExpr *cte)
{
	const Query *body = castNode(Query, cte->ctequery);

	return !cte->cterecursive && cte->ctematerialized != CTEMaterializeNever && (cte->cterefcount > 1 || cte->ctematerialized == CTEMaterializeAlways) &&
		body->commandType == CMD_SELECT && body->rowMarks == NIL && reads_scannable_walker(cte->ctequery, NULL);
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

		if (shared_cte(cte))
			w->found++;
		if (shared_cte(cte) && w->mark)
		{
			cte->ctematerialized = CTEMaterializeNever;
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
 * The statement with its unread laterals dropped planned, or, when it has shared CTEs or the rewrites of flat_rewrite.c apply, the plan with the CTEs inline and the rewrites made if
 * that one runs the aggregate scan, and costs less unless it only moved HAVING conditions to WHERE; the loser's memory goes with its context
 */
static PlannedStmt *
agg_planner(SIFT_PLANNER_PARAMS)
{
	CteWalk		w = {0};
	MemoryContext caller = CurrentMemoryContext;
	MemoryContext cx;
	Query	   *alt;
	PlannedStmt *plain,
			   *rewritten;
	bool		priced,
				rows;

	if (!sift_enable_agg || !sift_db_has_extension() || !reads_scannable_walker((Node *) parse, NULL))
		return plan_query(SIFT_PLANNER_ARGS(parse));
	if (parse->commandType == CMD_SELECT)
		sift_agg_drop_unused_laterals(parse);
	cte_walker((Node *) parse, &w);
	priced = w.found > 0 || sift_agg_flat_decorrelate(parse, false);
	rows = sift_agg_having_to_where(parse, false);
	if (!priced && !rows)
		return plan_query(SIFT_PLANNER_ARGS(parse));
	alt = copyObject(parse);
	w.mark = true;
	cte_walker((Node *) alt, &w);
	sift_agg_having_to_where(alt, true);
	sift_agg_flat_decorrelate(alt, true);
	plain = plan_query(SIFT_PLANNER_ARGS(parse));
	cx = sift_mem_allocset_create(caller, "sift rewritten plan");
	MemoryContextSwitchTo(cx);
	rewritten = plan_query(SIFT_PLANNER_ARGS(alt));
	MemoryContextSwitchTo(caller);
	if (runs_agg_scan(rewritten) && (!priced || sift_agg_force || rewritten->planTree->total_cost < plain->planTree->total_cost))
		return rewritten;
	MemoryContextDelete(cx);
	return plain;
}

void
sift_agg_cte_init(void)
{
	prev_planner_hook = planner_hook;
	planner_hook = agg_planner;
}
