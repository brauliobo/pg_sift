/*
 * agg/v6/v6_par_plan.c - the planner side of the parallel SiftAggV6: which strategy of v6_par.h a query gets, from numbers only (the rule is a function of its input, so a test states it), and (PostgreSQL
 * only) the Gather path that wraps the parallel-aware custom path.
 *
 * Serial when anything that makes a parallel plan unsafe or not worth its launch is true: sift.parallel_scan off, no workers, a relation that is not parallel safe, row security, a target or qual that is
 * not parallel safe, a float sum under sift.agg_exact_float (the order of the additions is the answer), fewer rows than four granules, fewer than two granules left by the zone maps. Otherwise the cost model
 * of agg/cost.h picks the number of workers (the granules left allow one worker for every SIFT_PAR_MIN_GRANULES_PER_W of them) and the groups pick the strategy.
 */
#include "core/host.h"

#include "agg/v6/v6_par.h"

static bool
serial_gate(const SiftV6ParInput *in)
{
	return !in->parallel_scan || in->max_workers <= 0 || !in->parallel_ok || in->has_rls || in->parallel_hazard ||
		(in->exact_float && in->float_sum_avg) || in->est_rows < SIFT_PAR_MIN_ROWS || in->g_eff < (double) SIFT_PAR_MIN_GRANULES_PER_W;
}

SiftV6ParStrategy
sift_v6_par_choose_strategy(const SiftV6ParInput *in, int *nworkers)
{
	const int	by_granules = (int) Min((double) in->max_workers, in->g_eff / SIFT_PAR_MIN_GRANULES_PER_W);
	int			w;
	double		ring_bytes;

	*nworkers = 0;
	if (serial_gate(in) || by_granules < 1 || (w = in->cost_workers(in->cost_arg, by_granules)) <= 0)
		return SV6P_SERIAL;
	*nworkers = Min(w, by_granules);
	if (in->has_distinct)
		return in->nkeys == 0 ? SV6P_RADIX_UNGROUPED_DIST : SV6P_RADIX_GROUPED;
	ring_bytes = in->est_groups * (double) *nworkers * in->state_bytes;
	if (in->est_groups <= (double) SIFT_PAR_TWO_STAGE_MAX_GROUPS && in->est_groups * (double) *nworkers <= in->est_rows * SIFT_PAR_TWO_STAGE_MERGE_RATIO &&
		ring_bytes <= Min(in->hash_limit * 0.5, (double) SIFT_PAR_RING_MAX_BYTES))
		return SV6P_TWO_STAGE;
	return SV6P_RADIX_GROUPED;
}
