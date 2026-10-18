/*
 * sift_col_maint.c - promotion, merge, VACUUM, the maintenance worker and the SQL functions of sift_col indexes (docs/COL.md 2.6, 2.7).
 * The lock, the directory switch and the sweep are those of sift_maint.c; what is written is a column segment.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "sift_col_am.h"
#include "sift_col_seg.h"
#include "sift_maint.h"
#include "sift_merge.h"
#include "sift_ops.h"

#define ROOM_SEGMENTS	(SIFT_MAX_SEGMENTS - 16)	/* directory entries from which inserters merge */

#define corrupt(index, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift index \"%s\" is corrupted", RelationGetRelationName(index)), \
			 errdetail(__VA_ARGS__)))

/* ---------- promotion ---------- */

static void
promote_one(Relation index, const SiftSegEntry *sealed)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col promote", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	const SiftColSchema *schema = sift_col_rel(index)->schema;
	SiftColBatch **batches;
	int			nb;
	SiftWriteTarget *wt = NULL;
	SiftColSegWriter *w = NULL;
	SiftSegEntry neu;
	SiftExtent *pages;
	int			npages;
	SiftMaintSwitch sw = {.old = &sealed->gen,.nold = 1};
	TimestampTz start = GetCurrentTimestamp();
	uint64		rows = 0;

	sift_cmut_flag_sealed(index, sealed);
	batches = sift_cmut_batches(index, sealed, schema, cx, &nb);
	if (nb > 0)
	{
		wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
		w = sift_colw_begin(wt, sift_store_new_gen(index), schema, sift_col_rel(index)->policy, cx);
		for (int i = 0; i < nb; i++)
			sift_colw_add(w, batches[i]);
		rows = sift_colw_rows(w);
		sift_colw_finish(w, &neu);
		sift_wt_finish(wt);
		sw.neu = &neu;
	}
	pages = sift_cmut_pages(index, sealed, &npages);
	sift_dir_update(index, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw,.release = pages,
			.nrelease = npages,.extra = InvalidBuffer});
	sift_log_maint(index, "promoted a sealed column segment of", start, rows, sw.neu ? neu.bytes : 0);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

static const SiftSegEntry *
first_sealed(const SiftDirSnapshot *snap)
{
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_MUTABLE_SEALED)
			return &snap->segs[i];
	return NULL;
}

static bool merge_locked(Relation index, bool force);
static bool make_room_locked(Relation index);

static int
promote_locked(Relation index, bool seal_active)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	const SiftSegEntry *s;
	int			n = 0;

	if (seal_active)
	{
		make_room_locked(index);
		sift_cmut_seal(index, true);
	}
	for (;;)
	{
		sift_dir_read(index, snap);
		if ((s = first_sealed(snap)) == NULL)
			break;
		promote_one(index, s);
		n++;
		CHECK_FOR_INTERRUPTS();
	}
	pfree(snap);
	return n;
}

int
sift_col_maint_promote(Relation index, bool wait)
{
	int			n;

	if (!sift_maint_lock(index, wait))
		return 0;
	n = promote_locked(index, false);
	sift_maint_unlock(index);
	return n;
}

/* ---------- merge ---------- */

bool
sift_col_mostly_dead(Relation index, const SiftSegEntry *e)
{
	SiftColSegment *seg;
	bool		dead;

	if (!(e->flags & SIFT_SEG_DIRTY))
		return false;
	seg = sift_cseg_open(index, e, CurrentMemoryContext);
	dead = sift_cseg_live_rows(seg) * 2 < e->ndocs;
	sift_cseg_close(seg);
	return dead;
}

static int
pick_merge(Relation index, const SiftDirSnapshot *snap, bool force, SiftSegEntry *sel)
{
	SiftMergePolicy pol = {.tier_base = (uint64) sift_col_options(index)->mutable_limit * 1024, .fanin = SIFT_MERGE_FANIN, .mostly_dead = sift_col_mostly_dead};

	return sift_merge_pick(index, &pol, snap, force, sel);
}

