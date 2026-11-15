/*
 * agg/exec/nospill.c - the nodes of a plan that hold their groups to the end and cannot spill them
 * Which of them the statement can run again for (agg/cte/rerun.c), and whether a worker of a scan passed the bound.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"

#include "agg/agg.h"
#include "agg/rows.h"
#include "agg/spec.h"
#include "agg/exec/int.h"

bool		sift_agg_overflow;

static bool
nospill_walk(PlanState *ps, void *context)
{
	const char **rerun = context;

	if (IsA(ps, CustomScanState))
	{
		if (((CustomScanState *) ps)->methods == &agg_exec_methods)
			*rerun = sift_aggspec_holds_groups(((AggExec *) ps)->spec) ? ((AggExec *) ps)->spec->rerun : NULL;
		else if (((CustomScan *) ps->plan)->methods == &sift_agg_rows_plan_methods)
			*rerun = sift_agg_rows_rerun((CustomScanState *) ps);
		if (*rerun != NULL)
			return true;
	}
	return planstate_tree_walker(ps, nospill_walk, context);
}

static bool
overflow_walk(PlanState *ps, void *context)
{
	if (IsA(ps, CustomScanState) && ((CustomScanState *) ps)->methods == &agg_exec_methods && par_overflowed((AggExec *) ps))
		return true;
	return planstate_tree_walker(ps, overflow_walk, context);
}

/* did a worker of a scan in the plan pass the bound of a node that cannot spill? (the error that reached the leader is the worker's) */
bool
sift_agg_workers_overflowed(PlanState *ps)
{
	return overflow_walk(ps, NULL);
}

/* the statement's query for planning it again, when it runs a node that holds its groups to the end and cannot spill them, and can run again; else NULL */
const char *
sift_agg_rerun_text(PlanState *ps)
{
	const char *rerun = NULL;

	nospill_walk(ps, &rerun);
	return rerun;
}
