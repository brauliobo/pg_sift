/*
 * col/f2/maint.c - the SQL functions that read or drive the file of an index of format major 2 (col/am/format_ops.h, SiftColSqlFn): the space, the extents, the parts, the leaves, the
 * verification, the maintenance and the state of the workers. They read the metapage, the allocator and the manifest, never the directory of format major 1.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/pg_am_d.h"
#include "funcapi.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/api/format.h"
#include "col/f2/journal.h"
#include "col/f2/compact/compact.h"
#include "col/f2/f2.h"
#include "col/f2/choices.h"
#include "col/f2/vacuum/run.h"
#include "col/f2/proj/proj.h"
#include "col/f2/scope/scope.h"
#include "col/f2/tier/host.h"
#include "col/granule/int.h"
#include "table/f2/col_delta/col_delta.h"
#include "core/catname.h"
#include "core/check.h"
#include "core/cpu.h"
#include "core/errctx.h"
#include "core/f2/progress.h"
#include "core/f2/fsm/fsm.h"
#include "core/f2/meta.h"
#include "core/inspect.h"
#include "core/memtrack.h"
#include "core/meta_peek.h"
#include "core/ops.h"
#include "core/sql/sql.h"
#include "core/worker.h"

static Relation
open_f2(Oid relid)
{
	Relation	index;

	sift_inspect_expect(relid, "sift_col", NULL);
	index = index_open(relid, AccessShareLock);
	sift_check_inspect(index);
	return index;
}

/* The relation of a worker inspector, a sift_col index or a sift_table of format major 2, locked for reading: NULL while another session holds it in a lock the probe would wait for, or when it is gone */
static Relation
open_f2_worker(Oid relid)
{
	Relation	rel;

	sift_inspect_expect(relid, NULL, NULL);
	if (!ConditionalLockRelationOid(relid, AccessShareLock))
		return NULL;
	rel = try_relation_open(relid, NoLock);
	if (rel == NULL)
	{
		UnlockRelationOid(relid, AccessShareLock);
		return NULL;
	}
	sift_check_inspect(rel);
	return rel;
}

static uint64
free_blocks(const SiftFsmStats *st)
{
	uint64		n = 0;

	for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
		n += st->class_blocks[k];
	return n;
}

static uint64
free_extents(const SiftFsmStats *st)
{
	uint64		n = 0;

	for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
		n += st->class_extents[k];
	return n;
}

/* The extents of the file that something holds: the parts with their deletion vectors, the summaries of the scopes, the projection parts and the pages of the projection directory */
typedef void (*ExtentFn) (void *arg, const char *what, uint64 gen, const SiftExtent *ext, int n);

static void
each_proj_extent(Relation index, ExtentFn fn, void *arg)
{
	SiftColMetaPageData meta = sift_col_meta_read(index);
	int			nrefs,
				nb;
	SiftManifestProjRef *refs;
	BlockNumber *blk;

	if (meta.nprojs == 0)
		return;
	refs = sift_manifest_read_projs(index, &meta, &nrefs);
	blk = sift_proj_ref_dir_blocks(index, &meta, &nb);
	for (int i = 0; i < nb; i++)
		fn(arg, "proj_dir", 0, &(SiftExtent) {blk[i], 1}, 1);
	for (int i = 0; i < nrefs; i++)
	{
		SiftProjParts parts = sift_manifest_read_proj_parts(index, &refs[i]);

		blk = sift_proj_one_dir_blocks(index, &refs[i], &nb);
		for (int k = 0; k < nb; k++)
			fn(arg, "proj_dir", 0, &(SiftExtent) {blk[k], 1}, 1);
		for (int k = 0; k < parts.nparts; k++)
		{
			int			n;
			SiftExtent *ext = sift_proj_part_extents(index, &parts.parts[k], &n);

			fn(arg, "proj_part", parts.parts[k].part_gen, ext, n);
		}
	}
}

