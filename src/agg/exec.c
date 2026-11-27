/*
 * agg/exec.c - the executor node of SiftAggV6: the scan of the index folds every row into the groups when the node is first asked for a tuple, then one tuple leaves for each group.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/parallel.h"
#include "access/table.h"
#include "common/hashfn.h"
#include "datatype/timestamp.h"
#include "commands/explain.h"
#include "executor/nodeIndexscan.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "agg/engine.h"
#include "agg/vexpr.h"
#include "agg/ext.h"
#include "agg/explain.h"
#include "agg/fuse/fuse.h"
#include "agg/fuse/keys.h"
#include "agg/gpu.h"
#include "agg/open_scan.h"
#include "agg/par/node.h"
#include "agg/star/run.h"
#include "agg/qual/qual.h"
#include "agg/scan_src.h"
#include "agg/stats.h"
#include "agg/topn/rows.h"
#include "agg/topn/topk_run.h"
#include "core/maint/pacer.h"
#include "access/tableam.h"
#include "col/am.h"
#include "core/store/ops.h"
#include "core/cpu.h"
#include "core/errctx.h"
#include "core/memtrack.h"

typedef struct V6State
{
	CustomScanState css;
	SiftV6Spec *spec;
	int			nsources;
	SiftV6ScanSrc *sources;		/* the index of the plan, or the index of each live partition of a partitioned table */
	MemoryContext gcx;
	SiftV6Group *group;
	SiftV6ScanStats stats;
	SiftProjResult proj;
	const SiftProjOps *proj_ops;	/* the format that served the statement from a projection */
	SiftV6Gpu	gpu;
	SiftV6StarRun *star;		/* the dimensions of a star join */
	uint32		star_passes;
	uint32		star_shared;
	SiftV6TopKRun *topk;
	SiftV6Rows *rows;
	Datum	   *totals;			/* a row Top-N with aggregates of the whole set: their values, which every row it gives carries */
	bool	   *totals_null;
	bool		ran;
	bool		explain_only;
	SiftV6FuseArm arm;			/* in the registry of the statement: arms of one index and one set of keys scan once */
	SiftV6ParNode *par;			/* a parallel node: the participants' scan of the units of the index (v6_par_node.c) */
	int64		dict_shared0[2];	/* the backend's counts of shared dictionaries hit and missed when the node began */
} V6State;

/* Arms of equal keys read the same rows: the keys of the scan are constants, so their text tells them apart */
static uint64
scan_signature(const SiftV6Spec *spec)
{
	/* MEM[MEM-AGG-06]: scan_signature nodeToString buffer (the text of the index quals, freed after it is hashed; was left in the query context for every plan execution) */
	char	   *text = nodeToString(spec->indexqual);
	uint64		sig = DatumGetUInt64(hash_any_extended((const unsigned char *) text, strlen(text), 0));

	pfree(text);
	return sig;
}

static void
begin_index(V6State *st, CustomScanState *node, SiftV6ScanSrc *src, Oid indexoid)
{
	src->ops = sift_store_ops_for_oid(indexoid, false);
	src->index = index_open(indexoid, AccessShareLock);
	src->heap = table_open(src->index->rd_index->indrelid, AccessShareLock);
	sift_v6_qual_scankeys(&node->ss.ps, st->spec->jsonqual, src->index, st->spec->indexqual, &src->keys, &src->nkeys, &src->rtkeys, &src->nrt);
}

static void
begin_table(V6State *st, EState *estate, SiftV6ScanSrc *src, Oid tableoid)
{
	src->heap = table_open(tableoid, AccessShareLock);
	src->ops = sift_store_ops_for_oid(tableoid, true);
	if (src->ops->zone != NULL)
		src->quals = src->ops->zone->compile_quals(src->heap, st->spec->indexqual, estate->es_query_cxt);
}

static void
begin_star(V6State *st, CustomScanState *node, EState *estate, int eflags)
{
	PlanState  *children[SIFT_STAR_MAX_DIMS];
	int			d = 0;

	foreach_ptr(Plan, plan, ((CustomScan *) node->ss.ps.plan)->custom_plans)
	{
		SIFT_INVARIANT(d < SIFT_STAR_MAX_DIMS, "a star join has more dimensions than the planner allows");
		children[d++] = ExecInitNode(plan, estate, eflags);
		node->custom_ps = lappend(node->custom_ps, children[d - 1]);
	}
	st->star = sift_v6_star_begin(st->spec->ext, children, estate->es_query_cxt);
}

/* After the quarantine net of sift_v6_source_enter, which says why an index is out: an index with no format left service some other way (REINDEX CONCURRENTLY marks the replaced one invalid) */
static void
require_formats(const V6State *st)
{
	for (int k = 0; k < st->nsources; k++)
		if (st->sources[k].ops == NULL)
			ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							errmsg("sift index \"%s\" left service after the statement was planned; the statement must be planned again", RelationGetRelationName(st->sources[k].index != NULL ? st->sources[k].index : st->sources[k].heap)),
							errhint("Run the statement again: it is planned without the index.")));
}

