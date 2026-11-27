/*
 * bm25/planner/post.c - the finished plan, looked at once the planner is done.
 *
 * Facts of the plan as a whole, which no path knows: the scans under an incremental sort and a LIMIT (planner_ties.c), and the matches that a
 * bitmap heap scan checks again on every row it fetches although the index gave them exactly, and the scores that the RETURNING of an UPDATE or DELETE reads.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "nodes/makefuncs.h"
#include "nodes/plannodes.h"

#include "agg/win/window.h"
#include "bm25/planner/planner.h"
#include "core/catalog.h"
#include "bm25/planner/int.h"

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
	List	   *served = NIL;

	if (!IsA(plan, BitmapHeapScan))
		return;
	foreach_ptr(Node, q, plan->qual)
		if (IsA(q, FuncExpr) && ((FuncExpr *) q)->funcid == sift_catalog()->match_idx_func && IsA(lthird(((FuncExpr *) q)->args), Const) &&
			bitmap_serves(plan->lefttree, (FuncExpr *) q))
			served = lappend(served, q);
	foreach_ptr(Node, q, served)
	{
		plan->qual = list_delete_ptr(plan->qual, q);
		scan->bitmapqualorig = lappend(scan->bitmapqualorig, q);
	}
}

typedef struct ReturningScores
{
	ModifyTable *modify;
	List	   *seen;			/* the scores met in RETURNING, in order: the order of the junk columns (SIFT_RETURNING_SCORE) */
} ReturningScores;

/* The junk column of the statement's plan that holds the score of the n-th one of RETURNING */
static TargetEntry *
score_column(const ModifyTable *mt, int n)
{
	char	   *name = psprintf(SIFT_RETURNING_SCORE "%d", n);

	foreach_node(TargetEntry, tle, mt->plan.lefttree->targetlist)
		if (tle->resjunk && tle->resname != NULL && strcmp(tle->resname, name) == 0)
			return tle;
	SIFT_INVARIANT(false, "sift: the score %d of a RETURNING list is not in the plan of its statement", n);
	return NULL;
}

/* A score of RETURNING is the column of the plan that the scan computed it in: the score of the row that the statement changes */
static Node *
returning_mutator(Node *node, ReturningScores *rs)
{
	const SiftCatalog *cat = sift_catalog();

	if (node == NULL)
		return NULL;
	if (IsA(node, FuncExpr) && (((FuncExpr *) node)->funcid == cat->score_func || ((FuncExpr *) node)->funcid == cat->max_score_func))
	{
		int			n = 0;

		foreach_ptr(Node, met, rs->seen)
		{
			if (equal(met, node))
				break;
			n++;
		}
		if (n == list_length(rs->seen))
			rs->seen = lappend(rs->seen, node);
		return (Node *) makeVar(OUTER_VAR, score_column(rs->modify, n)->resno, FLOAT4OID, -1, InvalidOid, 0);
	}
	return expression_tree_mutator(node, returning_mutator, rs);
}

static void
returning_scores(ModifyTable *mt)
{
	List	   *lists = NIL;

	foreach_ptr(List, rlist, mt->returningLists)
	{
		ReturningScores rs = {mt, NIL};

		lists = lappend(lists, expression_tree_mutator((Node *) rlist, returning_mutator, &rs));
	}
	mt->returningLists = lists;
}

static bool
has_score_column(const ModifyTable *mt)
{
	foreach_node(TargetEntry, tle, mt->plan.lefttree->targetlist)
		if (tle->resjunk && tle->resname != NULL && strncmp(tle->resname, SIFT_RETURNING_SCORE, strlen(SIFT_RETURNING_SCORE)) == 0)
			return true;
	return false;
}

static void
finish_walk(PlannedStmt *stmt, Plan *plan, bool scans)
{
	if (plan == NULL)
		return;
	bitmap_recheck(plan);
	sift_v6_window_finish_tlist(plan);
	if (IsA(plan, ModifyTable) && ((ModifyTable *) plan)->returningLists != NIL && has_score_column((ModifyTable *) plan))
		returning_scores((ModifyTable *) plan);
	if (scans)
		sift_tie_node(stmt, plan);
	finish_walk(stmt, plan->lefttree, scans);
	finish_walk(stmt, plan->righttree, scans);
	if (IsA(plan, Append))
		foreach_ptr(Plan, p, ((Append *) plan)->appendplans)
			finish_walk(stmt, p, scans);
	else if (IsA(plan, MergeAppend))
		foreach_ptr(Plan, p, ((MergeAppend *) plan)->mergeplans)
			finish_walk(stmt, p, scans);
	else if (IsA(plan, SubqueryScan))
		finish_walk(stmt, ((SubqueryScan *) plan)->subplan, scans);
	else if (IsA(plan, CustomScan))
		foreach_ptr(Plan, p, ((CustomScan *) plan)->custom_plans)
			finish_walk(stmt, p, scans);
}

void
sift_finish_plan(PlannedStmt *stmt, bool scans)
{
	if (!sift_db_has_extension())
		return;
	finish_walk(stmt, stmt->planTree, scans);
	foreach_ptr(Plan, p, stmt->subplans)
		finish_walk(stmt, p, scans);
}