static void
each_extent(Relation index, ExtentFn fn, void *arg)
{
	SiftManifestRead mr = sift_manifest_read_range(index, NULL, NULL);

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *parts = sift_manifest_read_parts(index, &mr.entries[i], &np);

		if (mr.entries[i].l0_slab_blk != InvalidBlockNumber)
			fn(arg, "slab", 0, &(SiftExtent) {mr.entries[i].l0_slab_blk, 1}, 1);
		for (int p = 0; p < np; p++)
		{
			int			n;
			SiftExtent *ext = sift_part_blocks(sift_part_open_desc(index, &parts[p], (uint32) mr.entries[i].part_id), &n);

			fn(arg, "part", parts[p].part_gen, ext, n);
			if (parts[p].dv_blk != InvalidBlockNumber)
				fn(arg, "delvec", parts[p].part_gen, &(SiftExtent) {parts[p].dv_blk, parts[p].dv_nblocks}, 1);
		}
	}
	each_proj_extent(index, fn, arg);
	for (int s = 0; s < sift_col_rel(index)->nscope; s++)
	{
		SiftExtent *ext;
		int			n;

		if (sift_f2_scope_blocks(index, s, &ext, &n))
			fn(arg, "scope", 0, ext, n);
	}
}

typedef struct ExtentRows
{
	FunctionCallInfo fcinfo;
	ReturnSetInfo *rsi;
} ExtentRows;

static void
put_extent_row(void *arg, const char *what, uint64 gen, const SiftExtent *ext, int n)
{
	ExtentRows *er = arg;

	for (int i = 0; i < n; i++)
	{
		Datum		v[5] = {CStringGetTextDatum(what), Int64GetDatum((int64) gen), Int64GetDatum(ext[i].start), Int64GetDatum(ext[i].nblocks), BoolGetDatum(false)};
		bool		nulls[5] = {false};

		tuplestore_putvalues(er->rsi->setResult, er->rsi->setDesc, v, nulls);
	}
}

static Datum
f2_extents(FunctionCallInfo fcinfo)
{
	Relation	index = open_f2(PG_GETARG_OID(0));
	ExtentRows	er = {fcinfo, (ReturnSetInfo *) fcinfo->resultinfo};

	InitMaterializedSRF(fcinfo, 0);
	each_extent(index, put_extent_row, &er);
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

static Datum
f2_space(FunctionCallInfo fcinfo)
{
	Relation	index = open_f2(PG_GETARG_OID(0));
	SiftColMetaPageData meta = sift_col_meta_read(index);
	SiftFsmStats st = sift_fsm_check(index);
	Datum		v[12] = {0};
	bool		nulls[12] = {false};
	uint64		reusable = free_blocks(&st);

	v[0] = Int64GetDatum(RelationGetNumberOfBlocks(index));
	v[1] = Int64GetDatum(meta.high_water);
	v[2] = Int64GetDatum((int64) meta.high_water - (int64) reusable - (int64) st.quarantine_blocks);
	v[3] = Int64GetDatum(reusable);
	v[4] = Int64GetDatum(st.quarantine_blocks);
	v[5] = Int32GetDatum((int32) free_extents(&st));
	v[6] = Int64GetDatum(0);
	nulls[6] = nulls[7] = nulls[8] = nulls[10] = nulls[11] = true;
	v[9] = Int64GetDatum(meta.high_water);
	index_close(index, AccessShareLock);
	return sift_result_row(fcinfo, v, nulls);
}

static Datum
f2_shrink_history(FunctionCallInfo fcinfo)
{
	Relation	index = open_f2(PG_GETARG_OID(0));
	SiftColMetaPageData meta = sift_col_meta_read(index);
	SiftFsmStats st = sift_fsm_check(index);
	Datum		v[2] = {0};
	bool		nulls[2] = {false};

	nulls[0] = meta.shrink_blocked == 0;
	v[0] = CStringGetTextDatum(st.quarantine_blocks > 0 ? "snapshot" : "lock");
	nulls[1] = meta.last_shrink_at == 0;
	v[1] = TimestampTzGetDatum(meta.last_shrink_at);
	index_close(index, AccessShareLock);
	return sift_result_row(fcinfo, v, nulls);
}

static Datum
f2_index_info(FunctionCallInfo fcinfo)
{
	Relation	index = open_f2(PG_GETARG_OID(0));
	SiftColMetaPageData meta = sift_col_meta_read(index);
	SiftFsmStats st = sift_fsm_check(index);
	SiftManifestRead mr = sift_manifest_read_range(index, NULL, NULL);
	Datum		v[16] = {0};
	bool		nulls[16] = {false};
	int64		parts = 0,
				granules = 0,
				slabs = 0;

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(index, &mr.entries[i], &np);

		parts += np;
		slabs += mr.entries[i].l0_slab_blk != InvalidBlockNumber;
		for (int p = 0; p < np; p++)
			granules += sift_part_open_desc(index, &pd[p], (uint32) mr.entries[i].part_id)->hdr.ngranules;
	}
	v[0] = Int64GetDatum(RelationGetNumberOfBlocks(index));
	v[1] = Int64GetDatum(meta.high_water);
	v[2] = Int64GetDatum(free_blocks(&st) + st.quarantine_blocks);
	v[3] = Int64GetDatum(free_blocks(&st));
	v[4] = Int64GetDatum(free_extents(&st));
	v[5] = Int32GetDatum((int32) parts);
	nulls[6] = true;
	v[7] = Int64GetDatum((int64) (meta.total_rows - meta.total_dead));
	v[8] = Int64GetDatum((int64) meta.total_bytes);
	v[9] = Int64GetDatum(granules);
	v[10] = Int32GetDatum((int32) slabs);
	v[11] = Int32GetDatum(sift_col_rel(index)->schema->ncols);
	nulls[12] = true;
	v[13] = CStringGetTextDatum(sift_simd_level_name(sift_cpu.active));
	nulls[14] = nulls[15] = true;
	index_close(index, AccessShareLock);
	return sift_result_row(fcinfo, v, nulls);
}

