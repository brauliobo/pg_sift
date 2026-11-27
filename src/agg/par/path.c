/*
 * agg/par/path.c - the parallel path of SiftAggV6: the planner's numbers for a query go to sift_v6_par_choose_strategy (v6_par_plan.c) and, when it picks a parallel strategy, a Gather over a
 * parallel-aware SiftAggV6 is added next to the serial path, for the optimizer to compare. The node under the Gather returns finished groups that no other participant returns, so no combine
 * step is planned above it. What keeps a query serial is the settings of the session and what a worker cannot run: sift.parallel_scan, max_parallel_workers_per_gather, a relation that is not
 * parallel safe, row security, a qual or target that is not parallel safe, an output the scan computes in another form (a CASE as a FILTER), and the exact float sum (sift.agg_exact_float), whose order of addition a parallel run changes.
 */
#include "postgres.h"

#include <math.h>

#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "executor/executor.h"
#include "access/parallel.h"
#include "catalog/pg_statistic.h"
#include "utils/syscache.h"

#include "agg/cost/cost.h"
#include "agg/cost/model.h"
#include "agg/safety.h"
#include "agg/cost/card.h"
#include "agg/group/int.h"
#include "agg/par/pinrun.h"
#include "agg/par/run.h"
#include "agg/par/pg.h"
#include "agg/test_gucs.h"
#include "agg/plan/rewrite.h"
#include "agg/settings.h"
#include "core/par/pool.h"
#include "core/sift.h"

#define GRANULE_ROWS	8192.0
#define SIFT_DICT_MAX_VALUES	65536.0

extern PGDLLIMPORT bool sift_agg_exact_float;

/*
 * What a parallel plan saves, in milliseconds as the executor measures them on the 1M-row ClickBench table (the price of a serial path in the units of the planner does not say: it spans 4 to 860
 * microseconds a unit across its queries). The scan of the serial path is shared by the participants; a parallel run adds its launch (the shared block, the workers, the Gather), and the records
 * the participants hand to the partitions, which is the price of a group that a participant saw but does not own. Only the plan that takes this much less time than the serial one is added, and the
 * Gather is priced by the ratio of the two times applied to the serial price: the setup cost of PostgreSQL's Gather is a fixed 1000 units, more than the whole serial price of a query that takes 100 ms.
 */
#define PAR_TUPLE_NS		15.0	/* the rows the scan reads */
#define PAR_TEXT_QUAL_NS	30.0	/* a text column a qual reads, for every row of the table */
#define PAR_INT_KEY_NS		10.0
#define PAR_TEXT_KEY_NS		40.0	/* a text key of many values: its strings are interned per part and hashed */
#define PAR_DICT_QUAL_NS	5.0		/* a qual on a column the query groups by, which the parts hold by dictionary codes */
#define PAR_DICT_DISTINCT_NS	4.0		/* a COUNT(DISTINCT) of a text column held by dictionary codes: a bit of the code */
#define PAR_DICT_KEY_NS		30.0	/* a text key of at most SIFT_DICT_MAX values: the parts hold it by dictionary codes (b_dec is a code read) */
#define PAR_DERIVE_NS		40.0
#define PAR_AGG_NS			8.0
#define PAR_VECTOR_AGG_NS	2.0		/* an aggregate of a query with no keys is a vector kernel over the batch */
#define PAR_DISTINCT_NS		12.0	/* a COUNT(DISTINCT) of no keys: the flat set of the values */
#define PAR_GROUP_DISTINCT_NS	8.0		/* of a GROUP BY: the set of its (group, value) pairs, in cache-sized radix buckets */
#define PAR_TEXT_AGG_NS		80.0
#define PAR_COLD_GROUPS		400000.0	/* from this many groups the table of a serial run no longer fits the caches ... */
#define PAR_COLD_NS			25.0		/* ... and each row waits for memory: a cache line of the compact table (a participant of a parallel run keeps a table of SIFT_PAR_LOCAL_GROUPS) */
#define PAR_COLD_TEXT_NS	150.0		/* ... of a table with a text key, whose strings are compared as well */
#define PAR_RECORD_NS		250.0	/* a record: made, routed, merged */
#define PAR_COLD_RECORD_NS	400.0	/* ... of a run of PAR_COLD_GROUPS groups or more, whose records wait for memory (Q18 and Q33 spend 400 to 450 ns on each, with no temporary file) */
#define PAR_VALUE_NS		60.0	/* a distinct value: its record routed and added to the set of its group at the owner */
#define PAR_LAUNCH_MS		17.0	/* the workers join 5 to 6 ms after the leader starts, and the Gather waits for them to leave: measured on the 1M-row table */
#define PAR_PER_WORKER_MS	1.0
#define PAR_MIN_GAIN		0.78	/* a parallel plan is added when it takes at most this share of the serial time: the model is not finer than that */

