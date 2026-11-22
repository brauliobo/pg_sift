/*
 * agg/v6/exec.c - the executor node of SiftAggV6: the scan of the index folds every row into the groups when the node is first asked for a tuple, then one tuple leaves for each group.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/parallel.h"
#include "access/table.h"
#include "common/hashfn.h"
#include "commands/explain.h"
#include "executor/nodeIndexscan.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_ext.h"
#include "agg/v6/v6_fuse.h"
#include "agg/v6/v6_fuse_keys.h"
#include "agg/v6/v6_gpu.h"
#include "agg/v6/v6_par_node.h"
#include "agg/v6/v6_proj_exec.h"
#include "agg/v6/v6_star_run.h"
#include "agg/v6/v6_unnest_run.h"
#include "agg/v6/v6_qual.h"
#include "agg/v6/v6_rows_topn.h"
#include "agg/v6/v6_topk_run.h"
#include "access/tableam.h"
#include "col/am.h"
#include "col/f2/f2.h"
#include "agg/exec/int.h"
#include "col/f2/f2_store_scan.h"
#include "core/cpu.h"
#include "core/errctx.h"
#include "core/memtrack.h"

typedef struct V6Source
{
	Relation	heap;
	Relation	index;			/* a sift_col index; NULL for a sift_table, which heap is */
	ScanKey		keys;
	int			nkeys;
	IndexRuntimeKeyInfo *rtkeys;
	int			nrt;
	SiftStoreQuals quals;		/* a sift_table: the keys and the predicates of its scan */
} V6Source;

typedef struct V6State
{
	CustomScanState css;
	SiftV6Spec *spec;
	int			nsources;
	V6Source   *sources;		/* the index of the plan, or the index of each live partition of a partitioned table */
	MemoryContext gcx;
	SiftV6Group *group;
	SiftV6ScanStats stats;
	SiftV6ProjResult proj;
	SiftV6Gpu	gpu;
	SiftV6StarRun *star;		/* the dimensions of a star join */
	uint32		star_passes;
	SiftV6TopKRun *topk;
	SiftV6Rows *rows;
	IndexFetchTableData *fetch;
	TupleTableSlot *fetch_slot;
	bool		ran;
	bool		explain_only;
	SiftV6FuseArm arm;			/* in the registry of the statement: arms of one index and one set of keys scan once */
	SiftV6ParNode *par;			/* a parallel node: the participants' scan of the units of the index (v6_par_node.c) */
} V6State;

/* Arms of equal keys read the same rows: the keys of the scan are constants, so their text tells them apart */
static uint64
scan_signature(const SiftV6Spec *spec)
{
	const char *text = nodeToString(spec->indexqual);

	return DatumGetUInt64(hash_any_extended((const unsigned char *) text, strlen(text), 0));
}

