/*
 * agg/par/plan.c - the planner side of the parallel SiftAggV6: which strategy of v6_par.h a query gets, from numbers only (the rule is a function of its input, so a test states it), and (PostgreSQL
 * only) the Gather path that wraps the parallel-aware custom path.
 *
 * Serial when anything that makes a parallel plan unsafe or not worth its launch is true: sift.parallel_scan off, no workers, row security, a target or qual that is
 * not parallel safe, a float sum under sift.agg_exact_float (the order of the additions is the answer), fewer than two granules left by the zone maps.
 * What a launch costs is the cost model's, from parallel_setup_cost: a small table stays serial unless the session makes the launch free. Otherwise the cost model
 * of agg/cost/model.h picks the number of workers (the granules left allow one worker for every SIFT_PAR_MIN_GRANULES_PER_W of them) and the groups pick the strategy.
 */
#include "core/host.h"

#include "agg/par/par.h"

/* why the input keeps the query serial whatever the workers would cost (NULL: nothing does); no workers asked for is no rule of the query's */
static const char *
serial_gate(const SiftV6ParInput *in)
{
	if (!in->parallel_scan)
		return "sift.parallel_scan is off";
	if (in->has_rls)
		return "row security applies to its table";
	if (in->hazard != NULL)
		return in->hazard;
	if (in->exact_float && in->float_sum_avg)
		return "sift.agg_exact_float adds a float sum in the order of the rows";
	if (!in->force && in->g_eff < (double) SIFT_PAR_MIN_GRANULES_PER_W)
		return "the granules it reads are fewer than two";
	return NULL;
}

SiftV6ParStrategy
sift_v6_par_choose_strategy(const SiftV6ParInput *in, int *nworkers, const char **why)
{
	const int	by_granules = in->force ? in->max_workers : (int) Min((double) in->max_workers, in->g_eff / SIFT_PAR_MIN_GRANULES_PER_W);
	int			w;
	double		ring_bytes;

	*nworkers = 0;
	*why = NULL;
	if (in->max_workers <= 0 || (*why = serial_gate(in)) != NULL)
		return SV6P_SERIAL;
	if (by_granules < 1 || (w = in->force ? by_granules : in->cost_workers(in->cost_arg, by_granules)) <= 0)
	{
		*why = by_granules < 1 ? "the granules it reads are fewer than two" : "the cost model expects the workers to take longer than one process";
		return SV6P_SERIAL;
	}
	*nworkers = Min(w, by_granules);
	if (in->has_distinct)
		return in->nkeys == 0 ? SV6P_RADIX_UNGROUPED_DIST : SV6P_RADIX_GROUPED;
	ring_bytes = in->est_groups * (double) *nworkers * in->state_bytes;
	if (in->est_groups <= (double) SIFT_PAR_TWO_STAGE_MAX_GROUPS && in->est_groups * (double) *nworkers <= in->est_rows * SIFT_PAR_TWO_STAGE_MERGE_RATIO &&
		ring_bytes <= Min(in->hash_limit * 0.5, (double) SIFT_PAR_RING_MAX_BYTES))
		return SV6P_TWO_STAGE;
	return SV6P_RADIX_GROUPED;
}