typedef struct CostPick
{
	Cost		serial_total;
	double		serial_ms;		/* of the serial plan */
	double		scan_ms;		/* of its scan without the waits for memory of a large table: what the participants share */
	double		rows;
	double		groups;
	uint32		topk;
	bool		values;			/* a count(DISTINCT): every row sends its value */
	int			nkeys;
	Cost		startup;
	Cost		total;
} CostPick;

/* what a row costs for the column c, which is read as text or as a number; a computed one (length, extract) is made from its source */
static double
col_ns(const SiftV6Spec *spec, const SiftV6Col *c, double text_ns, double number_ns)
{
	const Oid	type = c->derive > 0 ? spec->derive[c->derive - 1].src.typid : c->typid;

	return (c->derive > 0 ? PAR_DERIVE_NS : 0.0) + (c->attno > 0 && sift_v6_class(type) == SV6C_TEXT ? text_ns : number_ns);
}

static bool
is_key_column(const SiftV6Spec *spec, int attno)
{
	for (int i = 0; i < spec->nkeys; i++)
		if (spec->key[i].attno == attno && spec->key[i].derive == 0)
			return true;
	return false;
}

/* what a row is tested by before the aggregate sees it: the quals of the scan keys and the ones that run over the batch */
static List *
quals_of(const SiftV6Spec *spec)
{
	return list_concat_copy(spec->indexqual, spec->vqual);
}

/* the ns of a row for the text columns the quals of the scan read: their strings are decoded for every row, whether or not it passes; a column the query groups by (of few groups) is read as codes */
static double
text_qual_ns(const SiftV6Spec *spec, double groups)
{
	Bitmapset  *cols = NULL;
	double		ns = 0;

	foreach_ptr(Var, v, pull_var_clause((Node *) quals_of(spec), 0))
		if (sift_v6_class(v->vartype) == SV6C_TEXT && !bms_is_member(v->varattno, cols))
		{
			cols = bms_add_member(cols, v->varattno);
			ns += groups <= SIFT_DICT_MAX_VALUES && is_key_column(spec, v->varattno) ? PAR_DICT_QUAL_NS : PAR_TEXT_QUAL_NS;
		}
	return ns;
}

/* the index of the spec among those of the relation; NULL for a table, or the partitions of one */
static const IndexOptInfo *
spec_index(const RelOptInfo *rel, const SiftV6Spec *spec)
{
	if (!spec->is_table && spec->nparts == 0)
		foreach_ptr(IndexOptInfo, ix, rel->indexlist)
			if (ix->indexoid == spec->relid)
				return ix;
	return NULL;
}

/* the rows of the granules the scan reads: those the zone maps of an index leave to the quals (agg/cost/card.h), the share the quals leave of a table or of partitions, all of them for a
 * qual on a text column that is not a key (a LIKE keeps every granule of the zone maps and decodes it) */
static double
scan_rows(PlannerInfo *root, RelOptInfo *rel, const SiftV6Spec *spec, double groups)
{
	const IndexOptInfo *ix = spec_index(rel, spec);

	foreach_ptr(Var, v, pull_var_clause((Node *) quals_of(spec), 0))
		if (sift_v6_class(v->vartype) == SV6C_TEXT && !is_key_column(spec, v->varattno))
			return rel->tuples;
	if (ix == NULL)
		return Max(rel->rows, rel->tuples * sift_v6_clause_prune(root, rel->baserestrictinfo, 0));
	return Max(rel->rows, sift_v6_agg_price(root, rel, ix, spec, groups).in.g_eff * GRANULE_ROWS);
}

/* The distinct values of a key column in the statistics of the table: 0 when it has none (the attribute of an index is the one its key names) */
static double
key_distinct(const PlannerInfo *root, const RelOptInfo *rel, const SiftV6Spec *spec, const SiftV6Col *c)
{
	const IndexOptInfo *ix = spec_index(rel, spec);
	AttrNumber	attno = spec->is_table ? c->attno : 0;
	HeapTuple	tup;
	double		nd;

	if (ix != NULL && c->attno <= ix->ncolumns)
		attno = ix->indexkeys[c->attno - 1];
	if (attno <= 0)
		return 0.0;
	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(root->simple_rte_array[rel->relid]->relid), Int16GetDatum(attno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tup))
		return 0.0;
	nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
	ReleaseSysCache(tup);
	return nd > 0 ? nd : -nd * rel->tuples;
}

