/*
 * agg/cte.c - a CTE that several aggregates read, planned inline when that lets them run on the column index.
 *
 * PostgreSQL computes a CTE that the query names more than once exactly once and hands its rows to every reference, so the
 * aggregates over it (a time series at four granularities, each arm a GROUP BY of the same CTE) read a materialized copy and
 * never the index. The same query with the CTE written NOT MATERIALIZED is the same query: PostgreSQL itself inlines a CTE
 * with no side effects and no volatile function whenever the user says so, and refuses otherwise. This hook plans the query
 * both ways when a CTE over a table with a column index is referenced more than once, and keeps the inline plan if it has
 * a Custom Scan (SiftAgg) and costs less. Both plans are plans of the same query, so the rows are the same.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "catalog/pg_class.h"
#include "jit/jit.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/colsrc.h"

/*
 * The inline plan must cost this many times less on top of the unnest factor that agg_cost charges its scans (the plain plan is
 * priced with the 100 rows per call that PostgreSQL assumes for jsonb_to_recordset, the data has a handful): S4 on 300000 orders
 * stays on the materialized plan for the 400 store set and takes the inline one for the 4000 store set, where it is 5 times cheaper.
 */
#define CTE_INLINE_MARGIN	2.0

static planner_hook_type prev_planner_hook;

static bool
has_column_index(Oid relid)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	bool		found = false;

	foreach_oid(ix, RelationGetIndexList(rel))
		found |= sift_colsrc_is_index(get_rel_relam(ix));
	relation_close(rel, AccessShareLock);
	return found;
}

/* a CTE that is read twice or more and selects from a table that has a column index */
static bool
shared_cte_over_column_index(CommonTableExpr *cte)
{
	Query	   *q = castNode(Query, cte->ctequery);

	if (cte->cterecursive || cte->cterefcount < 2 || cte->ctematerialized != CTEMaterializeDefault || q->commandType != CMD_SELECT)
		return false;
	foreach_node(RangeTblEntry, rte, q->rtable)
		if (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_index(rte->relid))
			return true;
	return false;
}

static bool
has_shared_cte(Query *parse)
{
	if (parse->commandType != CMD_SELECT || parse->rowMarks != NIL)
		return false;
	foreach_node(CommonTableExpr, cte, parse->cteList)
		if (shared_cte_over_column_index(cte))
			return true;
	return false;
}

static bool uses_sift_agg(Plan *plan);

static bool
any_sift_agg(List *plans)
{
	foreach_ptr(Plan, p, plans)
		if (uses_sift_agg(p))
			return true;
	return false;
}

static bool
uses_sift_agg(Plan *plan)
{
	if (plan == NULL)
		return false;
	if (IsA(plan, CustomScan) && ((CustomScan *) plan)->methods == &sift_agg_plan_methods)
		return true;
	switch (nodeTag(plan))
	{
		case T_Append:
			if (any_sift_agg(((Append *) plan)->appendplans))
				return true;
			break;
		case T_MergeAppend:
			if (any_sift_agg(((MergeAppend *) plan)->mergeplans))
				return true;
			break;
		case T_SubqueryScan:
			if (uses_sift_agg(((SubqueryScan *) plan)->subplan))
				return true;
			break;
		case T_CustomScan:
			if (any_sift_agg(((CustomScan *) plan)->custom_plans))
				return true;
			break;
		default:
			break;
	}
	return uses_sift_agg(plan->lefttree) || uses_sift_agg(plan->righttree);
}

/* the CTEs (plan ids) that a CteScan of the plan, or of a CTE it reads, names */
static void cte_refs(PlannedStmt *stmt, Plan *plan, Bitmapset **refs);

static void
cte_refs_list(PlannedStmt *stmt, List *plans, Bitmapset **refs)
{
	foreach_ptr(Plan, p, plans)
		cte_refs(stmt, p, refs);
}

static void
cte_refs(PlannedStmt *stmt, Plan *plan, Bitmapset **refs)
{
	if (plan == NULL)
		return;
	if (IsA(plan, CteScan) && !bms_is_member(((CteScan *) plan)->ctePlanId, *refs))
	{
		*refs = bms_add_member(*refs, ((CteScan *) plan)->ctePlanId);
		cte_refs(stmt, list_nth(stmt->subplans, ((CteScan *) plan)->ctePlanId - 1), refs);
	}
	switch (nodeTag(plan))
	{
		case T_Append:
			cte_refs_list(stmt, ((Append *) plan)->appendplans, refs);
			break;
		case T_MergeAppend:
			cte_refs_list(stmt, ((MergeAppend *) plan)->mergeplans, refs);
			break;
		case T_SubqueryScan:
			cte_refs(stmt, ((SubqueryScan *) plan)->subplan, refs);
			break;
		case T_CustomScan:
			cte_refs_list(stmt, ((CustomScan *) plan)->custom_plans, refs);
			break;
		default:
			break;
	}
	cte_refs(stmt, plan->lefttree, refs);
	cte_refs(stmt, plan->righttree, refs);
}

