/*
 * agg/exec/run.c - the scan of SiftAgg as the executor runs it
 * Begin, next, rescan and end of the custom scan, the engine it opens over the source, and
 * the groups it hands out.
 */
#include "postgres.h"

#include "access/table.h"
#include "optimizer/optimizer.h"
#include "storage/predicate.h"
#include "utils/float.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/stats.h"
#include "col/am.h"
#include "agg/exec/int.h"

bool		sift_enable_agg = true;
bool		sift_agg_force = false;
double		sift_agg_scale_horizon = 100.0;
bool		sift_agg_preagg = true;
int			sift_agg_dead_delta = 15;
bool		sift_agg_super = true;
bool		sift_agg_dense = true;
bool		sift_agg_dist_stream = true;
bool		sift_agg_semi_lut = true;
bool		sift_agg_two_level_on = true;
int		sift_agg_semi_lut_size = 4096;
bool		sift_agg_lookup_pushdown = true;
bool		sift_agg_lookup_columns = true;
bool		sift_agg_join_filter = true;
bool		sift_agg_left_lateral = true;
bool		sift_agg_key_skew = true;
bool		sift_agg_share_lookups = true;
bool		sift_agg_fuse = true;
bool		sift_enable_topn = true;
bool		sift_topn_walk = true;
bool		sift_test_agg_wait = false;
int		sift_agg_combo_cache = 1 << 20;

static const char null_dict_id = 0;
static const uint32 null_dict_off[1] = {0};
static const SiftSrcDict null_dict = {&null_dict_id, 0, null_dict_off, ""};
static const uint64 null_data[SIFT_AGG_PAD] pg_attribute_aligned(SIFT_SIMD_ALIGN);
static const uint8 null_flags[SIFT_AGG_PAD] = {[0 ... SIFT_AGG_PAD - 1] = 1};

/* a unit of one row whose leaves are all NULL: what a row of the preserved side of an outer join joins to when no row of the scan matched it */
static void
null_unit(AggExec *ex)
{
	ex->null_input = true;
	MemoryContextReset(ex->ucxt);
	memset(ex->gofs, 0, ex->lay->nleaves * sizeof(uint32 *));
	memset(ex->gpar, 0, ex->lay->nleaves * sizeof(uint32 *));
	for (int i = 0; i < ex->spec->npreds_total; i++)
	{
		ex->ps[i].rowmask_ok = false;
		ex->ps[i].poison_ok = false;
	}
	for (int leaf = 0; leaf < ex->lay->nleaves; leaf++)
		ex->uvec[leaf] = (SiftSrcVec) {null_data, null_flags, &null_dict, 1, NULL};
	memset(ex->uvec_ok, true, ex->lay->nleaves * sizeof(bool));
	ex->unit.nrow = 1;
	ex->rsel = NULL;
	ex->qlevel = 0;
	ex->cstart = 0;
	ex->cn = 1;
}

static bool
join_preserved(const SiftAggSpec *s)
{
	for (int j = 0; j < s->njoin; j++)
		if (sift_aggjoin_preserves(s->join[j].kind))
			return true;
	return false;
}

/* Rows the nullable input never matched: every earlier lookup column is NULL as well. */
static void
unmatched_tail(AggExec *ex)
{
	int			ci;
	Tup		   *own;
	ComboEnt	ent;
	uint32	   *prior = palloc0(Max(ex->spec->ncombo, 1) * sizeof(uint32));

	if (ex->nj == 0 || !ex->cs[ex->jcombo[ex->nj - 1]].map->keep)
		return;
	ci = ex->jcombo[ex->nj - 1];
	own = stage_tuples(ex, ex->nj - 1);
	null_unit(ex);
	for (int x = 0; x < ex->nj - 1; x++)
	{
		int			prev = ex->jcombo[x];
		const SiftAggCombo *cb = &ex->spec->combo[prev];
		uint64		v[SIFT_AGG_MAX_CLEAVES] = {0};
		ComboEnt	null;
		MemoryContext old = MemoryContextSwitchTo(ex->comcxt);

		for (int p = 0; p < cb->nleaves; p++)
			if (cb->leaf[p] < 0)
				v[p] = combo_prior_vid(ex, prev, p, prior[cb->refcombo[p]]);
		combo_bind_leaves(ex, prev, v);
		combo_emit(ex, prev, &null, NULL, 0, 1);
		prior[prev] = null.base;
		MemoryContextSwitchTo(old);
		MemoryContextReset(ex->comcxt);
	}
	ent = combo_unmatched(ex, ci, prior);
	for (uint32 e = 0; e < ent.nout;)
	{
		int			k = 0;

		for (; k < VEC && e < ent.nout; k++)
		{
			own->sel[k] = own->tidx[k] = own->ridx[k] = 0;
			for (int x = 0; x < ex->nj - 1; x++)
				own->cout[ex->jcombo[x]][k] = prior[ex->jcombo[x]];
			own->cout[ci][k] = ent.base + e++;
		}
		own->n = k;
		agg_tuples(ex, own);
	}
	if (ex->lane_serial)
		lane_fold(ex);
}