static Datum
f2_segments(FunctionCallInfo fcinfo)
{
	Relation	index = open_f2(PG_GETARG_OID(0));
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftManifestRead mr = sift_manifest_read_range(index, NULL, NULL);

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(index, &mr.entries[i], &np);

		for (int p = 0; p < np; p++)
		{
			SiftPart   *part = sift_part_open_desc(index, &pd[p], (uint32) mr.entries[i].part_id);
			int			n;
			Datum		v[12] = {0};
			bool		nulls[12] = {false};

			(void) sift_part_blocks(part, &n);
			v[0] = Int64GetDatum((int64) pd[p].part_gen);
			v[1] = CStringGetTextDatum("immutable");
			v[2] = Int64GetDatum((int64) pd[p].nrows);
			v[3] = Int64GetDatum((int64) pd[p].nbytes);
			nulls[4] = nulls[5] = true;
			v[6] = Int32GetDatum((int32) part->hdr.ngranules);
			v[7] = BoolGetDatum(pd[p].ndead > 0);
			v[8] = Int64GetDatum((int64) pd[p].ndead);
			v[9] = Int32GetDatum(n);
			v[10] = Int32GetDatum(SIFT_FORMAT2_MAJOR);
			v[11] = Int64GetDatum(0);
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		}
		if (mr.entries[i].l0_slab_blk != InvalidBlockNumber)
		{
			Datum		v[12] = {0};
			bool		nulls[12] = {false};

			v[0] = Int64GetDatum(0);
			v[1] = CStringGetTextDatum("buffer");
			nulls[2] = nulls[3] = nulls[4] = nulls[5] = nulls[6] = nulls[8] = nulls[11] = true;
			v[7] = BoolGetDatum(true);
			v[9] = Int32GetDatum(1);
			v[10] = Int32GetDatum(SIFT_FORMAT2_MAJOR);
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		}
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* What each leaf holds: its chunks by the way they are coded, and the bytes of them */
typedef struct LeafCensus
{
	int64		nconst,
				nfor,
				npack,
				ncodec,
				ninline,
				bytes;
} LeafCensus;

static void
census_chunk(LeafCensus *lc, const SiftChunkRef *ref)
{
	int64	   *count[] = {&lc->nconst, &lc->nfor, &lc->npack, &lc->ninline, &lc->ncodec};

	lc->bytes += ref->len;
	(*count[sift_f2_codec_class(ref)])++;
}

static Datum
f2_leaves(FunctionCallInfo fcinfo)
{
	Relation	index = open_f2(PG_GETARG_OID(0));
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	const SiftColSchema *schema = sift_col_rel(index)->schema;
	LeafCensus *lc = palloc0(Max(schema->ncols, 1) * sizeof(LeafCensus));
	SiftManifestRead mr = sift_manifest_read_range(index, NULL, NULL);
	MemoryContext granule_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift leaves granule");

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(index, &mr.entries[i], &np);

		for (int p = 0; p < np; p++)
		{
			SiftPart   *part = sift_part_open_desc(index, &pd[p], (uint32) mr.entries[i].part_id);

			for (uint32 g = 0; g < part->hdr.ngranules; g++)
			{
				MemoryContext old = MemoryContextSwitchTo(granule_cx);
				SiftGranuleDesc gd;
				char	   *rec = sift_part_read_granule(index, part, g, &gd);
				SiftGranuleView v = sift_granule_open_part(index, part, rec, gd.data_len, &gd);

				for (int c = 0; c < Min((int) v.hdr->nchunks - 1, schema->ncols); c++)
					census_chunk(&lc[c], &v.hdr->chunks[c + 1]);
				MemoryContextSwitchTo(old);
				MemoryContextReset(granule_cx);
			}
		}
	}
	MemoryContextDelete(granule_cx);
	for (int c = 0; c < schema->ncols; c++)
	{
		const SiftColDef *d = &schema->cols[c];
		Datum		v[11] = {Int32GetDatum(c), CStringGetTextDatum(d->path != NULL ? d->path : ""), CStringGetTextDatum(d->kind == SIFT_COLK_SCALAR ? "scalar" : "group"),
		CStringGetTextDatum(format_type_be(TupleDescAttr(index->rd_att, d->attno - 1)->atttypid)), Int64GetDatum(lc[c].nconst), Int64GetDatum(lc[c].nfor), Int64GetDatum(lc[c].npack), Int64GetDatum(lc[c].ncodec),
		Int64GetDatum(lc[c].ninline), Int64GetDatum(lc[c].bytes), Int64GetDatum(0)};
		bool		nulls[11] = {false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

static void
check_manifest(void *arg)
{
	(void) sift_manifest_check(arg);
}

static void
check_col_delta(void *arg)
{
	SiftManifestRead mr = sift_manifest_read_range(arg, NULL, NULL);

	for (int i = 0; i < mr.n; i++)
		sift_col_delta_check(arg, &mr.entries[i]);
}

static Datum
f2_verify(FunctionCallInfo fcinfo)
{
	Relation	index = open_f2(PG_GETARG_OID(0));
	SiftErrCtx	ctx;

	sift_errctx_push_diagnostic(&ctx, RelationGetRelid(index));
	sift_check_maintain(index);
	(void) sift_fsm_check(index);
	check_manifest(index);
	check_col_delta(index);
	sift_tier_check_remote(index);
	sift_f2_check_blocks(index);
	ereport(NOTICE, (errmsg("sift_col index \"%s\": %s", RelationGetRelationName(index), sift_f2_check_keys(index))));
	if (PG_GETARG_BOOL(1))
		sift_f2_check_heap(index);
	sift_errctx_pop(&ctx);
	index_close(index, AccessShareLock);
	PG_RETURN_VOID();
}

static void
check_fsm(void *arg)
{
	(void) sift_fsm_check(arg);
}

static void
check_remote(void *arg)
{
	sift_tier_check_remote(arg);
}

static void
check_blocks(void *arg)
{
	sift_f2_check_blocks(arg);
}

static void
check_keys(void *arg)
{
	(void) sift_f2_check_keys(arg);
}

static void
check_heap(void *arg)
{
	sift_f2_check_heap(arg);
}

void
sift_f2_check_relation(SiftCheck *ck, Relation rel)
{
	if (sift_check_step(ck, SIFT_CHECK_WHOLE, check_fsm, rel) && sift_check_step(ck, SIFT_CHECK_WHOLE, check_manifest, rel) &&
		sift_check_step(ck, SIFT_CHECK_WHOLE, check_col_delta, rel) && sift_check_step(ck, SIFT_CHECK_WHOLE, check_remote, rel) &&
		sift_check_step(ck, SIFT_CHECK_WHOLE, check_blocks, rel))
		(void) sift_check_step(ck, SIFT_CHECK_WHOLE, check_keys, rel);
}

static void
check_journal(void *arg)
{
	sift_f2_journal_check(arg);
}

void
sift_f2_check(SiftCheck *ck, Oid indexoid, bool heapallindexed)
{
	Relation	index = open_f2(indexoid);
	SiftErrCtx	ctx;

	sift_errctx_push_diagnostic(&ctx, indexoid);
	sift_f2_check_relation(ck, index);
	(void) sift_check_step(ck, SIFT_CHECK_WHOLE, check_journal, index);
	if (heapallindexed)
		(void) sift_check_step(ck, SIFT_CHECK_WHOLE, check_heap, index);
	sift_errctx_pop(&ctx);
	index_close(index, AccessShareLock);
}

/* The steps a VACUUM takes on the index (the slab, the compaction, the summaries, the end of the file), without the heap's */
void
sift_f2_maintain(Relation index)
{
	IndexVacuumInfo info = {.index = index};
	SiftF2Trace tr;

	LockRelationOid(index->rd_index->indrelid, ShareUpdateExclusiveLock);
	sift_f2_trace_begin(&tr, index, SIFT_JOB_NONE, true);
	(void) sift_f2_vacuum_run(&info, NULL, true);
	sift_f2_say_maintain(&tr);
}

static Datum
f2_maintain(FunctionCallInfo fcinfo)
{
	Relation	index;

	PreventCommandIfReadOnly("sift.col_maintain()");
	PreventCommandDuringRecovery("sift.col_maintain()");
	index = open_f2(PG_GETARG_OID(0));
	sift_check_maintain(index);
	sift_f2_maintain(index);
	index_close(index, AccessShareLock);
	PG_RETURN_BOOL(true);
}

static Datum
f2_maintenance_due(FunctionCallInfo fcinfo)
{
	Relation	index = open_f2(PG_GETARG_OID(0));
	SiftF2Rel  *f = sift_f2_rel(index);
	SiftManifestRead mr = sift_manifest_read_range(index, NULL, NULL);
	int			groups = 0;

	for (int i = 0; i < mr.n; i++)
	{
		int			np,
					n;
		SiftPartDesc *pd = sift_manifest_read_parts(index, &mr.entries[i], &np);

		(void) sift_compact_plan(pd, np, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 4, 1), sift_f2_hot_cut(f), &n);
		groups += n;
	}
	index_close(index, AccessShareLock);
	if (groups == 0)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%d groups of parts to compact", groups)));
}

static Datum
f2_maintenance_status(FunctionCallInfo fcinfo)
{
	Relation	index = open_f2(PG_GETARG_OID(0));
	Oid			heapoid = index->rd_index->indrelid;
	SiftColMetaPageData meta = sift_col_meta_read(index);
	const PgStat_StatTabEntry *tab = pgstat_fetch_stat_tabentry(heapoid);
	static const char *const reasons[] = {"sift.async_maintenance is off", "standby", "temporary table"};
	const bool	blocked_by[lengthof(reasons)] = {!sift_async_maintenance, RecoveryInProgress(),
												 index->rd_rel->relpersistence == RELPERSISTENCE_TEMP};
	Datum		v[7] = {0};
	Datum		blocked[lengthof(reasons)];
	bool		nulls[7] = {false};
	int			nblocked = 0;

	for (int i = 0; i < lengthof(reasons); i++)
		if (blocked_by[i])
			blocked[nblocked++] = CStringGetTextDatum(reasons[i]);
	v[0] = ObjectIdGetDatum(heapoid);
	nulls[1] = nulls[2] = nulls[3] = tab == NULL;
	if (tab != NULL)
	{
		v[1] = TimestampTzGetDatum(Max(tab->last_vacuum_time, tab->last_autovacuum_time));
		nulls[1] = tab->last_vacuum_time == 0 && tab->last_autovacuum_time == 0;
		v[2] = Int64GetDatum(Max(tab->dead_tuples, 0) + Max(tab->ins_since_vacuum, 0));
		v[3] = Int64GetDatum(autovacuum_vac_thresh + (int64) (autovacuum_vac_scale * Max(tab->live_tuples, 0)));
	}
	nulls[4] = true;
	nulls[5] = meta.maint_requested_at == 0;
	v[5] = TimestampTzGetDatum(meta.maint_requested_at);
	v[6] = PointerGetDatum(construct_array(blocked, nblocked, TEXTOID, -1, false, TYPALIGN_INT));
	index_close(index, AccessShareLock);
	return sift_result_row(fcinfo, v, nulls);
}

static Datum
f2_tracking(FunctionCallInfo fcinfo)
{
	Relation	index = open_f2(PG_GETARG_OID(0));
	bool		triggers,
				heard;
	Datum		v[4];
	bool		nulls[4] = {false};

	sift_f2_track_state(index, &triggers, &heard);
	v[0] = BoolGetDatum(sift_col_track_on(index));
	v[1] = BoolGetDatum(triggers);
	v[2] = v[3] = BoolGetDatum(heard);

	index_close(index, AccessShareLock);
	return sift_result_row(fcinfo, v, nulls);
}

/* An index of format major 2 has no directory of segments to seal or promote: what waits is its slabs */
static Datum
f2_worker_state(FunctionCallInfo fcinfo)
{
	Relation	index = open_f2_worker(PG_GETARG_OID(0));
	SiftColMetaPageData meta;
	SiftManifestRead mr;
	Datum		v[8] = {0};
	bool		nulls[8] = {false};
	int			parts = 0,
				slabs = 0;

	if (index == NULL)
		PG_RETURN_NULL();
	meta = sift_col_meta_read(index);
	mr = sift_manifest_read_range(index, NULL, NULL);
	for (int i = 0; i < mr.n; i++)
	{
		int			np;

		(void) sift_manifest_read_parts(index, &mr.entries[i], &np);
		parts += np;
		slabs += mr.entries[i].l0_slab_blk != InvalidBlockNumber;
	}
	v[0] = Int32GetDatum(parts);
	v[1] = Int32GetDatum(slabs);
	v[2] = BoolGetDatum(false);
	v[3] = Int32GetDatum((int32) meta.failures_total);
	nulls[4] = nulls[5] = nulls[6] = nulls[7] = true;
	relation_close(index, AccessShareLock);
	return sift_result_row(fcinfo, v, nulls);
}

/* sift.col_refresh: an index of format major 2 keeps no pre-aggregate sidecar of a segment that its options could make stale, so there is nothing to rewrite */
static Datum
f2_refresh(FunctionCallInfo fcinfo)
{
	Relation	index = open_f2(PG_GETARG_OID(0));
	Datum		v[2] = {Int32GetDatum(0), Int32GetDatum(0)};
	bool		nulls[2] = {false};

	sift_check_maintain(index);
	index_close(index, AccessShareLock);
	return sift_result_row(fcinfo, v, nulls);
}

/* The last background step that ended well and the failures noted since the build, from the metapage */
Datum
sift_f2_worker_history(FunctionCallInfo fcinfo)
{
	Relation	index = open_f2_worker(PG_GETARG_OID(0));
	SiftColMetaPageData meta;
	Datum		v[4] = {0};
	bool		nulls[4] = {false};
	const char *job;

	if (index == NULL)
		PG_RETURN_NULL();
	meta = sift_col_meta_read(index);
	job = sift_job_name(meta.maint_ok_job);

	nulls[0] = nulls[1] = nulls[2] = meta.maint_ok_at == 0;
	if (!nulls[0])
	{
		v[0] = TimestampTzGetDatum(meta.maint_ok_at);
		v[1] = CStringGetTextDatum(job ? job : "");
		v[2] = Int64GetDatum(meta.maint_ok_ms);
	}
	v[3] = Int64GetDatum(meta.failures_total);
	relation_close(index, AccessShareLock);
	return sift_result_row(fcinfo, v, nulls);
}

Datum
sift_col_v2_sql(SiftColSqlFn fn, FunctionCallInfo fcinfo)
{
	switch (fn)
	{
		case SIFT_SQL_SPACE:
			return f2_space(fcinfo);
		case SIFT_SQL_EXTENTS:
			return f2_extents(fcinfo);
		case SIFT_SQL_SHRINK_HISTORY:
			return f2_shrink_history(fcinfo);
		case SIFT_SQL_INDEX_INFO:
			return f2_index_info(fcinfo);
		case SIFT_SQL_SEGMENTS:
			return f2_segments(fcinfo);
		case SIFT_SQL_LEAVES:
			return f2_leaves(fcinfo);
		case SIFT_SQL_SECTIONS:
			return sift_f2_sections(fcinfo);
		case SIFT_SQL_VERIFY:
			return f2_verify(fcinfo);
		case SIFT_SQL_MAINTAIN:
			return f2_maintain(fcinfo);
		case SIFT_SQL_MAINTENANCE_DUE:
			return f2_maintenance_due(fcinfo);
		case SIFT_SQL_MAINTENANCE_STATUS:
			return f2_maintenance_status(fcinfo);
		case SIFT_SQL_TRACKING:
			return f2_tracking(fcinfo);
		case SIFT_SQL_WORKER_STATE:
			return f2_worker_state(fcinfo);
		case SIFT_SQL_WORKER_HISTORY:
			return sift_f2_worker_history(fcinfo);
		case SIFT_SQL_REFRESH:
			return f2_refresh(fcinfo);
	}
	pg_unreachable();
}