/* a text key of more values than a dictionary holds is compared as a string in the table; one of fewer is a code */
static double
cold_ns(const PlannerInfo *root, const RelOptInfo *rel, const SiftV6Spec *spec)
{
	for (int i = 0; i < spec->nkeys; i++)
	{
		const SiftV6Col *k = &spec->key[i];
		double		nd;

		if (col_ns(spec, k, 1.0, 0.0) - (k->derive > 0 ? PAR_DERIVE_NS : 0.0) > 0.0 && !(k->derive == 0 && (nd = key_distinct(root, rel, spec, k)) > 0.0 && nd <= SIFT_DICT_MAX_VALUES))
			return PAR_COLD_TEXT_NS;
	}
	return PAR_COLD_NS;
}

static double
scan_ms(const SiftV6Spec *spec, double tuples, double rows, double groups)
{
	const double text_key_ns = groups <= SIFT_DICT_MAX_VALUES ? PAR_DICT_KEY_NS : PAR_TEXT_KEY_NS;
	double		ns = 0;

	for (int i = 0; i < spec->nkeys; i++)
		ns += col_ns(spec, &spec->key[i], text_key_ns, PAR_INT_KEY_NS);
	for (int j = 0; j < spec->naggs; j++)
	{
		const bool	distinct = sift_v6_kind_distinct(spec->agg[j].kind);

		ns += col_ns(spec, &spec->agg[j].arg, distinct ? PAR_DICT_DISTINCT_NS : PAR_TEXT_AGG_NS, spec->nkeys == 0 ? PAR_VECTOR_AGG_NS : PAR_AGG_NS) +
			(distinct ? (spec->nkeys == 0 ? PAR_DISTINCT_NS : PAR_GROUP_DISTINCT_NS) : 0.0);
	}
	return (tuples * (PAR_TUPLE_NS + text_qual_ns(spec, groups)) + rows * ns) / 1e6;
}

static double
gather_rows(const CostPick *c, int nworkers)
{
	return c->topk > 0 ? Min(c->groups, (double) c->topk * (nworkers + 1)) : c->groups;
}

/* the launch and the hand-over of the records follow parallel_setup_cost and parallel_tuple_cost, which are 1000 and 0.1 where the milliseconds above were measured */
static double
parallel_ms(const CostPick *c, int nworkers)
{
	const double p = nworkers + 1;
	const double groups = Max(c->groups, 1.0);
	const double records = c->values ? c->rows * (c->nkeys == 0 ? 0.3 : 1.0) : p * groups * (1.0 - exp(-c->rows / p / groups));
	const double launch = (PAR_LAUNCH_MS + PAR_PER_WORKER_MS * nworkers) * parallel_setup_cost / DEFAULT_PARALLEL_SETUP_COST;
	const double record_ns = c->values ? PAR_VALUE_NS : (c->groups >= PAR_COLD_GROUPS ? PAR_COLD_RECORD_NS : PAR_RECORD_NS);
	const double hand_over = records * record_ns / 1e6 / p * parallel_tuple_cost / DEFAULT_PARALLEL_TUPLE_COST;

	return launch + c->scan_ms / (1.0 + SIFT_PAR_EFFICIENCY * nworkers) + hand_over;
}

static int
pick_workers(void *arg, int max_by_granules)
{
	CostPick   *c = arg;
	const double par = parallel_ms(c, max_by_granules);

	if (par >= c->serial_ms * PAR_MIN_GAIN)
		return 0;
	c->startup = c->total = c->serial_total * par / Max(c->serial_ms, 1e-6);
	return max_by_granules;
}