void
run_scan(AggExec *ex)
{
	bool		ordered,
				none;

	ex->null_input = false;
	ex->started = true;
	ex->glist = NULL;
	engine_open(ex, ex->snap, ex->params);
	/* after engine_open: a correlated child read as a lookup table (join_first_rows) has no parameters, which the lanes of a scan with float sums ask of the spec */
	ordered = sift_aggspec_ordered(ex->spec);
	scan_buffers(ex);
	none = join_none(ex);
	ex->ndsorder = 0;
	ex->dsabort = false;
	/* a streaming count(DISTINCT) scans in the order of its zones, in one process */
	if (!backend_begin(ex) && !none && !dist_stream_begin(ex))
		par_launch(ex);
	if (ex->shared == NULL)
	{
		ex->shared = MemoryContextAlloc(ex->cxt, shared_size(ex->spec, 0));
		shared_init(ex->shared, ex);
	}
	/* a join without a row to match leaves no tuple: no item of work to take */
	if (none)
		pg_atomic_write_u32(&ex->shared->next, ex->shared->nitems);
	ex->lane_serial = ordered && ex->par == NULL;
	ex->lanes = ordered && !ex->lane_serial;
	spill_begin(ex);
	table_init(ex);
	if (ex->spec->spine)
		spine_load(ex);
	if (ex->ndsorder > 0)
		dist_stream_scan(ex);
	else if (ex->par == NULL || parallel_leader_participation)
		scan_items(ex);
	backend_finish(ex);
	/* the rows of a preserved side that no tuple matched, and of a spine that no cell joined: after the workers told which they matched */
	if (ex->par && (join_preserved(ex->spec) || ex->spec->spine))
		par_finish(ex);
	unmatched_tail(ex);
	arms_flush(ex);
	if (ex->spec->spine)
		spine_tail(ex);
	if (ex->par && ex->par->pcxt)
		par_finish(ex);
	if (spill_started(ex))
		spill_finish(ex);
	for (int i = 0; ex->lane_serial && i < ex->spec->naggs; i++)
		if (ex->acc[i].dt)
			memcpy(ex->acc[i].d, ex->acc[i].dt, (ex->hash ? ex->ngroups : ex->gcap) * sizeof(double));
	if (ex->lanes)
		lanes_sum(ex);
	osa_finish_all(ex);
	gen_flush(ex);
	final_checks(ex, ex->n_tuples);
}

/* the groups that exist, in index order */
void
list_groups(AggExec *ex)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap,
				k = 0;

	ex->glist = MemoryContextAlloc(ex->cxt, Max(n, 1) * sizeof(uint32));
	for (uint32 g = 0; g < n; g++)
		if (ex->seen[g] > 0)
			ex->glist[k++] = g;
	if (ex->spec->nkeys == ex->spec->nhidden && k == 0)
		ex->glist[k++] = 0;
	ex->nglist = k;
	ex->n_groups = k;
}

Datum
key_datum(AggExec *ex, int j, uint32 g, bool *isnull)
{
	return key_id_datum(ex, j, key_id(ex, j, g), isnull);
}

/* the value of id `id` of key j */
Datum
key_id_datum(AggExec *ex, int j, uint32 id, bool *isnull)
{
	KeyState   *k = &ex->ks[j];

	*isnull = id == 0;
	if (id == 0)
		return (Datum) 0;
	if (k->numk)
		return ex->spec->key[j].typid == NUMERICOID ? scaled_numeric(k->im.val[id - 1], ex->spec->key[j].num.scale) :
			leaf_datum(ex->spec->key[j].typid, (int64) k->im.val[id - 1]);
	if (k->ident)
		/* PG_INT64_MIN: the bits of -0 */
		return id == k->negzero ? leaf_datum(ex->spec->key[j].typid, PG_INT64_MIN) : leaf_datum(ex->spec->key[j].typid, k->idval[id - 1]);
	return k->ot.val[id - 1];
}