/*
 * PostgreSQL charges the plan the cost of every CTE it computes, also of one that no node reads: a SiftAgg node that aggregates the
 * arms of a UNION ALL in one scan of the index (or reads the table of the CTE itself) leaves the CTE unread, and it is not run.
 */
static double
plan_cost_without_unread_ctes(PlannedStmt *stmt)
{
	Bitmapset  *refs = NULL;
	double		cost = stmt->planTree->total_cost;
	int			id = 0;

	cte_refs(stmt, stmt->planTree, &refs);
	foreach_ptr(Plan, sub, stmt->subplans)
		if (!bms_is_member(++id, refs) && sub != NULL)
			cost -= sub->total_cost;
	return Max(cost, 0.0);
}

static PlannedStmt *
plan_cte(SIFT_PLANNER_PARAMS)
{
	Query	   *inline_parse;
	PlannedStmt *plain,
			   *inlined;

	if (!sift_enable_agg || !has_shared_cte(parse))
		return prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(parse)) :
			standard_planner(SIFT_PLANNER_ARGS(parse));
	inline_parse = copyObject(parse);
	foreach_node(CommonTableExpr, cte, inline_parse->cteList)
		if (shared_cte_over_column_index(cte))
			cte->ctematerialized = CTEMaterializeNever;
	plain = prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(parse)) :
		standard_planner(SIFT_PLANNER_ARGS(parse));
	inlined = prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(inline_parse)) :
		standard_planner(SIFT_PLANNER_ARGS(inline_parse));
	elog(DEBUG1, "sift: shared CTE plain cost %.2f, inline cost %.2f", plan_cost_without_unread_ctes(plain), inlined->planTree->total_cost);
	if (inlined->planTree->total_cost * CTE_INLINE_MARGIN < plan_cost_without_unread_ctes(plain) &&
		(uses_sift_agg(inlined->planTree) || any_sift_agg(inlined->subplans)))
		return inlined;
	return plain;
}

/* does the query, or one it holds, read a table that has a column index? */
static bool
reads_column_index(Query *q)
{
	foreach_node(RangeTblEntry, rte, q->rtable)
		if ((rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_index(rte->relid)) ||
			(rte->rtekind == RTE_SUBQUERY && reads_column_index(rte->subquery)))
			return true;
	foreach_node(CommonTableExpr, cte, q->cteList)
		if (reads_column_index(castNode(Query, cte->ctequery)))
			return true;
	return false;
}

/*
 * The query planned as it is, and with its lateral sub-selects pulled up when that is possible (agg/flat.c): the flat plan wins if it
 * runs in the scan and costs less. Both are plans of the same query.
 */
static PlannedStmt *
plan_flat(SIFT_PLANNER_PARAMS)
{
	Query	   *flat = NULL;
	PlannedStmt *plain,
			   *flatplan;

	if (sift_enable_agg && parse->commandType == CMD_SELECT && sift_agg_has_flattenable_laterals(parse) && reads_column_index(parse))
	{
		flat = copyObject(parse);
		sift_agg_flatten_laterals(flat);
	}
	plain = plan_cte(SIFT_PLANNER_ARGS(parse));
	if (flat == NULL)
		return plain;
	flatplan = plan_cte(SIFT_PLANNER_ARGS(flat));
	return (uses_sift_agg(flatplan->planTree) || any_sift_agg(flatplan->subplans)) && flatplan->planTree->total_cost < plain->planTree->total_cost ?
		flatplan : plain;
}

/*
 * The cost of a SiftAgg plan turns on JIT (jit_above_cost) for work that does not use it: the node reads columns, and the nodes
 * above it handle its groups. Compiling their projections takes 3 to 5 ms of a 15 to 40 ms query.
 */
static PlannedStmt *
sift_planner(SIFT_PLANNER_PARAMS)
{
	PlannedStmt *stmt = plan_flat(SIFT_PLANNER_ARGS(parse));

	if (uses_sift_agg(stmt->planTree) || any_sift_agg(stmt->subplans))
		stmt->jitFlags = PGJIT_NONE;
	return stmt;
}

void
sift_aggcte_init(void)
{
	prev_planner_hook = planner_hook;
	planner_hook = sift_planner;
}
