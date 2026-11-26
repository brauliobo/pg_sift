/*
 * bm25/planner/ties.c - ORDER BY sift.score(ctid) DESC, key LIMIT k: the scan under the incremental sort.
 *
 * The sort groups its rows by score and reads the whole group of its k-th row, and the row after it. The ranker can hand out that group
 * at once, the k best hits and every hit that ties with the k-th, and then be at its end: the sort needs no row of a lower score (it
 * ends its group at the end of its input). That holds only for a scan whose rows go to such a sort under a LIMIT, which is a fact of the
 * finished plan, not of the scan's path, so the plan is looked at once the planner is done, and the scans it finds are told.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "common/int.h"
#include "nodes/makefuncs.h"
#include "nodes/plannodes.h"

#include "agg/hybrid/int.h"
#include "core/sift.h"
#include "bm25/planner/planner.h"
#include "bm25/planner/int.h"
#include "bm25/cscan/int.h"

/* The rows pass through unchanged: a projection, or the gather of the workers that share one ranked search (the emitter returns it whole) */
static Plan *
rows_below(Plan *plan)
{
	while (plan != NULL && plan->qual == NIL &&
		   ((IsA(plan, Result) && ((Result *) plan)->resconstantqual == NULL) || IsA(plan, Gather) || IsA(plan, GatherMerge)))
		plan = plan->lefttree;
	return plan;
}

static bool
limit_rows(const Limit *limit, int64 *rows)
{
	const Const *count = (const Const *) limit->limitCount,
			   *offset = (const Const *) limit->limitOffset;

	if (limit->limitOption != LIMIT_OPTION_COUNT || count == NULL || !IsA(count, Const) || count->constisnull ||
		(offset != NULL && (!IsA(offset, Const) || offset->constisnull)))
		return false;
	return !pg_add_s64_overflow(DatumGetInt64(count->constvalue), offset ? DatumGetInt64(offset->constvalue) : 0, rows) &&
		*rows > 0 && *rows <= LIMIT_HINT_MAX;
}

static void
set_private(CustomScan *cs, int field, Const *value)
{
	lfirst(list_nth_cell(cs->custom_private, field)) = value;
}

static int64
private_int(const CustomScan *cs, int field)
{
	return DatumGetInt64(((const Const *) list_nth(cs->custom_private, field))->constvalue);
}

/* A SiftHybridScan of direction A (or one that takes its direction at run time) reads hits the ranker verifies itself, as the scan above does: the ranker may hand out the group tied with the last */
static void
tie_hybrid_scan(CustomScan *cs, int64 rows)
{
	List	   *ints = linitial(cs->custom_private);

	if (list_nth_int(ints, SIFT_HYBP_DIR) == SIFT_HYB_BM25_TO_COL || !list_nth_int(ints, SIFT_HYBP_RANKED) || list_nth_int(ints, SIFT_HYBP_K) <= 0)
		return;
	lfirst_int(list_nth_cell(ints, SIFT_HYBP_K)) = (int) Min(rows, PG_INT32_MAX);
	lfirst_int(list_nth_cell(ints, SIFT_HYBP_TIES)) = 1;
}

/* A ranked scan whose hits are the rows it hands out: ranked by the score alone, every qual applied by the ranker */
static CustomScan *
ranked_scan(Plan *plan)
{
	CustomScan *cs;

	if (plan == NULL || !IsA(plan, CustomScan))
		return NULL;
	cs = (CustomScan *) plan;
	if (cs->methods != &sift_scan_plan_methods || private_int(cs, SIFT_PRIV_MODE) != SIFT_MODE_RANKED ||
		private_int(cs, SIFT_PRIV_K) <= 0 || private_int(cs, SIFT_PRIV_LATE) != 0 ||
		list_nth(cs->custom_private, SIFT_PRIV_KEYOPS) != NIL || cs->scan.plan.qual != NIL)
		return NULL;
	return cs;
}

static void
tie_scan(Plan *plan, int64 rows)
{
	CustomScan *cs;

	if (plan != NULL && IsA(plan, CustomScan) && ((CustomScan *) plan)->methods == &sift_hybrid_plan_methods)
	{
		tie_hybrid_scan((CustomScan *) plan, rows);
		return;
	}
	cs = ranked_scan(plan);
	if (cs == NULL)
		return;
	set_private(cs, SIFT_PRIV_K, makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(rows), false, FLOAT8PASSBYVAL));
	set_private(cs, SIFT_PRIV_TIES, makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(1), false, true));
}

/*
 * The scans of the partitions under a MergeAppend under a LIMIT of k rows each search their k best: the k-th best score of the first
 * search is the least any later partition's hit needs to beat, so the scans share a floor (an exec param holding it) that each search
 * starts above and raises.
 */
static void
floor_scans(PlannedStmt *stmt, MergeAppend *merge, int64 rows)
{
	if (!sift_parallel_scan || list_length(merge->mergeplans) < 2)
		return;
	foreach_ptr(Plan, p, merge->mergeplans)
	{
		CustomScan *cs = ranked_scan(p);

		if (cs == NULL || private_int(cs, SIFT_PRIV_K) != rows)
			return;
	}
	stmt->paramExecTypes = lappend_oid(stmt->paramExecTypes, INTERNALOID);
	foreach_ptr(Plan, p, merge->mergeplans)
		set_private((CustomScan *) p, SIFT_PRIV_GROUP,
					makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(list_length(stmt->paramExecTypes)), false, true));
}

/* One node of the finished plan (sift_finish_plan visits them all) */
void
sift_tie_node(PlannedStmt *stmt, Plan *plan)
{
	int64		rows;

	if (IsA(plan, Limit) && limit_rows((Limit *) plan, &rows))
	{
		Plan	   *below = rows_below(plan->lefttree);

		if (below != NULL && IsA(below, IncrementalSort))
			tie_scan(rows_below(below->lefttree), rows);
		else if (below != NULL && IsA(below, MergeAppend))
			floor_scans(stmt, (MergeAppend *) below, rows);
	}
}