Datum
agg_datum(AggExec *ex, int ai, uint32 g, bool *isnull)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];

	*isnull = false;
	switch (a->kind)
	{
			case SAA_COUNT_STAR:
		case SAA_COUNT:
		case SAA_COUNT_ROWS:
		case SAA_COUNT_DISTINCT:
		case SAA_O_COUNT_STAR:
		case SAA_O_COUNT_ROWS:
		case SAA_O_COUNT_KEY:
		case SAA_M_COUNT:
		case SAA_M_DISTINCT:
		case SAA_X_COUNT:
			return Int64GetDatum(acc->n[g]);
		case SAA_GROUPING:
			return Int32GetDatum(grouping_bits(ex, a, g));
		case SAA_M_MIN:
		case SAA_M_MAX:
		case SAA_X_MIN:
		case SAA_X_MAX:
			*isnull = acc->n[g] == 0;
			return acc->v[g];
		case SAA_M_SUM:
		case SAA_M_AVG:
		case SAA_X_SUM:
		case SAA_X_AVG:
			return mapped_sum_datum(ex, a, ai, g, isnull);
		case SAA_GEN:
			*isnull = (acc->nf[g] & GEN_RNULL) != 0;
			return *isnull ? (Datum) 0 : acc->r[g];
		case SAA_M_PG:
			*isnull = acc->n[g] == 0 || !acc->nf[g];
			return *isnull ? (Datum) 0 : acc->v[g];
		case SAA_O_SUM_I64:
			*isnull = acc->n[g] == 0;
			return sift_col_w128_numeric(acc->w[g], 0);
		case SAA_O_SUM_F64:
			*isnull = acc->n[g] == 0;
			if (isinf(acc->d[g]) && !acc->nf[g])
				float_overflow_error();
			return Float8GetDatum(acc->d[g]);
		case SAA_O_AVG_F64:
			*isnull = acc->n[g] == 0;
			return Float8GetDatum(acc->d[g] / (double) acc->n[g]);
		case SAA_SUM_INT:
			*isnull = acc->n[g] == 0;
			return Int64GetDatum(acc->i[g]);
		case SAA_SUM_F64:
			*isnull = acc->n[g] == 0;
			if (isinf(acc->d[g]) && !acc->nf[g])
				float_overflow_error();
			return Float8GetDatum(acc->d[g]);
		case SAA_AVG_INT:
			if (acc->n[g] == 0)
			{
				*isnull = true;
				return (Datum) 0;
			}
			return DirectFunctionCall2(numeric_div, DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->i[g])),
									   DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->n[g])));
		case SAA_AVG_F64:
			*isnull = acc->n[g] == 0;
			return Float8GetDatum(acc->d[g] / (double) acc->n[g]);
		case SAA_SUM_NUM:
			*isnull = acc->n[g] == 0;
			if (*isnull)
				return (Datum) 0;
			/* the sum of an int4 expression is a bigint, as int4_sum gives it */
			return a->restype == INT8OID ? DirectFunctionCall1(numeric_int8, agg_sum_datum(acc, g, a->num.scale)) : agg_sum_datum(acc, g, a->num.scale);
		case SAA_AVG_NUM:
			*isnull = acc->n[g] == 0;
			if (*isnull)
				return (Datum) 0;
			return DirectFunctionCall2(numeric_div, agg_sum_datum(acc, g, a->num.scale),
									   DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->n[g])));
		case SAA_PCT_CONT:
		case SAA_PCT_DISC:
			{
				OsaVal		v;

				*isnull = acc->n[g] == 0;
				if (*isnull)
					return (Datum) 0;
				v = ex->oval[ai][ex->acc[a->share].i[g] - 1];
				if (a->argtype == NUMERICOID && osa_int_key(a))
					return scaled_numeric(v.i == PG_INT64_MAX ? SIFT_NX_NAN : v.i, a->num.scale);
				if (osa_int_key(a))
					return leaf_datum(a->restype, v.i);
				if (a->restype == FLOAT4OID)
					return Float4GetDatum((float4) v.d);
				return a->restype == FLOAT8OID ? Float8GetDatum(v.d) : leaf_datum(a->restype, (int64) v.d);
			}
		case SAA_MIN:
		case SAA_MAX:
			*isnull = acc->n[g] == 0;
			if (!a->num.n && ex->lay->leaf[a->leaf].vtype == SCV_F64)
				return a->restype == FLOAT4OID ? Float4GetDatum((float4) acc->d[g]) : Float8GetDatum(acc->d[g]);
			if (a->num.n)
				return scaled_numeric(acc->w[g], a->num.scale);
			return leaf_datum(a->restype, acc->i[g]);
	}
	return (Datum) 0;
}