static bool
v6_fusable(const V6State *st)
{
	return st->sources[0].ops->zone != NULL && st->spec->nparts == 0 && !st->spec->is_table && st->spec->vqual == NIL && (st->spec->ext == NULL || (st->star != NULL && !st->spec->ext->unnest)) &&
		st->spec->par_strategy == 0 && !OidIsValid(st->spec->text_indexoid) && !st->spec->rows && st->spec->topk == 0;
}

static void
begin_arm(V6State *st, CustomScanState *node, EState *estate, int eflags)
{
	const SiftV6ScanSrc *src = &st->sources[0];

	if (!v6_fusable(st))
		return;
	st->explain_only = (eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0;
	st->arm = (SiftV6FuseArm) {.owner = st,.indexoid = st->spec->relid,.snap = estate->es_snapshot,.scan_sig = scan_signature(st->spec),
	.parallel = node->ss.ps.plan->parallel_aware || IsParallelWorker(),.param_exec = node->ss.ps.plan->extParam != NULL};
	sift_v6_fuse_describe(&st->arm, src->index, st->spec);
	sift_v6_fuse_arm_add(estate, &st->arm);
}

/* The quals of a parallel scan are constants, parameters or stable expressions of the statement: the participants read the values the leader read when the scan began */
static void
begin_par(V6State *st, CustomScanState *node, EState *estate)
{
	for (int k = 0; k < st->nsources; k++)
	{
		if (st->sources[k].nrt > 0)
			ExecIndexEvalRuntimeKeys(node->ss.ps.ps_ExprContext, st->sources[k].rtkeys, st->sources[k].nrt);
		if (st->sources[k].index == NULL)
			st->sources[k].ops->zone->bind_quals(st->sources[k].heap, st->sources[k].quals, node->ss.ps.ps_ExprContext);
	}
	st->par = sift_v6_par_node_begin(st->spec, node->ss.ss_ScanTupleSlot->tts_tupleDescriptor, st->sources, st->nsources, estate->es_snapshot, estate->es_query_cxt,
									 node->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
}

static void
v6_begin(CustomScanState *node, EState *estate, int eflags)
{
	V6State    *st = (V6State *) node;

	st->spec = sift_v6_spec_unpack(((CustomScan *) node->ss.ps.plan)->custom_private);
	sift_v6_bind_params(st->spec, estate->es_param_list_info);
	sift_v6_exec_begin(st->spec, &node->ss.ps);
	st->dict_shared0[0] = sift_stat_v[SIFT_STAT_DICT_SHARED_HITS];
	st->dict_shared0[1] = sift_stat_v[SIFT_STAT_DICT_SHARED_MISSES];
	st->nsources = Max(st->spec->nparts, 1);
	st->sources = palloc0(sizeof(SiftV6ScanSrc) * st->nsources);
	for (int k = 0; k < st->nsources; k++)
		if (sift_v6_spec_part_is_table(st->spec, k))
			begin_table(st, estate, &st->sources[k], sift_v6_spec_part_oid(st->spec, k));
		else
			begin_index(st, node, &st->sources[k], sift_v6_spec_part_oid(st->spec, k));
	if (st->spec->ext != NULL && st->spec->ext->ndims > 0)
		begin_star(st, node, estate, eflags);
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		for (int k = 0; k < st->nsources; k++)
			sift_v6_source_enter(st->sources[k].index != NULL ? st->sources[k].index : st->sources[k].heap, st->sources[k].index == NULL, estate->es_snapshot);
	require_formats(st);
	begin_arm(st, node, estate, eflags);
	if (st->spec->par_strategy != 0)
		begin_par(st, node, estate);
}

static bool
v6_serve_projection(V6State *st, Snapshot snap)
{
	SiftV6ScanSrc *src = &st->sources[0];

	if (st->spec->nparts > 0 || st->spec->ext != NULL || OidIsValid(st->spec->text_indexoid))
		return false;
	ScanKey		keys = src->keys;
	int			nkeys = src->nkeys;

	if (src->ops->proj == NULL || (src->index == NULL && !src->ops->zone->quals_keys(src->quals, &keys, &nkeys)))
		return false;
	st->proj_ops = src->ops->proj;
	return st->proj_ops->serve(st->spec, src->heap, src->index, keys, nkeys, snap, st->gcx, &st->proj);
}

/* The hits of the text query of the plan, NULL without one: opened for one scan of the index and closed with it (each pass of a star join reads them again) */
static SiftV6Text *
text_begin(const V6State *st)
{
	return OidIsValid(st->spec->text_indexoid) ? sift_v6_text_open(st->spec, CurrentMemoryContext) : NULL;
}

static void
text_end(SiftV6Text *text)
{
	if (text != NULL)
		sift_v6_text_close(text);
}

static void
scan_with_text(V6State *st, const SiftV6ScanSrc *src, Snapshot snap, SiftV6StarRun *star)
{
	SiftV6Text *text = text_begin(st);

	src->ops->zone->scan_run(st->spec, st->group, src->heap, src->index, src->keys, src->nkeys, snap, st->gpu.skip, st->gpu.nskip, NULL, &st->stats, star,
						 text ? sift_v6_text_chunk : NULL, text);
	text_end(text);
}

static void
scan_table_with_text(V6State *st, const SiftV6ScanSrc *src, Snapshot snap, SiftV6StarRun *star)
{
	SiftV6Text *text = text_begin(st);

	src->ops->zone->scan_table(st->spec, st->group, src->heap, src->quals, snap, st->gpu.skip, st->gpu.nskip, &st->stats, star,
							   text ? sift_v6_text_chunk : NULL, text);
	text_end(text);
}

/* The unnest of an array column: the elements the index keeps, or those of the heap rows of the table the scan reads */
static void
v6_scan_unnest(V6State *st, const SiftV6ScanSrc *src, Snapshot snap)
{
	if (src->index == NULL)
		src->ops->zone->scan_table(st->spec, st->group, src->heap, src->quals, snap, NULL, 0, &st->stats, st->star, NULL, NULL);
	else
		src->ops->jsonb->unnest_run(st->spec, st->group, src->heap, src->index, snap, &st->stats, st->star);
}

/* One scan of a fact table of a join or an unnest: a source of the node, the partitions of a partitioned fact being one each */
static void
v6_scan_fact(V6State *st, const SiftV6ScanSrc *src, Snapshot snap)
{
	if (st->spec->ext->unnest)
		v6_scan_unnest(st, src, snap);
	else if (src->index == NULL)
		scan_table_with_text(st, src, snap, st->star);
	else
	{
		SiftV6ScanSrc pass = *src;

		pass.keys = sift_v6_star_scan_keys(st->star, src->index, src->keys, src->nkeys, &pass.nkeys);
		scan_with_text(st, &pass, snap, st->star);
	}
}

/* The scans of the fact table of a join or an unnest: once for each pass of the dimensions of a star join (one when they all fit work_mem), each pass reading every source */
static void
v6_scan_ext(V6State *st, Snapshot snap)
{
	SiftStarOpen open;
	uint32		pass = 0;

	if (st->spec->ext->unnest && st->star == NULL)
	{
		v6_scan_unnest(st, &st->sources[0], snap);
		return;
	}
	while ((open = sift_v6_star_open(st->star, pass++, st->nsources)) != SSO_DONE)
	{
		MemoryContext pass_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 star pass");
		MemoryContext old = MemoryContextSwitchTo(pass_cx);

		for (int k = 0; open == SSO_SCAN && k < st->nsources; k++)
			v6_scan_fact(st, &st->sources[k], snap);
		MemoryContextSwitchTo(old);
		MemoryContextDelete(pass_cx);
	}
	st->star_passes = sift_v6_star_passes(st->star);
	st->star_shared = sift_v6_star_shared(st->star);
	sift_v6_star_close(st->star);
}

static void
v6_scan(V6State *st, Snapshot snap)
{
	TupleDesc	desc = st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;

	if (st->spec->nparts == 0 && st->spec->ext == NULL && !OidIsValid(st->spec->text_indexoid))
		st->group = sift_v6_gpu_agg(st->spec, st->sources[0].heap, st->sources[0].index, snap, st->css.ss.ps.plan->plan_rows, st->css.ss.ps.qual != NULL, st->gcx, desc, &st->gpu);
	if (st->group == NULL)
		st->group = sift_v6_group_new(st->spec, st->gcx, desc);
	if (st->spec->ext != NULL)
	{
		v6_scan_ext(st, snap);
		sift_v6_group_finish(st->group);
		return;
	}
	for (int k = 0; k < st->nsources; k++)
	{
		SiftV6ScanSrc *src = &st->sources[k];

		if (src->ops->zone == NULL)
			sift_v6_open_scan_run(st->spec, st->group, src->ops, st->css.ss.ps.state, src->heap, src->index, &st->stats);
		else if (src->index != NULL)
			scan_with_text(st, src, snap, NULL);
		else
			scan_table_with_text(st, src, snap, NULL);
	}
	sift_v6_group_finish(st->group);
}

/* The groups of an arm, to be filled by the pass that serves it */
static SiftV6ScanArm
arm_groups(V6State *st, EState *estate)
{
	if (st->gcx == NULL)
		st->gcx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 groups");
	st->group = sift_v6_group_new(st->spec, st->gcx, st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	st->ran = true;
	return (SiftV6ScanArm) {.spec = st->spec,.group = st->group,.stats = &st->stats};
}

/* The keys of the pass: those of the leader when every arm has them, else the rest of the quals with the lead key in the union of the values of the arms (each arm takes its own by its gate) */
static ScanKey
pass_keys(V6State *st, const SiftV6FuseCohort *c, List *quals, int *nkeys)
{
	const SiftV6ScanSrc *src = &st->sources[0];
	IndexRuntimeKeyInfo *rtkeys = NULL;
	ScanKey		keys;
	int			nrt = 0;

	if (c->same_keys)
		return (*nkeys = src->nkeys, src->keys);
	sift_v6_qual_scankeys(&st->css.ss.ps, st->spec->jsonqual, src->index, quals, &keys, nkeys, &rtkeys, &nrt);
	SIFT_INVARIANT(nrt == 0, "the quals of the aggregate scan are constants");
	return keys;
}

/* The dimensions of a star join of an arm, read for the pass: false when they need more than one pass, join no fact row, or the arms take their rows by gates (the arm then scans alone) */
static bool
star_fits_pass(V6State *owner, const SiftV6FuseCohort *c)
{
	if (owner->star == NULL)
		return true;
	if (!c->same_keys)
		return false;
	if (sift_v6_star_open(owner->star, 0, 1) == SSO_SCAN && sift_v6_star_one_pass(owner->star))
		return true;
	sift_v6_star_close(owner->star);
	return false;
}

/* An arm of the cohort whose star join cannot share the pass: it leaves the cohort and scans alone, its groups ready when its node is asked */
static void
run_alone(V6State *owner, EState *estate)
{
	sift_v6_fuse_detach(&owner->arm);
	if (owner->gcx == NULL)
		owner->gcx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 groups");
	owner->ran = true;
	v6_scan(owner, estate->es_snapshot);
}

/* One pass for the arms of the cohort that this node leads: the groups of each are done when its node is asked */
static void
v6_run_fused(V6State *st, SiftV6FuseCohort *c, EState *estate)
{
	const SiftV6ScanSrc *src = &st->sources[0];
	V6State    *alone[SIFT_V6_FUSE_MAX_ARMS];
	SiftV6ScanArm *arms;
	List	   *common = NIL;
	List	   *quals;
	const int	narms = c->narms;
	int			nkeys,
				nalone = 0;
	ScanKey		keys;

	for (int i = 0; i < narms; i++)
		if (!star_fits_pass(c->arms[i]->owner, c))
			alone[nalone++] = c->arms[i]->owner;
	for (int i = 0; i < nalone; i++)
		run_alone(alone[i], estate);
	if (nalone == narms)
		return;
	quals = c->same_keys ? NIL : sift_v6_fuse_union_quals(c, src->index, &common);
	keys = pass_keys(st, c, quals, &nkeys);
	arms = palloc(c->narms * sizeof(SiftV6ScanArm));
	for (int i = 0; i < c->narms; i++)
	{
		V6State    *owner = c->arms[i]->owner;
		SiftV6Spec *pass_spec;
		struct SiftClause *gate;

		arms[i] = arm_groups(owner, estate);
		arms[i].star = owner->star;
		if (!c->same_keys && sift_v6_fuse_gate(estate->es_query_cxt, c->arms[i], common, owner->spec, &pass_spec, &gate))
		{
			arms[i].spec = pass_spec;
			arms[i].gate = gate;
		}
	}
	src->ops->zone->scan_arms(c, arms, src->heap, src->index, keys, nkeys, estate->es_snapshot);
	for (int i = 0; i < c->narms; i++)
	{
		V6State    *owner = c->arms[i]->owner;

		if (owner->star != NULL)
		{
			owner->star_passes = sift_v6_star_passes(owner->star);
			owner->star_shared = sift_v6_star_shared(owner->star);
			sift_v6_star_close(owner->star);
		}
		sift_v6_group_finish(arms[i].group);
	}
}

static bool
proj_stream_next(void *arg, Datum *values, bool *isnull)
{
	V6State    *st = arg;

	return st->proj_ops->next(&st->proj, values, isnull);
}

/* the rows of one source: once for each pass of the dimensions of a join (one when they all fit work_mem) */
static void
v6_scan_rows(V6State *st, const SiftV6ScanSrc *src, SiftV6Text *text, Snapshot snap)
{
	SiftStarOpen open;
	uint32		pass = 0;

	if (st->star == NULL)
	{
		src->ops->order->scan_rows(st->spec, st->rows, st->group, NULL, src->heap, src->index, src->keys, src->nkeys, src->quals, snap, &st->stats,
								   text ? sift_v6_text_chunk : NULL, text);
		return;
	}
	while ((open = sift_v6_star_open(st->star, pass++, 1)) != SSO_DONE)
	{
		MemoryContext pass_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 star pass");
		MemoryContext old = MemoryContextSwitchTo(pass_cx);

		if (open == SSO_SCAN)
			src->ops->order->scan_rows(st->spec, st->rows, st->group, st->star, src->heap, src->index, src->keys, src->nkeys, src->quals, snap, &st->stats, NULL, NULL);
		MemoryContextSwitchTo(old);
		MemoryContextDelete(pass_cx);
	}
	st->star_passes = sift_v6_star_passes(st->star);
	st->star_shared = sift_v6_star_shared(st->star);
	sift_v6_star_close(st->star);
}

/* the aggregates of the whole set: the one row of the group the scan folded every row it kept into */
static void
v6_totals(V6State *st)
{
	const int	n = st->spec->naggs;

	st->totals = MemoryContextAlloc(st->gcx, n * sizeof(Datum));
	st->totals_null = MemoryContextAlloc(st->gcx, n * sizeof(bool));
	sift_v6_group_finish(st->group);
	SIFT_INVARIANT(sift_v6_group_next(st->group, st->totals, st->totals_null), "the aggregates of the whole set of a row Top-N have no row");
}

static void
v6_run_rows(V6State *st, EState *estate)
{
	SiftV6Text *text = text_begin(st);

	st->rows = sift_v6_rows_new(st->gcx, st->spec);
	if (st->spec->naggs > 0)
		st->group = sift_v6_rows_totals_new(st->gcx, st->spec);
	for (int k = 0; k < st->nsources; k++)
	{
		SiftV6ScanSrc   *src = &st->sources[k];

		sift_v6_rows_source(st->rows, k);
		v6_scan_rows(st, src, text, estate->es_snapshot);
		src->fetch = sift_table_index_fetch_begin(src->heap);
		src->fetch_slot = table_slot_create(src->heap, NULL);
	}
	text_end(text);
	sift_v6_rows_finish(st->rows);
	if (st->group != NULL)
		v6_totals(st);
}

/* the next winner of a row Top-N: its columns from the heap row at its address, in the partition it came from, or from what the heap of winners and the scan kept of it */
static bool
v6_next_row(V6State *st, TupleTableSlot *slot)
{
	const SiftV6Spec *spec = st->spec;
	ItemPointerData tid;
	bool		again = false,
				dead = false;
	uint32		source;
	uint64		addr;
	int			i = 0;
	const SiftV6ScanSrc *src;

	if (!sift_v6_rows_next(st->rows, &source, &addr))
		return false;
	src = &st->sources[source];
	sift_f2_tid_set(&tid, addr);
	SIFT_INVARIANT(table_index_fetch_tuple(src->fetch, &tid, st->css.ss.ps.state->es_snapshot, src->fetch_slot, &again, &dead), "a row the scan kept is not in the heap");
	foreach_node(TargetEntry, te, ((CustomScan *) st->css.ss.ps.plan)->custom_scan_tlist)
	{
		const SiftV6RowOut *out = &spec->out[i];
		const AttrNumber attno = out->from != SV6OUT_HEAP ? 0 : spec->nparts > 0 ? spec->part_attno[source * spec->part_ncols + i] : ((Var *) te->expr)->varattno;

		if (out->from == SV6OUT_HEAP)
			slot->tts_values[i] = slot_getattr(src->fetch_slot, attno, &slot->tts_isnull[i]);
		else if (out->from == SV6OUT_KEY)
			slot->tts_values[i] = sift_v6_rows_key(st->rows, out->at, &slot->tts_isnull[i]);
		else
		{
			slot->tts_values[i] = st->totals[out->at];
			slot->tts_isnull[i] = st->totals_null[out->at];
		}
		i++;
	}
	return true;
}

static void
v6_run_scan(V6State *st, EState *estate)
{
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	SiftV6FuseCohort *c;

	for (int k = 0; k < st->nsources; k++)
	{
		if (st->sources[k].nrt > 0)
			ExecIndexEvalRuntimeKeys(st->css.ss.ps.ps_ExprContext, st->sources[k].rtkeys, st->sources[k].nrt);
		if (st->sources[k].index == NULL && st->sources[k].ops->zone != NULL)
			st->sources[k].ops->zone->bind_quals(st->sources[k].heap, st->sources[k].quals, st->css.ss.ps.ps_ExprContext);
	}
	st->gcx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 groups");
	st->ran = true;
	if (st->spec->rows)
	{
		v6_run_rows(st, estate);
		MemoryContextSwitchTo(old);
		return;
	}
	if (v6_serve_projection(st, estate->es_snapshot))
		sift_v6_fuse_detach(&st->arm);
	else if (v6_fusable(st) && (c = sift_v6_fuse_lead(estate, &st->arm, false, st->explain_only)) != NULL)
		v6_run_fused(st, c, estate);
	else
		v6_scan(st, estate->es_snapshot);
	if (st->spec->topk > 0)
	{
		PlanState  *ps = &st->css.ss.ps;
		TupleTableSlot *slot = st->css.ss.ss_ScanTupleSlot;

		st->topk = sift_v6_topk_run_new(st->gcx, st->spec, slot->tts_tupleDescriptor);
		if (st->proj.used)
			sift_v6_topk_run_stream(st->topk, proj_stream_next, st, ps->qual, ps->ps_ExprContext, slot);
		else
			sift_v6_topk_run_groups(st->topk, st->group, ps->qual, ps->ps_ExprContext, slot);
	}
	MemoryContextSwitchTo(old);
}

/* One run of the node in 16 feeds the foreground averages the maintenance pacer decides by (core/maint/pacer.h) */
static void
v6_run(V6State *st, EState *estate)
{
	TimestampTz t0;
	const bool	sampled = sift_pacer_fg_sample_begin(&t0);

	v6_run_scan(st, estate);
	if (sampled)
		sift_pacer_record_foreground(GetCurrentTimestamp() - t0);
}

/* A parallel node with its shared block runs the participants' scan; a Gather that made no block (no worker could start) leaves the node to the serial run */
static bool
v6_parallel(const V6State *st)
{
	return st->par != NULL && sift_v6_par_node_attached(st->par);
}

static bool
v6_group_next(V6State *st, TupleTableSlot *slot)
{
	if (v6_parallel(st))
		return sift_v6_par_node_next(st->par, slot->tts_values, slot->tts_isnull);
	if (st->proj.used)
		return st->proj_ops->next(&st->proj, slot->tts_values, slot->tts_isnull);
	return sift_v6_group_next(st->group, slot->tts_values, slot->tts_isnull);
}

static TupleTableSlot *
v6_next(ScanState *ss)
{
	V6State    *st = (V6State *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (!v6_parallel(st) && !st->ran)
		v6_run(st, ss->ps.state);
	ExecClearTuple(slot);
	MemoryContextSwitchTo(ss->ps.ps_ExprContext->ecxt_per_tuple_memory);
	if (st->rows != NULL ? !v6_next_row(st, slot) :
		st->topk != NULL ? !sift_v6_topk_run_next(st->topk, slot->tts_values, slot->tts_isnull) : !v6_group_next(st, slot))
		return NULL;
	return ExecStoreVirtualTuple(slot);
}

static bool
v6_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
v6_exec(CustomScanState *node)
{
	MemoryContext old = CurrentMemoryContext;
	TupleTableSlot *slot = ExecScan(&node->ss, v6_next, v6_recheck);

	MemoryContextSwitchTo(old);
	return slot;
}

static void
v6_fetch_end(V6State *st)
{
	for (int k = 0; k < st->nsources; k++)
	{
		SiftV6ScanSrc   *src = &st->sources[k];

		if (src->fetch == NULL)
			continue;
		table_index_fetch_end(src->fetch);
		ExecDropSingleTupleTableSlot(src->fetch_slot);
		src->fetch = NULL;
		src->fetch_slot = NULL;
	}
}

/* the counters of sift.stats() a scan adds when it ends: the ones of its EXPLAIN ANALYZE */
static void
v6_fold_stats(const V6State *st)
{
	SiftV6ParSummary ps = {0};
	uint64		spills = 0,
				spill_bytes;

	if (st->par != NULL)
		sift_v6_par_node_summary(st->par, &ps);
	if (!st->ran && !ps.ran)
		return;
	if (st->group != NULL)
		sift_v6_group_spill_stats(st->group, &spills, &spill_bytes);
	sift_stat_add(SIFT_STAT_GROUP_SPILLS, spills > 0);
	sift_stat_add(SIFT_STAT_SCANS, 1);
	sift_stat_add(SIFT_STAT_WORKERS, ps.workers);
	sift_stat_add(SIFT_STAT_UNITS, st->stats.grans + ps.grans_scanned);
	sift_stat_add(SIFT_STAT_UNITS_PREAGG, st->stats.grans_preagg + ps.grans_preagg);
	sift_stat_add(SIFT_STAT_ROWS_DECODED, st->stats.rows);
	sift_stat_add(SIFT_STAT_ROWS_VETTED, st->stats.heap_vet_rows + ps.heap_vet_rows);
}

/* The sorts the heaps of ORDER BY .. LIMIT moved to past work_mem end before their context goes */
static void
v6_heaps_end(const V6State *st)
{
	if (st->topk != NULL)
		sift_v6_topk_run_end(st->topk);
	if (st->rows != NULL)
		sift_v6_rows_end(st->rows);
}

static void
v6_end(CustomScanState *node)
{
	V6State    *st = (V6State *) node;

	v6_fold_stats(st);
	if (st->par != NULL)
		sift_v6_par_node_end(st->par);
	v6_heaps_end(st);
	v6_fetch_end(st);
	sift_v6_gpu_end(&st->gpu);
	if (st->group != NULL)
		sift_v6_group_free(st->group);
	if (st->gcx != NULL)
	{
		/* MEM[MEM-AGG-06]: v6_end teardown (the heap the groups of the query peaked at goes back to the system: sift_mem_release_os with the bytes of the groups context; was never trimmed) */
		const Size	peak = MemoryContextMemAllocated(st->gcx, true);

		MemoryContextDelete(st->gcx);
		sift_mem_release_os(peak);
	}
	sift_v6_fuse_detach(&st->arm);
	foreach_ptr(PlanState, child, node->custom_ps)
		ExecEndNode(child);
	for (int k = 0; k < st->nsources; k++)
	{
		table_close(st->sources[k].heap, AccessShareLock);
		if (st->sources[k].index != NULL)
			index_close(st->sources[k].index, AccessShareLock);
	}
}

static void
v6_rescan(CustomScanState *node)
{
	V6State    *st = (V6State *) node;

	if (st->par != NULL)
		sift_v6_par_node_end(st->par);
	v6_heaps_end(st);
	v6_fetch_end(st);
	sift_v6_gpu_end(&st->gpu);
	if (st->group != NULL)
		sift_v6_group_free(st->group);
	if (st->gcx != NULL)
		MemoryContextDelete(st->gcx);
	st->gcx = NULL;
	st->rows = NULL;
	st->gpu = (SiftV6Gpu) {0};
	st->group = NULL;
	st->ran = false;
	st->star_passes = 0;
	st->star_shared = 0;
	if (st->star != NULL)
		sift_v6_star_close(st->star);
	foreach_ptr(PlanState, child, node->custom_ps)
		if (node->ss.ps.chgParam != NULL)
			UpdateChangedParamSet(child, node->ss.ps.chgParam);
	st->proj = (SiftProjResult) {0};
	st->topk = NULL;
	st->stats = (SiftV6ScanStats) {0};
	sift_v6_fuse_detach(&st->arm);
}

/* the groups the heaps of the top-N left out: the node's, or those of every participant of a parallel run */
static uint64
topk_dropped(const V6State *st)
{
	SiftV6ParSummary ps = {0};

	if (st->par != NULL)
		sift_v6_par_node_summary(st->par, &ps);
	return ps.ran ? ps.topk_dropped : st->topk != NULL ? sift_v6_topk_run_dropped(st->topk) : 0;
}

static void
explain_par(const SiftV6ParNode *par, SiftExplainStatsV6 *ex)
{
	SiftV6ParSummary ps;

	sift_v6_par_node_summary(par, &ps);
	ex->par_strategy = ps.strategy;
	ex->par_ran = ps.ran;
	ex->par_escalated = ps.escalated;
	ex->grans_scanned += ps.grans_scanned;
	ex->grans_pruned_zone += ps.grans_pruned_zone;
	ex->grans_pruned_skip += ps.grans_pruned_skip;
	ex->grans_preagg += ps.grans_preagg;
	ex->grans_preagg_delta += ps.grans_preagg_delta;
	ex->heap_vet_rows += ps.heap_vet_rows;
	ex->par_morsels_stolen = ps.morsels_stolen;
	ex->par_radix_parts = ps.radix_parts;
	ex->par_partial_recs = ps.partial_recs;
	ex->par_partial_bytes = ps.partial_bytes;
}

static void
v6_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	V6State    *st = (V6State *) node;
	SiftExplainStatsV6 ex = {.group_mode = "salted-ht"};
	uint32		dim_hits,
				dim_built;

	ExplainPropertyText(st->sources[0].index == NULL ? "Table" : "Index", RelationGetRelationName(st->sources[0].index == NULL ? st->sources[0].heap : st->sources[0].index), es);
	if (st->spec->nparts > 0)
		ExplainPropertyInteger("Partitions", NULL, st->spec->nparts, es);
	sift_v6_explain_reads(st->spec, st->sources[0].heap, st->sources[0].index, es);
	if (st->spec->rows)
		sift_v6_rows_explain(es, st->spec);
	if (es->analyze && st->stats.global_order_reads > 0)
		ExplainPropertyBool("Global Order", true, es);
	if (!st->spec->rows)
	{
		sift_v6_explain_group_keys(st->spec, es);
		if (st->spec->topk > 0)
			sift_v6_explain_topn_groups(st->spec, es);
		sift_v6_explain_pushed_aggs(node, ancestors, es);
		if (es->analyze && (st->topk != NULL || (st->par != NULL && st->spec->topk > 0)))
			ExplainPropertyUInteger("Groups Dropped By Top-N", NULL, topk_dropped(st), es);
	}
	if (OidIsValid(st->spec->text_indexoid) && es->analyze)
	{
		ExplainPropertyInteger("Bridge Chunks", NULL, st->stats.chunks, es);
		ExplainPropertyInteger("Bridge TIDs", NULL, st->stats.bridge_tids, es);
	}
	if (es->analyze && st->spec->ext != NULL && st->spec->ext->unnest)
		ExplainPropertyUInteger("Rows Read Again From The Heap", NULL, st->stats.heap_reread_rows, es);
	if (st->arm.cohort != NULL)
	{
		const SiftV6FuseCohort *c = st->arm.cohort;

		ExplainPropertyInteger("Fused Arms", NULL, c->narms, es);
		ExplainPropertyText("Fused Role", sift_v6_fuse_role(&st->arm), es);
		ExplainPropertyText("Fused Keys", c->same_keys ? "shared" : "union", es);
		ExplainPropertyInteger("Shared Rows Vetted", NULL, (int64) c->rows_shared_vetted, es);
	}
	if (st->proj.used)
	{
		ExplainPropertyText("Projection", st->proj.name, es);
		ExplainPropertyInteger("Projection Parts", NULL, st->proj.nparts, es);
		ExplainPropertyInteger("Projection Parts Subtracted", NULL, st->proj.nsubtracted, es);
		ExplainPropertyInteger("Projection Base Parts", NULL, st->proj.nbase, es);
		ExplainPropertyInteger("Projection Base Rows", NULL, st->proj.nrest_rows, es);
	}
	if (st->group != NULL)
	{
		ex.group_mode = sift_v6_group_mode(st->group);
		ex.peak_mem_bytes = sift_v6_group_bytes(st->group);
		sift_v6_group_spill_stats(st->group, &ex.spill_count, &ex.spill_bytes);
	}
	if (st->spec->ext != NULL && st->spec->ext->ndims > 0)
	{
		ExplainPropertyInteger("Star Dimensions", NULL, st->spec->ext->ndims, es);
		sift_v6_explain_lookup_joins(&st->css, st->spec->ext, es);
	}
	if (st->spec->ext != NULL && st->spec->ext->unnest && st->spec->ext->elem.fn == SV6E_KEYS)
		ExplainPropertyText("Map Keys", sift_v6_mapkey_text(st->spec->ext, RelationGetDescr(st->sources[0].index)), es);
	else if (st->spec->ext != NULL && st->spec->ext->unnest)
		ExplainPropertyText("Unnest", st->spec->ext->elem.left ? "left lateral columnar" : "lateral columnar", es);
	if (st->spec->ext != NULL && st->spec->ext->unnest && st->spec->ext->elem.vread != SV6V_NONE)
		ExplainPropertyText("Unnest Value", psprintf("%s.*.value", NameStr(TupleDescAttr(RelationGetDescr(st->sources[0].index), st->spec->ext->elem.attno - 1)->attname)), es);
	if (st->star_passes > 1)
		ExplainPropertyInteger("Star Passes", NULL, st->star_passes, es);
	if (st->star != NULL && sift_v6_star_cache(st->star, &dim_hits, &dim_built))
		ExplainPropertyText("Dimension Cache", psprintf("%u hit, %u built", dim_hits, dim_built), es);
	if (st->star_shared > 0)
		ExplainPropertyInteger("Shared Dimensions", NULL, st->star_shared, es);
	sift_v6_gpu_explain(&st->gpu, es);
	ex.heap_vet_rows = st->stats.heap_vet_rows;
	ex.grans_scanned = st->stats.grans;
	ex.grans_pruned_zone = st->stats.grans_pruned_zone;
	ex.src_filters = st->stats.src_filters;
	ex.grans_pruned_skip = st->stats.grans_pruned_skip;
	ex.grans_preagg = st->stats.grans_preagg;
	ex.grans_preagg_delta = st->stats.grans_preagg_delta;
	ex.dict_shared_hits = sift_stat_v[SIFT_STAT_DICT_SHARED_HITS] - st->dict_shared0[0];
	ex.dict_shared_misses = sift_stat_v[SIFT_STAT_DICT_SHARED_MISSES] - st->dict_shared0[1];
	if (st->par != NULL)
		explain_par(st->par, &ex);
	if (st->par != NULL && !es->analyze)
		ExplainPropertyText("Parallel Strategy", ex.par_strategy, es);
	if (es->analyze)
		sift_agg_explain_v6(es, &ex, sift_simd_level_name(sift_cpu.active), 0, 0);
}

static Size
v6_par_estimate(CustomScanState *node, ParallelContext *pcxt)
{
	return sift_v6_par_node_estimate(((V6State *) node)->par, pcxt);
}

static void
v6_par_initialize(CustomScanState *node, ParallelContext *pcxt, void *coordinate)
{
	sift_v6_par_node_initialize(((V6State *) node)->par, pcxt, coordinate);
}

static void
v6_par_reinitialize(CustomScanState *node, ParallelContext *pcxt, void *coordinate)
{
	sift_v6_par_node_reinitialize(((V6State *) node)->par, coordinate);
}

static void
v6_par_worker(CustomScanState *node, shm_toc *toc, void *coordinate)
{
	sift_v6_par_node_worker(((V6State *) node)->par, coordinate);
}

static void
v6_par_shutdown(CustomScanState *node)
{
	V6State    *st = (V6State *) node;

	if (st->par != NULL)
		sift_v6_par_node_shutdown(st->par);
}

static const CustomExecMethods v6_exec_methods = {
	.CustomName = "SiftAggV6",
	.BeginCustomScan = v6_begin,
	.ExecCustomScan = v6_exec,
	.EndCustomScan = v6_end,
	.ReScanCustomScan = v6_rescan,
	.ExplainCustomScan = v6_explain,
	.EstimateDSMCustomScan = v6_par_estimate,
	.InitializeDSMCustomScan = v6_par_initialize,
	.ReInitializeDSMCustomScan = v6_par_reinitialize,
	.InitializeWorkerCustomScan = v6_par_worker,
	.ShutdownCustomScan = v6_par_shutdown,
};

static Node *
v6_create_state(CustomScan *cscan)
{
	V6State    *st = (V6State *) newNode(sizeof(V6State), T_CustomScanState);

	st->css.methods = &v6_exec_methods;
	return (Node *) st;
}

const CustomScanMethods sift_agg_v6_plan_methods = {"SiftAggV6", v6_create_state};