void
sift_col_merge_segments(Relation index, const SiftColSchema *schema, const SiftCcPolicy *policy, const SiftSegEntry *in, int n)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col merge", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColSegment **seg = palloc(n * sizeof(SiftColSegment *));
	SiftColBatch **part = palloc(n * sizeof(SiftColBatch *));
	SiftColBatch **use = palloc(n * sizeof(SiftColBatch *));
	uint32	   *cur = palloc0(n * sizeof(uint32));
	uint64	   *gens = palloc(n * sizeof(uint64));
	SiftColBatch *out = sift_cbatch_create(cx, schema);
	SiftExtent *rel = palloc(0);
	int			nrel = 0;
	SiftWriteTarget *wt = NULL;
	SiftColSegWriter *w = NULL;
	SiftSegEntry neu;
	SiftMaintSwitch sw = {.old = gens,.nold = n};
	TimestampTz start = GetCurrentTimestamp();
	uint64		expect = 0;

	for (int i = 0; i < n; i++)
	{
		int			k;
		SiftExtent *ext = sift_store_immutable_extents(index, &in[i], &k);

		expect += in[i].bytes;

		seg[i] = sift_cseg_open(index, &in[i], cx);
		if (seg[i]->schema->hash != schema->hash)
			corrupt(index, "Segment %llu was written for another column layout.", (unsigned long long) in[i].gen);
		part[i] = sift_cbatch_create(cx, schema);
		gens[i] = in[i].gen;
		rel = repalloc(rel, (nrel + k) * sizeof(SiftExtent));
		memcpy(rel + nrel, ext, k * sizeof(SiftExtent));
		nrel += k;
	}
	for (;;)
	{
		uint32		sb = PG_UINT32_MAX;
		int			nuse = 0;

		for (int i = 0; i < n; i++)
			if (cur[i] < seg[i]->nsb)
				sb = Min(sb, seg[i]->sbdir[cur[i]].sb);
		if (sb == PG_UINT32_MAX)
			break;
		for (int i = 0; i < n; i++)
		{
			if (cur[i] >= seg[i]->nsb || seg[i]->sbdir[cur[i]].sb != sb)
				continue;
			sift_cseg_decode_unit(seg[i], cur[i]++, part[i]);
			if (part[i]->nrows > 0)
				use[nuse++] = part[i];
		}
		if (nuse == 0)
			continue;
		sift_cbatch_reset(out, sb);
		sift_cbatch_merge(out, use, nuse);
		if (w == NULL)
		{
			wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
			sift_wt_expect(wt, expect);
			w = sift_colw_begin(wt, sift_store_new_gen(index), schema, policy, cx);
		}
		sift_colw_add(w, out);
		CHECK_FOR_INTERRUPTS();
	}
	for (int i = 0; i < n; i++)
		sift_cseg_close(seg[i]);
	if (w)
	{
		sift_colw_finish(w, &neu);
		sift_wt_finish(wt);
		sw.neu = &neu;
	}
	sift_dir_update(index, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw,.release = rel,
			.nrelease = nrel,.extra = InvalidBuffer});
	sift_log_maint(index, psprintf("merged %d column segments of", n), start, sw.neu ? neu.ndocs : 0, sw.neu ? neu.bytes : 0);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

static bool
merge_locked(Relation index, bool force)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	SiftSegEntry sel[SIFT_MAX_SEGMENTS];
	int			n;

	sift_dir_read(index, snap);
	n = pick_merge(index, snap, force, sel);
	pfree(snap);
	if (n > 0)
		sift_col_merge_segments(index, sift_col_rel(index)->schema, sift_col_rel(index)->policy, sel, n);
	return n > 0;
}

/* Merges until the directory is below ROOM_SEGMENTS entries, so a seal always finds a free one; false when nothing can be merged any more */
static bool
make_room_locked(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	bool		room;

	for (;;)
	{
		int			promoted;

		sift_dir_read(index, snap);
		if (snap->nsegs < ROOM_SEGMENTS)
			break;
		promoted = promote_locked(index, false);
		if (!merge_locked(index, false) && promoted == 0)
			break;
		CHECK_FOR_INTERRUPTS();
	}
	room = snap->nsegs < ROOM_SEGMENTS;
	pfree(snap);
	return room;
}

bool
sift_col_maint_make_room(Relation index)
{
	bool		room;
	TimestampTz start = GetCurrentTimestamp();

	sift_maint_lock(index, true);
	room = make_room_locked(index);
	sift_maint_unlock(index);
	sift_log_stall(index, start);
	return room;
}

bool
sift_col_maint_merge_step(Relation index, bool wait)
{
	bool		merged;

	if (!sift_maint_lock(index, wait))
		return false;
	merged = merge_locked(index, false);
	sift_maint_unlock(index);
	return merged;
}

/* ---------- VACUUM ---------- */

IndexBulkDeleteResult *
sift_col_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state)
{
	Relation	index = info->index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	const SiftColSchema *schema = sift_col_rel(index)->schema;

	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));
	sift_maint_lock(index, true);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);

			sift_cseg_bulkdelete(seg, callback, callback_state, stats);
			sift_cseg_close(seg);
		}
		else
			sift_cmut_bulkdelete(index, &snap->segs[i], schema, callback, callback_state, stats);
	}
	sift_maint_unlock(index);
	pfree(snap);
	return stats;
}