/* Windows of superblocks from the end the best values are at (agg/topn.h) */

/* Testing (sift.test_agg_wait): lets a test run VACUUM or DML after the scan has opened its source (test/t/topn_vacuum_scan.sh) */
void
agg_test_wait(void)
{
	LOCKTAG		tag;

	if (!sift_test_agg_wait)
		return;
	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, 0x51F7, 1, 2);
	(void) LockAcquire(&tag, ShareLock, false, false);
	LockRelease(&tag, ShareLock, false);
}

/* The groups of the scan as the aggregate pass of agg_next ends them */
void
finish_groups(AggExec *ex)
{
	empty_set_groups(ex);
	dist_finish_all(ex);
	dist_end_all(ex);
	mapped_finish(ex);
	list_groups(ex);
	gen_finish(ex);
	ex->n_groups = ex->nglist;
}

/* group g of table t as a tuple of the scan */
static void
group_tuple(AggExec *t, uint32 g, TupleTableSlot *slot)
{
	ExecClearTuple(slot);
	for (int j = 0; j < t->spec->nkeys; j++)
		slot->tts_values[j] = key_datum(t, j, g, &slot->tts_isnull[j]);
	for (int i = 0; i < t->spec->naggs; i++)
	{
		int			c = t->spec->nkeys + i;

		slot->tts_values[c] = agg_datum(t, i, g, &slot->tts_isnull[c]);
	}
	ExecStoreVirtualTuple(slot);
}

/* the next group of the table as a tuple of the scan (a spilled scan has one partition of them at a time) */
static bool
group_slot(AggExec *ex, TupleTableSlot *slot)
{
	AggExec    *t = ex;

	while (ex->gpos >= t->nglist)
		if (!spill_more(ex))
			return false;
	group_tuple(t, t->glist[ex->gpos++], slot);
	return true;
}

/*
 * The units of a partition are over and no other partition holds a row of its groups (the group key holds the partition key): the groups are
 * final. They go to the tuples of the result and the table starts again, so the scan holds the groups of one partition, not of all.
 */
void
part_flush(AggExec *ex)
{
	TupleTableSlot *slot = ex->css.ss.ss_ScanTupleSlot;
	MemoryContext tmp = AllocSetContextCreate(ex->cxt, "sift agg partition", ALLOCSET_DEFAULT_SIZES),
				old = MemoryContextSwitchTo(tmp);

	if (ex->done == NULL)
	{
		MemoryContextSwitchTo(ex->cxt);
		ex->done = tuplestore_begin_heap(false, false, work_mem);
		MemoryContextSwitchTo(tmp);
	}
	dist_finish_all(ex);
	mapped_finish(ex);
	list_groups(ex);
	for (uint32 i = 0; i < ex->nglist; i++)
	{
		group_tuple(ex, ex->glist[i], slot);
		tuplestore_puttupleslot(ex->done, slot);
	}
	ex->n_flushed += ex->nglist;
	ex->glist = NULL;
	ex->nglist = 0;
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
	table_reset(ex);
}