static void
begin_index(V6State *st, CustomScanState *node, V6Source *src, Oid indexoid)
{
	src->index = index_open(indexoid, AccessShareLock);
	src->heap = table_open(src->index->rd_index->indrelid, AccessShareLock);
	sift_v6_qual_scankeys(&node->ss.ps, src->index, st->spec->indexqual, &src->keys, &src->nkeys, &src->rtkeys, &src->nrt);
	sift_v6_json_scankeys(st->spec, src->index, &src->keys, &src->nkeys);
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

static bool
v6_fusable(const V6State *st)
{
	return st->spec->nparts == 0 && !st->spec->is_table && st->spec->ext == NULL && st->spec->par_strategy == 0 && !OidIsValid(st->spec->text_indexoid)
		&& !st->spec->rows && st->spec->topk == 0;
}

static void
begin_arm(V6State *st, CustomScanState *node, EState *estate, int eflags)
{
	const V6Source *src = &st->sources[0];

	if (!v6_fusable(st))
		return;
	st->explain_only = (eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0;
	st->arm = (SiftV6FuseArm) {.owner = st,.indexoid = st->spec->relid,.snap = estate->es_snapshot,.scan_sig = scan_signature(st->spec),
	.parallel = node->ss.ps.plan->parallel_aware || IsParallelWorker(),.param_exec = node->ss.ps.plan->extParam != NULL};
	sift_v6_fuse_describe(&st->arm, src->index, st->spec);
	sift_v6_fuse_arm_add(estate, &st->arm);
}

/* The quals of a parallel scan are constants or parameters of the statement: the participants read them once, as the leader does */
static void
begin_par(V6State *st, CustomScanState *node, EState *estate)
{
	V6Source   *src = &st->sources[0];

	if (src->nrt > 0)
		ExecIndexEvalRuntimeKeys(node->ss.ps.ps_ExprContext, src->rtkeys, src->nrt);
	src->nrt = 0;
	st->par = sift_v6_par_node_begin(st->spec, node->ss.ss_ScanTupleSlot->tts_tupleDescriptor, src->heap, src->index, src->keys, src->nkeys, estate->es_snapshot, estate->es_query_cxt,
									 node->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
}

static void
v6_begin(CustomScanState *node, EState *estate, int eflags)
{
	V6State    *st = (V6State *) node;

	st->spec = sift_v6_spec_unpack(((CustomScan *) node->ss.ps.plan)->custom_private);
	st->nsources = Max(st->spec->nparts, 1);
	st->sources = palloc0(sizeof(V6Source) * st->nsources);
	if (st->spec->is_table)
	{
		V6Source   *src = &st->sources[0];

		src->heap = table_open(st->spec->relid, AccessShareLock);
		sift_f2_store_compile_quals(src->heap, sift_f2_store_open(src->heap), st->spec->indexqual, estate->es_query_cxt, &src->quals);
		return;
	}
	for (int k = 0; k < st->nsources; k++)
		begin_index(st, node, &st->sources[k], st->spec->nparts > 0 ? st->spec->part_index[k] : st->spec->relid);
	if (st->spec->ext != NULL && st->spec->ext->ndims > 0)
		begin_star(st, node, estate, eflags);
	begin_arm(st, node, estate, eflags);
	if (st->spec->par_strategy != 0)
		begin_par(st, node, estate);
}

static bool
v6_serve_projection(V6State *st, Snapshot snap)
{
	V6Source   *src = &st->sources[0];

	if (st->spec->nparts > 0 || st->spec->ext != NULL || OidIsValid(st->spec->text_indexoid))
		return false;
	if (src->index != NULL)
		return sift_v6_proj_serve(st->spec, src->heap, src->index, src->keys, src->nkeys, snap, st->gcx, &st->proj);
	return src->quals.possible && sift_v6_proj_serve_table(st->spec, src->heap, src->quals.keys, src->quals.nkeys, snap, st->gcx, &st->proj);
}

/* The scan of the fact table of a join or an unnest: once for each pass of the dimensions of a star join (one when they all fit work_mem) */
static void
v6_scan_ext(V6State *st, Snapshot snap)
{
	const V6Source *src = &st->sources[0];
	SiftStarOpen open;
	uint32		pass = 0;

	if (st->spec->ext->unnest)
	{
		sift_v6_unnest_run(st->spec, st->group, src->heap, src->index, snap, &st->stats);
		return;
	}
	while ((open = sift_v6_star_open(st->star, pass++)) != SSO_DONE)
	{
		MemoryContext pass_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 star pass");
		MemoryContext old = MemoryContextSwitchTo(pass_cx);

		if (open == SSO_SCAN)
			sift_v6_scan_run_ext(st->spec, st->group, src->heap, src->index, src->keys, src->nkeys, snap, NULL, 0, &st->stats, st->star, NULL, NULL);
		MemoryContextSwitchTo(old);
		MemoryContextDelete(pass_cx);
	}
	st->star_passes = sift_v6_star_passes(st->star);
	sift_v6_star_close(st->star);
}

static void
v6_scan(V6State *st, Snapshot snap)
{
	TupleDesc	desc = st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	SiftV6Text *text = OidIsValid(st->spec->text_indexoid) ? sift_v6_text_open(st->spec, CurrentMemoryContext) : NULL;

	if (st->spec->nparts == 0 && st->spec->ext == NULL && text == NULL)
		st->group = sift_v6_gpu_agg(st->spec, st->sources[0].heap, st->sources[0].index, snap, st->css.ss.ps.plan->plan_rows, st->gcx, desc, &st->gpu);
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
		V6Source   *src = &st->sources[k];

		if (src->index != NULL)
			sift_v6_scan_run(st->spec, st->group, src->heap, src->index, src->keys, src->nkeys, snap, st->gpu.skip, st->gpu.nskip, &st->stats,
							 text ? sift_v6_text_chunk : NULL, text);
		else
			sift_v6_scan_table(st->spec, st->group, src->heap, &src->quals, snap, st->gpu.skip, st->gpu.nskip, &st->stats);
	}
	sift_v6_group_finish(st->group);
	if (text != NULL)
		sift_v6_text_close(text);
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
	const V6Source *src = &st->sources[0];
	IndexRuntimeKeyInfo *rtkeys = NULL;
	ScanKey		keys;
	int			nrt = 0;

	if (c->same_keys)
		return (*nkeys = src->nkeys, src->keys);
	sift_v6_qual_scankeys(&st->css.ss.ps, src->index, quals, &keys, nkeys, &rtkeys, &nrt);
	SIFT_INVARIANT(nrt == 0, "the quals of the aggregate scan are constants");
	sift_v6_json_scankeys(st->spec, src->index, &keys, nkeys);
	return keys;
}

/* One pass for the arms of the cohort that this node leads: the groups of each are done when its node is asked */
static void
v6_run_fused(V6State *st, SiftV6FuseCohort *c, EState *estate)
{
	const V6Source *src = &st->sources[0];
	SiftV6ScanArm *arms = palloc(c->narms * sizeof(SiftV6ScanArm));
	List	   *common = NIL;
	List	   *quals = c->same_keys ? NIL : sift_v6_fuse_union_quals(c, src->index, &common);
	int			nkeys;
	ScanKey		keys = pass_keys(st, c, quals, &nkeys);

	for (int i = 0; i < c->narms; i++)
	{
		V6State    *owner = c->arms[i]->owner;
		SiftV6Spec *pass_spec;
		struct SiftClause *gate;

		arms[i] = arm_groups(owner, estate);
		if (!c->same_keys && sift_v6_fuse_gate(estate->es_query_cxt, c->arms[i], common, owner->spec, &pass_spec, &gate))
		{
			arms[i].spec = pass_spec;
			arms[i].gate = gate;
		}
	}
	sift_v6_scan_run_arms(c, arms, src->heap, src->index, keys, nkeys, estate->es_snapshot);
	for (int i = 0; i < c->narms; i++)
		sift_v6_group_finish(arms[i].group);
}

static bool
proj_stream_next(void *arg, Datum *values, bool *isnull)
{
	return sift_v6_proj_next(arg, values, isnull);
}

static void
v6_run_rows(V6State *st, EState *estate)
{
	const V6Source *src = &st->sources[0];

	st->rows = sift_v6_rows_new(st->gcx, st->spec);
	sift_v6_scan_rows(st->spec, st->rows, src->heap, src->index, src->keys, src->nkeys, estate->es_snapshot, &st->stats);
	sift_v6_rows_finish(st->rows);
	st->fetch = sift_table_index_fetch_begin(src->heap);
	st->fetch_slot = table_slot_create(src->heap, NULL);
}

/* the next winner of a row Top-N: its columns from the heap row at its address */
static bool
v6_next_row(V6State *st, TupleTableSlot *slot)
{
	ItemPointerData tid;
	bool		again = false,
				dead = false;
	uint64		addr;
	int			i = 0;

	if (!sift_v6_rows_next(st->rows, &addr))
		return false;
	sift_f2_tid_set(&tid, addr);
	SIFT_INVARIANT(table_index_fetch_tuple(st->fetch, &tid, st->css.ss.ps.state->es_snapshot, st->fetch_slot, &again, &dead), "a row the scan kept is not in the heap");
	foreach_node(TargetEntry, te, ((CustomScan *) st->css.ss.ps.plan)->custom_scan_tlist)
	{
		slot->tts_values[i] = slot_getattr(st->fetch_slot, ((Var *) te->expr)->varattno, &slot->tts_isnull[i]);
		i++;
	}
	return true;
}

static void
v6_run(V6State *st, EState *estate)
{
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	SiftV6FuseCohort *c;

	for (int k = 0; k < st->nsources; k++)
		if (st->sources[k].nrt > 0)
			ExecIndexEvalRuntimeKeys(st->css.ss.ps.ps_ExprContext, st->sources[k].rtkeys, st->sources[k].nrt);
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
			sift_v6_topk_run_stream(st->topk, proj_stream_next, &st->proj, ps->qual, ps->ps_ExprContext, slot);
		else
			sift_v6_topk_run_groups(st->topk, st->group, ps->qual, ps->ps_ExprContext, slot);
	}
	MemoryContextSwitchTo(old);
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
		return sift_v6_proj_next(&st->proj, slot->tts_values, slot->tts_isnull);
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
	if (st->fetch == NULL)
		return;
	table_index_fetch_end(st->fetch);
	ExecDropSingleTupleTableSlot(st->fetch_slot);
	st->fetch = NULL;
	st->fetch_slot = NULL;
}

static void
v6_end(CustomScanState *node)
{
	V6State    *st = (V6State *) node;

	if (st->par != NULL)
		sift_v6_par_node_end(st->par);
	v6_fetch_end(st);
	sift_v6_gpu_end(&st->gpu);
	if (st->group != NULL)
		sift_v6_group_free(st->group);
	if (st->gcx != NULL)
		MemoryContextDelete(st->gcx);
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
	if (st->star != NULL)
		sift_v6_star_close(st->star);
	foreach_ptr(PlanState, child, node->custom_ps)
		if (node->ss.ps.chgParam != NULL)
			UpdateChangedParamSet(child, node->ss.ps.chgParam);
	st->proj = (SiftV6ProjResult) {0};
	st->topk = NULL;
	st->stats = (SiftV6ScanStats) {0};
	sift_v6_fuse_detach(&st->arm);
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

	ExplainPropertyText(st->spec->is_table ? "Table" : "Index", RelationGetRelationName(st->spec->is_table ? st->sources[0].heap : st->sources[0].index), es);
	if (st->spec->nparts > 0)
		ExplainPropertyInteger("Partitions", NULL, st->spec->nparts, es);
	if (OidIsValid(st->spec->text_indexoid))
	{
		ExplainPropertyText("Text Index", get_rel_name(st->spec->text_indexoid), es);
		if (es->analyze)
		{
			ExplainPropertyInteger("Bridge Chunks", NULL, st->stats.chunks, es);
			ExplainPropertyInteger("Bridge TIDs", NULL, st->stats.bridge_tids, es);
		}
	}
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
		ExplainPropertyInteger("Star Dimensions", NULL, st->spec->ext->ndims, es);
	if (st->spec->ext != NULL && st->spec->ext->unnest)
		ExplainPropertyText("Unnest", st->spec->ext->elem.left ? "left lateral columnar" : "lateral columnar", es);
	if (st->star_passes > 1)
		ExplainPropertyInteger("Star Passes", NULL, st->star_passes, es);
	sift_v6_gpu_explain(&st->gpu, es);
	ex.heap_vet_rows = st->stats.heap_vet_rows;
	ex.grans_scanned = st->stats.batches;
	if (st->par != NULL)
		explain_par(st->par, &ex);
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