IndexBulkDeleteResult *
sift_col_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Relation	index = info->index;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	if (info->analyze_only)
		return stats;
	if (stats == NULL)
		stats = palloc0(sizeof(IndexBulkDeleteResult));
	sift_maint_lock(index, true);
	promote_locked(index, false);
	merge_locked(index, false);
	sift_recycle(index);
	sift_maint_orphan_sweep(index);
	sift_maint_unlock(index);
	sift_dir_read(index, snap);
	stats->num_pages = RelationGetNumberOfBlocks(index);
	stats->estimated_count = false;
	if (stats->num_index_tuples == 0)
		for (int i = 0; i < snap->nsegs; i++)
			stats->num_index_tuples += snap->segs[i].ndocs;
	pfree(snap);
	return stats;
}

/* ---------- asynchronous maintenance ---------- */

/* Promotion is asynchronous when allowed; the back-pressure rules are those of sift_maint_after_seal (DESIGN.md (c)) */
void
sift_col_maint_after_seal(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	int			nsealed = 0;

	sift_dir_read(index, snap);
	if (snap->nsegs >= ROOM_SEGMENTS)
		sift_col_maint_make_room(index);
	if (sift_cmut_seal(index, false) == 0)
		return;
	if (sift_async_maintenance)
		sift_maint_request_worker(index, "sift_col_maint_worker_main");
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		nsealed += snap->segs[i].kind == SIFT_SEG_MUTABLE_SEALED;
	pfree(snap);
	if (nsealed >= 4)
		sift_col_maint_promote(index, false);
}

void
sift_col_maint_worker_main(Datum main_arg)
{
	Oid			indexoid = DatumGetObjectId(main_arg);
	Oid			dboid;
	Relation	index;
	ErrorContextCallback errctx;
	TimestampTz start = GetCurrentTimestamp();

	sift_maint_context(&errctx, indexoid);
	memcpy(&dboid, MyBgworkerEntry->bgw_extra, sizeof(Oid));
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, 0);
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	index = try_index_open(indexoid, RowExclusiveLock);
	if (index != NULL && sift_col_is_index(index))
	{
		/* the lock is busy when another worker or a VACUUM is at it, which then does the work */
		while (sift_col_maint_promote(index, false) > 0 || sift_col_maint_merge_step(index, false))
			CHECK_FOR_INTERRUPTS();
		sift_log_maint(index, "column maintenance worker finished", start, -1, -1);
		index_close(index, RowExclusiveLock);
	}
	else if (index != NULL)
		index_close(index, RowExclusiveLock);
	PopActiveSnapshot();
	CommitTransactionCommand();
}

/* ---------- SQL ---------- */

static Relation
open_col(Oid indexoid, LOCKMODE lock)
{
	Relation	index = index_open(indexoid, lock);

	if (!sift_col_is_index(index))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sift_col index", RelationGetRelationName(index))));
	return index;
}

static Relation
open_col_maintain(Oid indexoid)
{
	Relation	index = open_col(indexoid, RowExclusiveLock);

	sift_check_maintain(index);
	return index;
}

Datum
sift_col_promote(PG_FUNCTION_ARGS)
{
	Relation	index = open_col_maintain(PG_GETARG_OID(0));
	int			n;

	sift_maint_lock(index, true);
	n = promote_locked(index, true);
	sift_maint_unlock(index);
	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}

Datum
sift_col_merge(PG_FUNCTION_ARGS)
{
	Relation	index = open_col_maintain(PG_GETARG_OID(0));
	bool		force = PG_NARGS() > 1 && PG_GETARG_BOOL(1);
	int			n = 0;

	sift_maint_lock(index, true);
	promote_locked(index, false);
	while (merge_locked(index, force && n == 0))
	{
		n++;
		CHECK_FOR_INTERRUPTS();
	}
	sift_maint_unlock(index);
	index_close(index, RowExclusiveLock);
	PG_RETURN_INT32(n);
}

