/*
 * bm25/planner_post.c - the finished plan, looked at once the planner is done.
 *
 * Facts of the plan as a whole, which no path knows: the scans under an incremental sort and a LIMIT (planner_ties.c), and the matches that a
 * bitmap heap scan checks again on every row it fetches although the index gave them exactly.
 */
#include "postgres.h"

#include "nodes/plannodes.h"

#include "bm25/planner.h"
#include "bm25/planner_int.h"

/* Does a bitmap index scan that every row of the bitmap went through (not one under an OR) serve the match, exactly, with the index's analyzer? */
static bool
bitmap_serves(Plan *plan, const FuncExpr *match)
{
	const SiftCatalog *cat = sift_catalog();

	if (IsA(plan, BitmapAnd))
	{
		foreach_ptr(Plan, p, ((BitmapAnd *) plan)->bitmapplans)
			if (bitmap_serves(p, match))
				return true;
	}
	else if (IsA(plan, BitmapIndexScan))
		foreach_ptr(Node, q, ((BitmapIndexScan *) plan)->indexqualorig)
			if (IsA(q, OpExpr) && ((OpExpr *) q)->opno == cat->match_op &&
				equal(linitial(((OpExpr *) q)->args), linitial(match->args)) && equal(lsecond(((OpExpr *) q)->args), lsecond(match->args)))
				return sift_index_serves(((BitmapIndexScan *) plan)->indexid, DatumGetObjectId(((Const *) lthird(match->args))->constvalue));
	return false;
}

/*
 * `key @@> q` on an index whose analyzer is not the default is the call sift.match_idx(key, q, index), which PostgreSQL does not see as the
 * index condition it derived (an operator): the call stays a filter on every row of the bitmap heap scan, and the rows are analyzed again.
 * The index gave the rows exactly, so the call moves to the recheck, which only the rows of a lossy page go through.
 */
static void
bitmap_recheck(Plan *plan)
{
	BitmapHeapScan *scan = (BitmapHeapScan *) plan;

	if (!IsA(plan, BitmapHeapScan))
		return;
	foreach_ptr(Node, q, list_copy(plan->qual))
		if (IsA(q, FuncExpr) && ((FuncExpr *) q)->funcid == sift_catalog()->match_idx_func && IsA(lthird(((FuncExpr *) q)->args), Const) &&
			bitmap_serves(plan->lefttree, (FuncExpr *) q))
		{
			plan->qual = list_delete_ptr(plan->qual, q);
			scan->bitmapqualorig = lappend(scan->bitmapqualorig, q);
		}
}

static void
finish_walk(Plan *plan, bool scans)
{
	if (plan == NULL)
		return;
	bitmap_recheck(plan);
	if (scans)
		sift_tie_node(plan);
	finish_walk(plan->lefttree, scans);
	finish_walk(plan->righttree, scans);
	if (IsA(plan, Append))
		foreach_ptr(Plan, p, ((Append *) plan)->appendplans)
			finish_walk(p, scans);
	else if (IsA(plan, MergeAppend))
		foreach_ptr(Plan, p, ((MergeAppend *) plan)->mergeplans)
			finish_walk(p, scans);
	else if (IsA(plan, SubqueryScan))
		finish_walk(((SubqueryScan *) plan)->subplan, scans);
	else if (IsA(plan, CustomScan))
		foreach_ptr(Plan, p, ((CustomScan *) plan)->custom_plans)
			finish_walk(p, scans);
}

void
sift_finish_plan(PlannedStmt *stmt, bool scans)
{
	finish_walk(stmt->planTree, scans);
	foreach_ptr(Plan, p, stmt->subplans)
		finish_walk(p, scans);
}