static TupleTableSlot *
agg_next(ScanState *node)
{
	AggExec    *ex = (AggExec *) node;
	TupleTableSlot *slot = node->ss_ScanTupleSlot;

	if (ex->spec->topn)
		return topn_next(ex, slot);
	if (!ex->started && !fuse_start(ex))
	{
		run_scan(ex);
		finish_groups(ex);
		ex->n_groups = ex->n_flushed ? ex->n_flushed : ex->n_groups;
		if (ex->spec->gtopn_k > 0 && !ex->done && !spill_started(ex))
			groups_topn(ex);
		if (ex->spec->nord > 0 && !ex->done && !spill_started(ex))
			groups_order(ex);
	}
	if (ex->done)
	{
		if (ex->done_slot == NULL)
			ex->done_slot = MakeSingleTupleTableSlot(slot->tts_tupleDescriptor, &TTSOpsMinimalTuple);
		/* the expressions above were compiled for the virtual slot of the node */
		if (!tuplestore_gettupleslot(ex->done, true, false, ex->done_slot))
			return NULL;
		return ExecCopySlot(slot, ex->done_slot);
	}
	if (ex->fuse)
		return fuse_next(ex, slot);
	if (spill_started(ex))
		return spill_output(ex, slot, group_slot);
	return group_slot(ex, slot) ? slot : NULL;
}

static bool
agg_recheck(ScanState *node, TupleTableSlot *slot)
{
	return true;
}

void
engine_contexts(AggExec *ex, MemoryContext parent)
{
	ex->cxt = AllocSetContextCreate(parent, "sift agg", ALLOCSET_DEFAULT_SIZES);
	ex->tcxt = AllocSetContextCreate(ex->cxt, "sift agg table", ALLOCSET_DEFAULT_SIZES);
	ex->ucxt = AllocSetContextCreate(ex->cxt, "sift agg unit", ALLOCSET_DEFAULT_SIZES);
}

void
engine_rels(AggExec *ex)
{
	if (OidIsValid(ex->spec->indexoid))
		ex->index = index_open(ex->spec->indexoid, AccessShareLock);
	ex->heap = table_open(ex->spec->heapoid, AccessShareLock);
	ex->heap_opened = true;
}

/* The scan engine on the relations of the spec: shared by the node and by a parallel worker */
void
engine_open(AggExec *ex, Snapshot snapshot, ParamListInfo params)
{
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	ex->dict_shared0[0] = sift_stat_v[SIFT_STAT_DICT_SHARED_HITS];
	ex->dict_shared0[1] = sift_stat_v[SIFT_STAT_DICT_SHARED_MISSES];
	ex->src = ex->spec->nparts > 0 ? sift_aggpart_open(ex->heap, ex->spec->nparts, ex->spec->partheap, ex->spec->partindex, snapshot, ex->cxt) :
		sift_colsrc_open(ex->heap, ex->index, snapshot, ex->cxt);
	if (ex->partoff)
		sift_aggpart_set_offsets(ex->src, ex->partoff);
	ex->lay = sift_colsrc_source_layout(ex->src);
	extend_layout(ex);
	ex->uvec = palloc0(ex->lay->nleaves * sizeof(SiftSrcVec));
	ex->uvec_ok = palloc0(ex->lay->nleaves * sizeof(bool));
	ex->nvec = palloc0(ex->lay->nleaves * sizeof(SiftSrcVec));
	ex->nvec_ok = palloc0(ex->lay->nleaves * sizeof(bool));
	ex->gofs = palloc0(ex->lay->nleaves * sizeof(uint32 *));
	ex->gpar = palloc0(ex->lay->nleaves * sizeof(uint32 *));
	/* its per-query memory is where a sub-plan runs its scans (init_expr) */
	MemoryContextSwitchTo(ex->css.ss.ps.state ? ex->css.ss.ps.state->es_query_cxt : ex->cxt);
	ex->lutcx = CreateStandaloneExprContext();
	MemoryContextSwitchTo(ex->cxt);
	ex->lutcx->ecxt_param_list_info = params;
	ex->lutcx->ecxt_param_exec_vals = ex->lutparams;
	part_prune_init(ex);
	if (ex->spec->search)
		ex->search = sift_search_open(ex->spec->search, ex->heap, snapshot, ex->lutcx, EXPR_PARENT(ex), ex->cxt);
	if (ex->spec->topn)
		ex->topn = sift_topn_open(ex->spec->topn, ex->lay, ex->src, ex->heap, snapshot, ex->lutcx, EXPR_PARENT(ex), ex->cxt, ex->widen);
	init_states(ex);
	register_preds(ex);
	for (int j = 0; j < ex->spec->nkeys; j++)
		register_key(ex, j);
	register_combos(ex);
	join_filters(ex, old);
	/* the planner takes only the joins that are (agg/order/shape.c, topn_joins_ok) */
	SIFT_INVARIANT(!ex->spec->topn || ex->njfilter == ex->spec->njoin, "sift: a join of a top-N is not a filter of the scan");
	pgagg_init(ex);
	prune_init(ex);
	filter_init(ex);
	type_init(ex);
	recheck_init(ex);
	wide_init(ex);
	numsum_init(ex);
	tid_init(ex, snapshot);
	str_init(ex);
	preagg_init(ex);
	dense_init(ex);
	gen_open(ex);
	MemoryContextSwitchTo(old);
}

