/*
 * agg/v6/v6_par_path.c - the parallel path of SiftAggV6: the planner's numbers for a query go to sift_v6_par_choose_strategy (v6_par_plan.c) and, when it picks a parallel strategy, a Gather over a
 * parallel-aware SiftAggV6 is added next to the serial path, for the optimizer to compare. The node under the Gather returns finished groups that no other participant returns, so no combine
 * step is planned above it. What keeps a query serial is the settings of the session and what a worker cannot run: sift.parallel_scan, max_parallel_workers_per_gather, a relation that is not
 * parallel safe, row security, a qual or target that is not parallel safe, and the exact float sum (sift.agg_exact_float), whose order of addition a parallel run changes.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "executor/executor.h"
#include "access/parallel.h"

#include "agg/cost.h"
#include "agg/plan/safety.h"
#include "agg/v6/v6_card.h"
#include "agg/v6/group_int.h"
#include "agg/v6/v6_par_pg.h"
#include "core/sift.h"

#define GRANULE_ROWS	8192.0

extern PGDLLIMPORT bool sift_agg_exact_float;

typedef struct CostPick
{
	SiftCostInputs in;
	double		selectivity;
	Cost		open;
	Cost		startup;
	Cost		total;
} CostPick;

static int
pick_workers(void *arg, int max_by_granules)
{
	CostPick   *c = arg;

	return sift_agg_choose_workers(&c->in, c->selectivity, false, false, c->open, max_by_granules, &c->startup, &c->total);
}

/* The share of the rows each participant takes, as PostgreSQL divides the rows of a partial path: the leader's share shrinks with the workers it has to serve */
static double
participants(int nworkers)
{
	const double leader = 1.0 - 0.3 * nworkers;

	return nworkers + (parallel_leader_participation && leader > 0 ? leader : 0.0);
}

static bool
unsafe_for_workers(PlannerInfo *root, const SiftV6Spec *spec, RelOptInfo *output_rel, List *having)
{
	return !is_parallel_safe(root, (Node *) spec->indexqual) || !is_parallel_safe(root, (Node *) spec->filter) ||
		!is_parallel_safe(root, (Node *) output_rel->reltarget->exprs) || !is_parallel_safe(root, (Node *) having);
}

/* The shapes whose distinct counts cannot be disjoint by group: a count(DISTINCT) beside a set of no keys that also has keys, or more of them than the block has slots */
static bool
distinct_not_partitionable(const SiftV6Spec *spec, uint32 ndist)
{
	bool		plain_set = spec->nkeys == 0 && spec->nsets == 0;

	for (int i = 0; i < spec->nsets; i++)
		plain_set |= spec->set_mask[i] == 0;
	return ndist > SIFT_PAR_MAX_DIST || (ndist > 0 && plain_set && (spec->nkeys > 0 || spec->nsets > 0));
}

static SiftV6ParInput
par_input(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, RelOptInfo *output_rel, const SiftV6Spec *spec, List *having, double groups, CostPick *pick)
{
	SiftV6ParInput in = {.parallel_scan = sift_parallel_scan,.parallel_ok = rel->consider_parallel && root->glob->parallelModeOK,.max_workers = max_parallel_workers_per_gather,
		.exact_float = sift_agg_exact_float,.nkeys = spec->nkeys,.nsets = spec->nsets,.naggs = spec->naggs,.est_rows = rel->rows,.g_eff = rel->rows / GRANULE_ROWS + 1.0,
		.est_groups = groups,.hash_limit = (double) get_hash_memory_limit(),.cost_workers = pick_workers,.cost_arg = pick};
	uint32		ndist = 0;
	bool		ext = false;
	SiftV6AggPrice price;

	for (int j = 0; j < spec->naggs; j++)
	{
		const SiftV6Agg *a = &spec->agg[j];

		ndist += a->kind == SV6_COUNT_DISTINCT;
		ext |= v6g_is_ext(a);
		in.float_sum_avg |= (a->kind == SV6_SUM || a->kind == SV6_AVG) && sift_v6_class(a->arg.typid) == SV6C_F64;
	}
	in.has_distinct = ndist > 0;
	in.has_rls = agg_query_has_rls(root);
	in.parallel_hazard = ext || OidIsValid(spec->text_indexoid) || unsafe_for_workers(root, spec, output_rel, having) || distinct_not_partitionable(spec, ndist);
	in.state_bytes = sizeof(SiftAggSlot64) + (double) spec->naggs * sizeof(V6Acc) + 16.0;
	price = sift_v6_agg_price(root, rel, ix, spec, groups);
	pick->in = price.in;
	pick->in.f_cold = 0.0;
	pick->in.n_ship = ndist > 0 ? pick->in.g_eff * GRANULE_ROWS * price.selectivity : 0.0;
	pick->selectivity = price.selectivity;
	pick->open = price.open;
	return in;
}

void
sift_v6_par_add_path(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, RelOptInfo *output_rel, const SiftV6Spec *serial_spec, const CustomPathMethods *methods, List *scan_tlist, List *having, double groups)
{
	CostPick	pick = {0};
	SiftV6ParInput in = par_input(root, rel, ix, output_rel, serial_spec, having, groups, &pick);
	SiftV6Spec	spec = *serial_spec;
	int			nworkers = 0;
	const SiftV6ParStrategy strategy = sift_v6_par_choose_strategy(&in, &nworkers);
	CustomPath *cp;
	double		total_groups = clamp_row_est(groups);

	if (strategy == SV6P_SERIAL)
		return;
	spec.par_strategy = strategy;
	spec.par_workers = nworkers;
	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = output_rel->reltarget;
	cp->path.parallel_aware = true;
	cp->path.parallel_safe = true;
	cp->path.parallel_workers = nworkers;
	cp->path.startup_cost = pick.startup;
	cp->path.total_cost = pick.total;
	cp->path.rows = clamp_row_est(total_groups / participants(nworkers));
	cp->methods = methods;
	cp->custom_private = list_make3(sift_v6_spec_pack(&spec), scan_tlist, having);
	add_path(output_rel, (Path *) create_gather_path(root, output_rel, &cp->path, output_rel->reltarget, NULL, &total_groups));
}