/* The threads split the scan as the participants of a Gather do, with no launch; the waits for memory of a large table stay the backend's */
void
sift_v6_threads_price(PlannerInfo *root, RelOptInfo *rel, const SiftV6Spec *spec, double groups, Path *path)
{
	const double scan = scan_ms(spec, rel->tuples, rel->rows, groups);
	const double serial = scan + (groups >= PAR_COLD_GROUPS ? rel->rows * cold_ns(root, rel, spec) / 1e6 : 0.0);
	double		share;

	if (!sift_v6_par_wanted(spec) && !sift_v6_pin_wanted(spec))
		return;
	share = (scan / (1.0 + SIFT_PAR_EFFICIENCY * (sift_par_pool_threads() - 1)) + serial - scan) / Max(serial, 1e-6);
	path->startup_cost *= share;
	path->total_cost *= share;
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
par_input(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *output_rel, const SiftV6Spec *spec, List *having, double groups, CostPick *pick)
{
	SiftV6ParInput in = {.parallel_scan = sift_parallel_scan,.parallel_ok = rel->consider_parallel && root->glob->parallelModeOK,.max_workers = max_parallel_workers_per_gather,
		.exact_float = sift_agg_exact_float,.force = sift_agg_par_min_rows == 0,.nkeys = spec->nkeys,.nsets = spec->nsets,.naggs = spec->naggs,.est_rows = rel->rows,.g_eff = scan_rows(root, rel, spec, groups) / GRANULE_ROWS + 1.0,
		.est_groups = groups,.hash_limit = (double) get_hash_memory_limit(),.cost_workers = pick_workers,.cost_arg = pick};
	uint32		ndist = 0;
	bool		slot_text = false;
	bool		valued = false;

	for (int j = 0; j < spec->naggs; j++)
	{
		const SiftV6Agg *a = &spec->agg[j];

		ndist += sift_v6_kind_distinct(a->kind);
		valued |= sift_v6_kind_valued(a->kind);
		slot_text |= v6g_is_ext(a) && sift_v6_class(a->arg.typid) == SV6C_TEXT;
		in.float_sum_avg |= (a->kind == SV6_SUM || a->kind == SV6_AVG) && sift_v6_class(a->arg.typid) == SV6C_F64;
	}
	in.has_distinct = ndist > 0;
	in.has_rls = agg_query_has_rls(root);
	in.parallel_hazard = (slot_text && ndist > 0 && spec->nkeys == 0) || (valued && spec->nkeys == 0) || OidIsValid(spec->text_indexoid) || unsafe_for_workers(root, spec, output_rel, having) || distinct_not_partitionable(spec, ndist);
	in.state_bytes = sizeof(SiftAggSlot64) + (double) spec->naggs * sizeof(V6Acc) + 16.0;
	return in;
}

/* ORDER BY .. LIMIT K of the groups, when each participant can keep its best K: HAVING would drop groups after the heap (the node does not run it), and grouping sets have no one order */
static void
set_topk(PlannerInfo *root, SiftV6Spec *spec, const List *scan_tlist, const List *having)
{
	const uint32 k = sift_v6_topk_of(root->parse);

	if (k == 0 || having != NIL || spec->nsets > 0 || spec->ntopk > 0)
		return;
	spec->topk = k;
	if (!sift_v6_topk_items(root->parse, scan_tlist, spec))
		spec->topk = spec->ntopk = 0;
}

void
sift_v6_par_add_path(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *output_rel, const SiftV6Spec *serial_spec, const CustomPathMethods *methods, List *scan_tlist, List *having, double groups,
					 Cost serial_total)
{
	SiftV6Spec	spec = *serial_spec;
	CostPick	pick = {.serial_total = serial_total,.groups = clamp_row_est(groups)};
	SiftV6ParInput in;
	int			nworkers = 0;
	SiftV6ParStrategy strategy;
	CustomPath *cp;
	GatherPath *gather;
	double		total_groups;

	set_topk(root, &spec, scan_tlist, having);
	if (parallel_setup_cost > 0 && debug_parallel_query == DEBUG_PARALLEL_OFF && (sift_v6_par_wanted(&spec) || sift_v6_pin_wanted(&spec)))
		return;
	pick.topk = spec.topk;
	pick.rows = rel->rows;
	pick.scan_ms = scan_ms(&spec, rel->tuples, rel->rows, groups);
	pick.serial_ms = pick.scan_ms + (groups >= PAR_COLD_GROUPS ? rel->rows * cold_ns(root, rel, &spec) / 1e6 : 0.0);
	pick.values = false;
	pick.nkeys = spec.nkeys;
	for (int j = 0; j < spec.naggs; j++)
		pick.values |= sift_v6_kind_distinct(spec.agg[j].kind);
	in = par_input(root, rel, output_rel, &spec, having, groups, &pick);
	strategy = sift_v6_par_choose_strategy(&in, &nworkers);
	if (strategy == SV6P_SERIAL)
		return;
	total_groups = gather_rows(&pick, nworkers) * (having == NIL ? 1.0 : sift_v6_quals_sel(root, having, 0));
	spec.par_strategy = strategy;
	spec.par_workers = nworkers;
	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = output_rel->reltarget;
	cp->path.parallel_aware = true;
	cp->path.parallel_safe = true;
	cp->path.parallel_workers = nworkers;
	cp->path.startup_cost = cp->path.total_cost = pick.total;
	cp->path.rows = clamp_row_est(total_groups / participants(nworkers));
	cp->methods = methods;
	cp->custom_private = list_make3(sift_v6_spec_pack(&spec), scan_tlist, having);
	gather = create_gather_path(root, output_rel, &cp->path, output_rel->reltarget, NULL, &total_groups);
	gather->path.startup_cost = gather->path.total_cost = pick.total;
	add_path(output_rel, (Path *) gather);
}