void
engine_close(AggExec *ex)
{
	if (ex->search)
		sift_search_close(ex->search);
	if (ex->topn)
		sift_topn_close(ex->topn);
	if (ex->tid_vis)
	{
		sift_vis_end(ex->tid_vis);
		ExecDropSingleTupleTableSlot(ex->tid_slot);
	}
	if (ex->src)
		sift_colsrc_close(ex->src);
	if (ex->lutcx)
		FreeExprContext(ex->lutcx, true);
	if (ex->pgstate)
		FreeExprContext(ex->pgstate->curaggcontext, true);
	if (ex->index)
		index_close(ex->index, NoLock);
	if (ex->heap_opened)
		table_close(ex->heap, NoLock);
}

/* the scan's memory, and the spec in it: a scan that starts over (agg_rescan) drops both */
static void
scan_spec(AggExec *ex, MemoryContext parent)
{
	MemoryContext old;

	engine_contexts(ex, parent);
	old = MemoryContextSwitchTo(ex->cxt);
	ex->spec = sift_aggspec_unpack(ex->pack);
	MemoryContextSwitchTo(old);
}

static void
agg_begin(CustomScanState *node, EState *estate, int eflags)
{
	AggExec    *ex = (AggExec *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;

	ex->pack = cs->custom_private;
	ex->params = estate->es_param_list_info;
	ex->snap = estate->es_snapshot;
	ex->rescannable = (eflags & (EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) != 0;
	ex->param_scan = cs->custom_exprs != NIL;
	scan_spec(ex, estate->es_query_cxt);
	/* the child plans of the joins (a sub-select, VALUES list or function) are the provider's to start and end; the scan reads each once */
	foreach_ptr(Plan, child, cs->custom_plans)
		node->custom_ps = lappend(node->custom_ps, ExecInitNode(child, estate, eflags & ~(EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	engine_rels(ex);
	PredicateLockRelation(ex->heap, estate->es_snapshot);
	fuse_register(ex);
}

static TupleTableSlot *
agg_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, agg_next, agg_recheck);
}

/* what the source counted for the run, with the workers' */
void
run_counters(AggExec *c, SiftSrcCounters *cc)
{
	sift_colsrc_counters(c->src, cc);
	if (c->par)
	{
		cc->units_pruned += c->par->cc.units_pruned;
		cc->units_set_pruned += c->par->cc.units_set_pruned;
		cc->units_search_pruned += c->par->cc.units_search_pruned;
		cc->rows_vetted += c->par->cc.rows_vetted;
		cc->heap_pages += c->par->cc.heap_pages;
		cc->rechecked += c->par->cc.rechecked;
		cc->recheck_units += c->par->cc.recheck_units;
		cc->point_units += c->par->cc.point_units;
		cc->point_rows += c->par->cc.point_rows;
		cc->run_windows += c->par->cc.run_windows;
	}
}

/*
 * Folds the run into the counters of the backend (sift.stats) when it ends, from the counters the loops keep for EXPLAIN ANALYZE: the loops
 * count nothing more. A scan whose pre-aggregates did not answer it is a miss of its index, for the reason pa_why gives (or, when they were on,
 * for the units that lacked their rollup).
 */
static void
stats_run(AggExec *c)
{
	SiftSrcCounters cc;
	Oid			rel = OidIsValid(c->spec->indexoid) ? c->spec->indexoid : c->spec->heapoid;

	run_counters(c, &cc);
	sift_stat_add(SIFT_STAT_SCANS, 1);
	sift_stat_add(SIFT_STAT_WORKERS, c->par ? c->par->nlaunched : 0);
	sift_stat_add(SIFT_STAT_UNITS, c->n_units);
	sift_stat_add(SIFT_STAT_UNITS_PRUNED_ZONE, cc.units_pruned - cc.units_set_pruned - cc.units_search_pruned);
	sift_stat_add(SIFT_STAT_UNITS_PRUNED_SET, cc.units_set_pruned);
	sift_stat_add(SIFT_STAT_UNITS_PRUNED_SEARCH, cc.units_search_pruned);
	sift_stat_add(SIFT_STAT_UNITS_PREAGG, c->n_pre_units);
	sift_stat_add(SIFT_STAT_ROWS_DECODED, c->n_rows);
	sift_stat_add(SIFT_STAT_ROWS_VETTED, cc.rows_vetted);
	if (OidIsValid(c->spec->indexoid))
		sift_col_vacuum_tick(c->spec->indexoid, c->spec->heapoid, cc.rows_vetted);
	sift_stat_add(SIFT_STAT_ROWS_RECHECKED, cc.rechecked);
	sift_stat_add(SIFT_STAT_UNITS_RECHECKED, cc.recheck_units);
	sift_stat_add(SIFT_STAT_UNITS_NO_ROLLUP, c->n_rollup_miss);
	sift_stat_add(SIFT_STAT_UNITS_ROLLUP_COST, c->n_rollup_cost);
	if (c->pa_why != SPW_OFF && c->pa_why != SPW_OK)
		sift_stats_preagg_miss(rel, sift_pawhy_slug(c->pa_why), sift_pawhy_fixable(c->pa_why), 0);
	else if (c->n_rollup_miss > 0)
		sift_stats_preagg_miss(rel, "unit_without_rollup", true, c->n_rollup_miss);
}

static void
agg_end(CustomScanState *node)
{
	AggExec    *ex = (AggExec *) node;

	for (int i = 0; ex->osa && i < ex->spec->naggs; i++)
		if (ex->osa[i] && ex->spec->agg[i].share == i && ex->osa[i]->sort)
			tuplesort_end(ex->osa[i]->sort);
	gen_close(ex);
	if (ex->done)
		tuplestore_end(ex->done);
	if (ex->done_slot)
		ExecDropSingleTupleTableSlot(ex->done_slot);
	if (ex->css.ss.ps.state)
		fuse_unregister(ex);
	if (ex->fuse && --ex->fuse->refs == 0)
	{
		stats_run(ex->fuse->F);
		engine_close(ex->fuse->F);
	}
	dist_end_all(ex);
	spill_end(ex);
	if (ex->started && !ex->fuse)
		stats_run(ex);
	engine_close(ex);
	foreach_ptr(AggExec, w, ex->retries)
	{
		engine_close(w);
		MemoryContextDelete(w->cxt);
	}
	if (ex->cxt)
		MemoryContextDelete(ex->cxt);
	foreach_ptr(PlanState, child, node->custom_ps)
		ExecEndNode(child);
}

/*
 * The conditions of the scan read parameters an enclosing query level has set anew (a LATERAL reference, a correlated sub-select): the scan starts
 * over, so its zones, key lists and heap follow the operands. Its inputs are the table alone (topn_paths); the relations are opened again.
 */
static void
agg_restart(AggExec *ex)
{
	bool		rescannable = ex->rescannable;

	if (ex->started)
		stats_run(ex);
	engine_close(ex);
	MemoryContextDelete(ex->cxt);
	memset((char *) ex + sizeof(CustomScanState), 0, sizeof(AggExec) - sizeof(CustomScanState));
	ex->pack = ((CustomScan *) ex->css.ss.ps.plan)->custom_private;
	ex->params = ex->css.ss.ps.state->es_param_list_info;
	ex->snap = ex->css.ss.ps.state->es_snapshot;
	ex->rescannable = rescannable;
	ex->param_scan = true;
	scan_spec(ex, ex->css.ss.ps.state->es_query_cxt);
	engine_rels(ex);
}

static void
agg_rescan(CustomScanState *node)
{
	AggExec    *ex = (AggExec *) node;

	if (ex->param_scan)
	{
		agg_restart(ex);
		return;
	}
	/* the plan has no parameter the scan reads (unsupported_clause), so the groups are the same: replay them */
	if (spill_rescan(ex))
		return;
	ex->gpos = 0;
	if (ex->topn)
		sift_topn_rewind(ex->topn);
}

const CustomExecMethods agg_exec_methods = {
	.CustomName = "SiftAgg",
	.BeginCustomScan = agg_begin,
	.ExecCustomScan = agg_exec,
	.EndCustomScan = agg_end,
	.ReScanCustomScan = agg_rescan,
	.ExplainCustomScan = agg_explain,
};

