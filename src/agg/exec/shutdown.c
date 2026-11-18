/*
 * agg/exec/shutdown.c - the end of the statement's parallel mode for the plans a scan reads
 * The sub-plans of the joins are the executor state's, not nodes of the plan tree that ExecShutdownNode walks.
 */
#include "postgres.h"

#include "executor/executor.h"

#include "agg/agg.h"
#include "agg/spec.h"
#include "agg/exec/int.h"

/* A Gather in a sub-plan has its parallel context until the executor ends, and the end of the statement's parallel mode finds it */
void
agg_shutdown(CustomScanState *node)
{
	AggExec    *ex = (AggExec *) node;

	for (int ji = 0; ji < ex->spec->njoin; ji++)
		if (ex->spec->join[ji].subplan > 0)
			ExecShutdownNode(list_nth(node->ss.ps.state->es_subplanstates, ex->spec->join[ji].subplan - 1));
}