Datum
sift_col_index_info(PG_FUNCTION_ARGS)
{
	Relation	index = open_col(PG_GETARG_OID(0), AccessShareLock);
	const SiftColIndexMeta *defs = sift_col_index_defs(index);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TupleDesc	tupdesc;
	SiftFreeStats fs;
	Datum		v[14];
	bool		nulls[14] = {0};
	int64		rows = 0,
				bytes = 0,
				units = 0;
	int			nmut = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	sift_dir_read(index, snap);
	fs = sift_store_free_stats(index);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			rows += snap->segs[i].ndocs;
			bytes += snap->segs[i].bytes;
			units += snap->segs[i].sb_hi - snap->segs[i].sb_lo + 1;
		}
		else
			nmut++;
	v[0] = Int64GetDatum(RelationGetNumberOfBlocks(index));
	v[1] = Int64GetDatum(snap->meta.high_water);
	v[2] = Int64GetDatum(fs.blocks);
	v[3] = Int64GetDatum(fs.reusable);
	v[4] = Int64GetDatum(fs.records);
	v[5] = Int32GetDatum(snap->nsegs);
	v[6] = Int32GetDatum(SIFT_MAX_SEGMENTS);
	v[7] = Int64GetDatum(rows);
	v[8] = Int64GetDatum(bytes);
	v[9] = Int64GetDatum(units);
	v[10] = Int32GetDatum(nmut);
	v[11] = Int32GetDatum(defs->ncols);
	nulls[12] = RecoveryInProgress();
	v[12] = BoolGetDatum(!nulls[12] && sift_maintenance_busy(index));
	v[13] = CStringGetTextDatum(sift_simd_level_name(sift_cpu.active));
	index_close(index, AccessShareLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

Datum
sift_col_segments(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = open_col(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];
		Datum		v[10];
		bool		nulls[10] = {0};

		v[0] = Int64GetDatum(e->gen);
		v[1] = CStringGetTextDatum(e->kind == SIFT_SEG_IMMUTABLE ? "immutable" :
								   e->kind == SIFT_SEG_MUTABLE_ACTIVE ? "mutable" : "sealed");
		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, e, CurrentMemoryContext);

			v[2] = Int64GetDatum(e->ndocs);
			v[3] = Int64GetDatum(e->bytes);
			v[4] = Int32GetDatum(e->sb_lo);
			v[5] = Int32GetDatum(e->sb_hi);
			v[6] = Int32GetDatum(seg->nsb);
			v[7] = BoolGetDatum((e->flags & SIFT_SEG_DIRTY) != 0);
			v[8] = Int64GetDatum(e->ndocs - sift_cseg_live_rows(seg));
			v[9] = Int32GetDatum(seg->hdr->nextents);
			sift_cseg_close(seg);
		}
		else
		{
			uint64		nrows,
						npages;

			sift_cmut_summary(index, e, &nrows, &npages);
			v[2] = Int64GetDatum(nrows);
			v[3] = Int64GetDatum(npages * BLCKSZ);
			nulls[4] = nulls[5] = nulls[6] = nulls[7] = nulls[8] = nulls[9] = true;
		}
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* One row per leaf: the chunks of all immutable segments by encoding, their bytes, and the dictionaries */
Datum
sift_col_leaves(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = open_col(PG_GETARG_OID(0), AccessShareLock);
	const SiftColSchema *sc = sift_col_rel(index)->schema;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	int64	   *chunks = palloc0(sc->ncols * 4 * sizeof(int64)),
			   *bytes = palloc0(sc->ncols * sizeof(int64)),
			   *dict_n = palloc0(sc->ncols * sizeof(int64)),
			   *dict_bytes = palloc0(sc->ncols * sizeof(int64));

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			SiftCUnitView v;
			uint32		len;
			const uint8 *rec = sift_cseg_unit(seg, u, &len);

			if (!sift_cunit_parse(rec, len, sc, &v))
				corrupt(index, "Unit %u of segment %llu does not parse.", u, (unsigned long long) snap->segs[i].gen);
			for (int c = 0; c < sc->ncols; c++)
			{
				SiftCChunk	ch;

				if (!sift_cunit_chunk(&v, c, &ch))
					continue;
				chunks[c * 4 + Min(ch.enc, 3)]++;
				bytes[c] += v.chunk_len[c];
			}
		}
		for (int c = 0; c < sc->ncols; c++)
		{
			SiftCDict  *d = sift_cseg_dict(seg, c);

			if (d)
			{
				dict_n[c] += d->n;
				dict_bytes[c] += d->nbytes + (int64) (d->n + 1) * sizeof(uint32);
			}
		}
		sift_cseg_close(seg);
	}
	for (int c = 0; c < sc->ncols; c++)
	{
		Datum		v[10];
		bool		nulls[10] = {false};

		v[0] = Int32GetDatum(c);
		v[1] = CStringGetTextDatum(sc->cols[c].path[0] ? sc->cols[c].path : NameStr(TupleDescAttr(RelationGetDescr(index), sc->cols[c].attno - 1)->attname));
		v[2] = CStringGetTextDatum(sc->cols[c].kind == SIFT_COLK_GROUP ? "group" : "scalar");
		v[3] = CStringGetTextDatum(format_type_be(sc->cols[c].typid));
		for (int e = 0; e < 4; e++)
			v[4 + e] = Int64GetDatum(chunks[c * 4 + e]);
		v[8] = Int64GetDatum(bytes[c]);
		v[9] = Int64GetDatum(dict_bytes[c]);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}
