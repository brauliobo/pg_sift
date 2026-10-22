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
#include "agg/refuse.h"

/*
 * The part of the inline plan that is the scans must cost this many times less than what the shared CTE's plan costs for it, on top of the unnest
 * factor that agg_cost charges its scans (the plain plan is priced with the 100 rows per call that PostgreSQL assumes for jsonb_to_recordset, the
 * data has a handful): S4 on 300000 orders stays on the materialized plan for the 400 store set and takes the inline one for the 4000 store set,
 * where it is 5 times cheaper. What is above the scans (a join of two aggregates, a window) is the same plan in both and not part of the test:
 * with scans S and the rest U, the plain plan costs U + P, the inline one U + S, and the margin is on P against S.
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

/* does the query, or a sub-select or CTE of it, hold a shared CTE over a column index? With *inline_them they are all written NOT MATERIALIZED (the walk is of a copy of the query, and goes on) */
static bool
shared_cte_walker(Node *n, bool *inline_them)
{
	if (n == NULL)
		return false;
	if (!IsA(n, Query))
		return expression_tree_walker(n, shared_cte_walker, inline_them);
	foreach_node(CommonTableExpr, cte, ((Query *) n)->cteList)
		if (shared_cte_over_column_index(cte))
		{
			if (!*inline_them)
				return true;
			cte->ctematerialized = CTEMaterializeNever;
		}
	return query_tree_walker((Query *) n, shared_cte_walker, inline_them, 0);
}

static bool
has_shared_cte(Query *parse)
{
	bool		inline_them = false;

	return parse->commandType == CMD_SELECT && parse->rowMarks == NIL && shared_cte_walker((Node *) parse, &inline_them);
}

static double sift_agg_cost(Plan *plan);

static double
sift_agg_cost_list(List *plans)
{
	double		cost = 0;

	foreach_ptr(Plan, p, plans)
		cost += sift_agg_cost(p);
	return cost;
}

/* what the SiftAgg nodes of the plan cost (they hold what is below them): 0 when it has none */
static double
sift_agg_cost(Plan *plan)
{
	double		cost = 0;

	if (plan == NULL)
		return 0;
	if (IsA(plan, CustomScan) && ((CustomScan *) plan)->methods == &sift_agg_plan_methods)
		return plan->total_cost;
	switch (nodeTag(plan))
	{
		case T_Append:
			cost += sift_agg_cost_list(((Append *) plan)->appendplans);
			break;
		case T_MergeAppend:
			cost += sift_agg_cost_list(((MergeAppend *) plan)->mergeplans);
			break;
		case T_SubqueryScan:
			cost += sift_agg_cost(((SubqueryScan *) plan)->subplan);
			break;
		case T_CustomScan:
			cost += sift_agg_cost_list(((CustomScan *) plan)->custom_plans);
			break;
		default:
			break;
	}
	return cost + sift_agg_cost(plan->lefttree) + sift_agg_cost(plan->righttree);
}

static bool
uses_sift_agg(Plan *plan)
{
	return sift_agg_cost(plan) > 0;
}

static bool
any_sift_agg(List *plans)
{
	return sift_agg_cost_list(plans) > 0;
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
	const char *outer_variant;
	double		plain_cost,
				scan_cost;

	if (!sift_enable_agg || !has_shared_cte(parse))
		return prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(parse)) :
			standard_planner(SIFT_PLANNER_ARGS(parse));
	inline_parse = copyObject(parse);
	shared_cte_walker((Node *) inline_parse, &(bool) {true});
	plain = prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(parse)) :
		standard_planner(SIFT_PLANNER_ARGS(parse));
	outer_variant = agg_variant_push("inline");
	inlined = prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(inline_parse)) :
		standard_planner(SIFT_PLANNER_ARGS(inline_parse));
	agg_variant = outer_variant;
	elog(DEBUG1, "sift: shared CTE plain cost %.2f, inline cost %.2f", plan_cost_without_unread_ctes(plain), inlined->planTree->total_cost);
	plain_cost = plan_cost_without_unread_ctes(plain);
	scan_cost = sift_agg_cost(inlined->planTree) + sift_agg_cost_list(inlined->subplans);
	if (scan_cost > 0 && (CTE_INLINE_MARGIN - 1.0) * scan_cost < plain_cost - inlined->planTree->total_cost)
	{
		agg_report("cte_inline", AGG_REFUSE_NONE, NULL, true);
		return inlined;
	}
	if (sift_explain_refusals && scan_cost > 0)
		agg_report("cte_inline", AGG_REFUSE_CTE_COST,
				   psprintf("inline costs %.2f (the scans %.2f), the shared CTE %.2f", inlined->planTree->total_cost, scan_cost, plain_cost), false);
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
	MemoryContext memo = sift_agg_memo_begin();
	PlannedStmt *stmt;

	PG_TRY();
	{
		stmt = plan_flat(SIFT_PLANNER_ARGS(parse));
	}
	PG_FINALLY();
	{
		sift_agg_memo_end(memo);
	}
	PG_END_TRY();
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
