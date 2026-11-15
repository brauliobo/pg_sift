/*
 * bm25/planner_ties.c - ORDER BY sift.score(ctid) DESC, key LIMIT k: the scan under the incremental sort.
 *
 * The sort groups its rows by score and reads the whole group of its k-th row, and the row after it. The ranker can hand out that group
 * at once, the k best hits and every hit that ties with the k-th, and then be at its end: the sort needs no row of a lower score (it
 * ends its group at the end of its input). That holds only for a scan whose rows go to such a sort under a LIMIT, which is a fact of the
 * finished plan, not of the scan's path, so the plan is looked at once the planner is done, and the scans it finds are told.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/plannodes.h"

#include "bm25/planner.h"
#include "bm25/planner_int.h"
#include "bm25/cscan_int.h"

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
	*rows = DatumGetInt64(count->constvalue) + (offset ? DatumGetInt64(offset->constvalue) : 0);
	return *rows > 0 && *rows <= LIMIT_HINT_MAX;
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

static void
tie_scan(Plan *plan, int64 rows)
{
	CustomScan *cs;

	if (plan == NULL || !IsA(plan, CustomScan))
		return;
	cs = (CustomScan *) plan;
	/* hits the scan hands out as they are: ranked by the score alone, every quals applied by the ranker */
	if (cs->methods != &sift_scan_plan_methods || private_int(cs, SIFT_PRIV_MODE) != SIFT_MODE_RANKED ||
		private_int(cs, SIFT_PRIV_K) <= 0 || private_int(cs, SIFT_PRIV_LATE) != 0 ||
		list_nth(cs->custom_private, SIFT_PRIV_KEYOPS) != NIL || cs->scan.plan.qual != NIL)
		return;
	set_private(cs, SIFT_PRIV_K, makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(rows), false, FLOAT8PASSBYVAL));
	set_private(cs, SIFT_PRIV_TIES, makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(1), false, true));
}

/* One node of the finished plan (sift_finish_plan visits them all) */
void
sift_tie_node(Plan *plan)
{
	int64		rows;

	if (IsA(plan, Limit) && limit_rows((Limit *) plan, &rows))
	{
		Plan	   *sort = rows_below(plan->lefttree);

		if (sort != NULL && IsA(sort, IncrementalSort))
			tie_scan(rows_below(sort->lefttree), rows);
	}
}
