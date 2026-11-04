/*
 * table/load/load.c - the rows of a bulk load (COPY, CREATE TABLE AS, REFRESH MATERIALIZED VIEW) written straight into column groups
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/table.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "table/load/load_int.h"

bool		sift_tbl_bulk_groups = true;
bool		sift_tbl_load_from_copy = false;
#ifdef SIFT_TESTING
int			sift_test_load_publish = 0;
bool		sift_test_load_reap_late = false;
int			sift_test_load_unit_bytes = 0;
int			sift_test_load_segment_kb = 0;
#endif
static List *loads = NIL;
static bool callbacks_registered = false;

static Load *
find_load(Oid relid)
{
	foreach_ptr(Load, ld, loads)
		if (ld->relid == relid)
			return ld;
	return NULL;
}

static void
drop_load(Load *ld)
{
	loads = list_delete_ptr(loads, ld);
	MemoryContextDelete(ld->cxt);
}

static void
reserve_vblocks(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	SIFT_TBL_NEXT_VBLOCK(meta) = *(uint64 *) arg;
}

/* The groups a run holds: a parallel context writes one for each participant (the rows of a run wait in the backend: they fit maintenance_work_mem), the backend alone one */
static int
load_groups(Relation rel)
{
	if (!IsUnderPostmaster || IsInParallelMode() || !ActiveSnapshotSet() || RelationUsesLocalBuffers(rel))
		return 1;
	return Max(1, Min(Min(1 + max_parallel_maintenance_workers, LOAD_MAX_GROUPS), maintenance_work_mem / (LOAD_SBS * sift_tbl_write_buffer_limit)));
}

/* Does the load of this table hand its groups to workers that stay for the command? */
static bool
ring_wanted(Load *ld, Relation rel)
{
	return ld->ring_ok && !ld->noring && load_groups(rel) > 1 && ring_room(sift_tbl_rel(rel), 0);
}

/* The next run of superblocks, taken under the lock that serializes the write buffers' own reservations */
static void
reserve_run(Load *ld, Relation rel, SiftTblRel *st)
{
	uint64		end;

	ld->maxunits = ld->rl || ring_wanted(ld, rel) ? LOAD_RING_UNITS : LOAD_SBS * load_groups(rel);
	if (ld->rl)
		load_ring_take_slot(ld, rel);
	LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(rel, &st->dir);
	ld->run.first = TYPEALIGN(SIFT_TBL_VBLOCKS_SB, Max(SIFT_TBL_NEXT_VBLOCK(&st->dir.meta), (uint64) sift_tbl_vblock_floor));
	end = ld->run.first + (uint64) ld->maxunits * SIFT_TBL_VBLOCKS_SB;
	if (end > (uint64) MaxBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift_table \"%s\" exhausted its TIDs", RelationGetRelationName(rel)),
				 errhint("Run VACUUM FULL on the table.")));
	sift_dir_update(rel, &(SiftDirChange) {.fn = reserve_vblocks,.arg = &end,.extra = InvalidBuffer});
	UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	ld->open = true;
	ld->run.units = 0;
	ld->run.uoff[0] = 0;
	ld->n = 0;
	ld->bytes = 0;
	if (!ld->rl)
		resetStringInfo(&ld->rows);
}
/* The unit of the current superblock is full: the rows go into the next one */
void
load_end_unit(Load *ld)
{
	LoadRun    *r = &ld->run;

	r->urows[r->units] = ld->n;
	r->uoff[++r->units] = ld->rows.len;
	ld->n = 0;
	ld->bytes = 0;
}
/* The groups of the run are written and become segments in the directory. parallel: the command is still running, so a parallel context may be used */
static void
end_run(Load *ld, Relation rel, SiftTblRel *st, bool parallel, bool more)
{
	LoadRun    *r = &ld->run;
	int			participants = parallel ? load_groups(rel) : 1;
	int			ngroups;
	int			room;
	SiftSegEntry segs[LOAD_MAX_GROUPS];
	Published	pub = {segs};

	if (ld->rl)
	{
		load_ring_hand_over(ld, rel, st, false);
		return;
	}
	ld->open = false;
	if (r->units == 0)
		return;
	if (more && parallel && ring_wanted(ld, rel) && participants > 1 && r->units == LOAD_RING_UNITS && ld->rows.len >= load_parallel_bytes() * LOAD_RING_UNITS / LOAD_SBS)
	{
		ld->open = true;
		if (load_ring_start(ld, rel, st))
			return;
		ld->open = false;
		ld->noring = true;
	}
	pub.n = ngroups = load_share_units(r, ld->rows.len, participants);
	room = load_lock_with_room(rel, st, ngroups);
	r->xid = r->vers ? GetCurrentTransactionId() : InvalidTransactionId;
	if (room >= ngroups)
	{
		if (!(participants > 1 && ngroups > 1 && load_write_parallel(r, ld->rows.data, rel, ngroups, segs)))
			for (int g = 0; g < ngroups; g++)
				load_write_group(r, ld->rows.data, rel, g, &segs[g]);
		sift_dir_update(rel, &(SiftDirChange) {.fn = load_publish_groups,.arg = &pub,.extra = InvalidBuffer});
		UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	}
	else
	{
		/* less room than groups: each is written and published under its own hold of the lock, the way a load without workers goes */
		for (int g = 0; g < ngroups; g++)
		{
			Published	one = {&segs[g], 1};

			if (g > 0)
				(void) load_lock_with_room(rel, st, 1);
			load_write_group(r, ld->rows.data, rel, g, &segs[g]);
			sift_dir_update(rel, &(SiftDirChange) {.fn = load_publish_groups,.arg = &one,.extra = InvalidBuffer});
			UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		}
	}
	sift_tbl_refresh(rel, st);
	if (sift_tbl_inline_promote)
		(void) sift_tbl_settle(rel, st, GetOldestNonRemovableTransactionId(rel), false, SIFT_TBL_SETTLE_STEPS);
}

/* The segment the writer holds is closed: its blocks are in no directory until load_publish_all */
static void
stream_end_segment(Load *ld)
{
	MemoryContext old = MemoryContextSwitchTo(ld->sw.cxt);

	if (ld->nsegs == ld->maxsegs)
		ld->segs = repalloc(ld->segs, (ld->maxsegs *= 2) * sizeof(SiftSegEntry));
	sift_colw_finish(ld->sw.w, &ld->segs[ld->nsegs++]);
	sift_wt_finish(ld->sw.wt);
	ld->sw.w = NULL;
	MemoryContextSwitchTo(old);
}

/* The current unit is full (or the load ends): it is encoded into the open segment, which ends at the memory budget or when the next unit is not the superblock after it */
static void
stream_end_unit(Load *ld)
{
	seal_flush(&ld->sw);
	ld->run.units++;
	ld->n = 0;
	ld->bytes = 0;
	ld->seg_next = ld->run.first + (uint64) ld->run.units * SIFT_TBL_VBLOCKS_SB;
	if (ld->run.units == ld->maxunits)
		ld->open = false;
	if (ld->sw.w && sift_colw_memory(ld->sw.w) > ld->budget)
		stream_end_segment(ld);
}

static void
stream_row(Load *ld, Relation rel, SiftTblRel *st, const Datum *values, const bool *isnull, CommandId cid, ItemPointer tid)
{
	MemoryContext old;
	SiftColRow	row;

	if (ld->open && (ld->n >= LOAD_SB_ROWS || ld->bytes >= load_unit_bytes()))
		stream_end_unit(ld);
	if (!ld->open)
	{
		reserve_run(ld, rel, st);
		if (ld->sw.w && ld->run.first != ld->seg_next)
			stream_end_segment(ld);
	}
	ItemPointerSet(tid, ld->run.first + (uint64) ld->run.units * SIFT_TBL_VBLOCKS_SB + ld->n / SIFT_TBL_MAX_OFF, ld->n % SIFT_TBL_MAX_OFF + 1);
	ld->sw.rel = rel;
	if (ld->n == 0)
		sift_cbatch_reset(ld->sw.batch, ItemPointerGetBlockNumber(tid) / SIFT_SB_PAGES);
	old = MemoryContextSwitchTo(ld->rowcx);
	row_in(ld->sw.l, ld->desc, tid, values, isnull, &row);
	if (ld->run.vers)
		sift_cbatch_add_ver(ld->sw.batch, &row, GetCurrentTransactionId(), cid);
	else
		sift_cbatch_add(ld->sw.batch, &row);
	MemoryContextSwitchTo(old);
	MemoryContextReset(ld->rowcx);
}

/* The rows of the open unit and the segment are written, and the segments published (they are all in the directory when the lock that kept the orphan sweep from them goes) */
static void
stream_flush(Load *ld, Relation rel, SiftTblRel *st)
{
	Published	pub;

	ld->sw.rel = rel;
	if (ld->n > 0)
		stream_end_unit(ld);
	if (ld->sw.w)
		stream_end_segment(ld);
	ld->open = false;
	pub = (Published) {ld->segs, ld->nsegs};
	if (pub.n > 0)
		load_publish_all(rel, st, &pub);
	ld->nsegs = 0;
	UnlockPage(rel, SIFT_LOAD_LOCK_BLKNO, ShareLock);
}

/* Writes what the load holds: the rows of the current unit, and the run they end */
static void
flush_load(Load *ld, Relation rel, bool parallel)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	if (ld->stream)
	{
		stream_flush(ld, rel, st);
		return;
	}
	if (ld->rl)
	{
		load_ring_stop(ld, rel, st);
		return;
	}
	if (!ld->open)
		return;
	if (ld->n > 0)
		load_end_unit(ld);
	end_run(ld, rel, st, parallel, false);
}

static void
flush_and_drop(Load *ld)
{
	Relation	rel = try_table_open(ld->relid, RowExclusiveLock);

	if (rel != NULL)
	{
		if (!ld->refused)
			flush_load(ld, rel, false);
		table_close(rel, NoLock);
	}
	drop_load(ld);
}

static void
load_xact_event(XactEvent event, void *arg)
{
	sift_tbl_load_from_copy = false;
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			foreach_ptr(Load, ld, list_copy(loads))
				flush_and_drop(ld);
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PREPARE:
			foreach_ptr(Load, ld, list_copy(loads))
				drop_load(ld);
			break;
		default:
			break;
	}
}

static void
load_subxact_event(SubXactEvent event, SubTransactionId mysubid, SubTransactionId parentsubid, void *arg)
{
	if (event != SUBXACT_EVENT_PRE_COMMIT_SUB && event != SUBXACT_EVENT_ABORT_SUB)
		return;
	foreach_ptr(Load, ld, list_copy(loads))
	{
		if (ld->subid != mysubid)
			continue;
		if (event == SUBXACT_EVENT_PRE_COMMIT_SUB)
			flush_and_drop(ld);
		else
			drop_load(ld);
	}
}

/* A load without workers keeps its segments' writer between rows; the lock keeps the orphan sweep of a VACUUM from the blocks it writes before they are published (the ring's, ring.c) */
static void
stream_begin(Load *ld, Relation rel, SiftTblRel *st)
{
	const SiftTblLayout *l = layout_of(rel, st);
	MemoryContext cx;

	if (load_groups(rel) > 1 || !ConditionalLockPage(rel, SIFT_LOAD_LOCK_BLKNO, ShareLock))
		return;
	cx = AllocSetContextCreate(ld->cxt, "sift_table load stream", ALLOCSET_DEFAULT_SIZES);
	ld->stream = true;
	ld->sw = (SealWriter) {rel, l, layout_policy(rel, l), cx, sift_cbatch_create(cx, l->schema), NULL, NULL, 0, tbl_col_format(ld->run.vers)};
	ld->rowcx = AllocSetContextCreate(cx, "sift_table load row", ALLOCSET_SMALL_SIZES);
	ld->budget = load_budget();
	ld->maxsegs = 8;
	ld->segs = palloc(ld->maxsegs * sizeof(SiftSegEntry));
}

bool
sift_tbl_load_wanted(Relation rel, SiftTblRel *st, bool frozen)
{
	Oid			relid = RelationGetRelid(rel);
	Load	   *ld = find_load(relid);
	MemoryContext cxt,
				old;

	if (ld != NULL)
		return !ld->refused;
	if (!callbacks_registered)
	{
		RegisterXactCallback(load_xact_event, NULL);
		RegisterSubXactCallback(load_subxact_event, NULL);
		callbacks_registered = true;
	}
	cxt = AllocSetContextCreate(TopMemoryContext, "sift_table bulk load", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	ld = palloc0(sizeof(Load));
	ld->relid = relid;
	ld->subid = GetCurrentSubTransactionId();
	ld->cxt = cxt;
	ld->refused = RecoveryInProgress() || sift_tbl_bulk_refusal(rel) != NULL;
	MemoryContextSwitchTo(TopMemoryContext);
	loads = lappend(loads, ld);
	if (!ld->refused)
	{
		MemoryContextSwitchTo(cxt);
		ld->run.vers = !frozen;
		ld->ring_ok = sift_tbl_load_from_copy && !load_row_work_unsafe_in_parallel_mode(rel);
		(void) layout_of(rel, st);
		ld->desc = RelationGetDescr(rel);
		ld->flat = palloc(ld->desc->natts * sizeof(Datum));
		initStringInfo(&ld->rows);
		stream_begin(ld, rel, st);
	}
	MemoryContextSwitchTo(old);
	return !ld->refused;
}

/* The row as a record at the end of the buffer of the run; false, with nothing written, when the buffer is a slot of the ring and the row does not fit */
static bool
put_row(Load *ld, const Datum *values, const bool *isnull, CommandId cid, Size datasize)
{
	TupleDesc	desc = ld->desc;
	const Datum *vals = values;
	bool		hasnull = false;
	Size		hoff,
				len;
	LoadRec    *rec;
	HeapTupleHeader th;

	for (int a = 0; a < desc->natts; a++)
	{
		if (isnull[a])
			hasnull = true;
		else if (TupleDescAttr(desc, a)->attlen == -1 && VARATT_IS_EXTERNAL(DatumGetPointer(values[a])) &&
				 !VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(values[a])))
		{
			/* the workers have no snapshot to read a toasted value with */
			if (vals == values)
			{
				memcpy(ld->flat, values, desc->natts * sizeof(Datum));
				vals = ld->flat;
			}
			ld->flat[a] = PointerGetDatum(detoast_attr((struct varlena *) DatumGetPointer(values[a])));
		}
	}
	hoff = MAXALIGN(SizeofHeapTupleHeader + (hasnull ? BITMAPLEN(desc->natts) : 0));
	if (vals != values)
		datasize = heap_compute_data_size(desc, (Datum *) vals, (bool *) isnull);
	len = sizeof(LoadRec) + hoff + datasize;
	if (ld->rows.maxlen - ld->rows.len <= MAXALIGN(len))
	{
		if (ld->rl)
			return false;
		enlargeStringInfo(&ld->rows, MAXALIGN(len));
	}
	rec = (LoadRec *) (ld->rows.data + ld->rows.len);
	rec->len = len;
	rec->cid = cid;
	th = (HeapTupleHeader) (rec + 1);
	memset(th, 0, len - sizeof(LoadRec));	/* the padding must be zero for heap_deform_tuple to find the next value */
	HeapTupleHeaderSetNatts(th, desc->natts);
	th->t_hoff = hoff;
	heap_fill_tuple(desc, vals, isnull, (char *) th + hoff, len - sizeof(LoadRec) - hoff, &th->t_infomask, hasnull ? th->t_bits : NULL);
	ld->rows.len += MAXALIGN(len);
	return true;
}

void
sift_tbl_load_row(Relation rel, SiftTblRel *st, const Datum *values, const bool *isnull, CommandId cid, ItemPointer tid)
{
	Load	   *ld = find_load(RelationGetRelid(rel));
	Size		hoff = offsetof(MinimalTupleData, t_bits);
	Size		datasize = heap_compute_data_size(ld->desc, (Datum *) values, (bool *) isnull);

	if (ld->stream)
		stream_row(ld, rel, st, values, isnull, cid, tid);
	else
	{
		for (;;)
		{
			if (!ld->open)
				reserve_run(ld, rel, st);
			else if (ld->n >= LOAD_SB_ROWS || ld->bytes >= load_unit_bytes())
			{
				load_end_unit(ld);
				if (ld->run.units == ld->maxunits)
				{
					end_run(ld, rel, st, true, true);
					reserve_run(ld, rel, st);
				}
			}
			if (put_row(ld, values, isnull, cid, datasize))
				break;
			/* the slot of the ring is full: the group is handed over with the units it has, and a row that no slot holds goes the way of a load without workers */
			if (ld->n == 0 && ld->run.units == 0)
			{
				ld->noring = true;
				load_ring_stop(ld, rel, st);
			}
			else
			{
				if (ld->n > 0)
					load_end_unit(ld);
				end_run(ld, rel, st, true, true);
			}
		}
		ItemPointerSet(tid, ld->run.first + (uint64) ld->run.units * SIFT_TBL_VBLOCKS_SB + ld->n / SIFT_TBL_MAX_OFF, ld->n % SIFT_TBL_MAX_OFF + 1);
	}
	ld->n++;
	/* the bytes the row has as a minimal tuple (the write buffer limit counts them) */
	for (int a = 0; a < ld->desc->natts; a++)
		if (isnull[a])
		{
			hoff += BITMAPLEN(ld->desc->natts);
			break;
		}
	ld->bytes += MAXALIGN(hoff) + datasize;
}

void
sift_tbl_load_finish(Relation rel)
{
	Load	   *ld = find_load(RelationGetRelid(rel));

	if (ld == NULL)
		return;
	PG_TRY();
	{
		if (!ld->refused)
			flush_load(ld, rel, ActiveSnapshotSet());
	}
	PG_FINALLY();
	{
		drop_load(ld);
	}
	PG_END_TRY();
}

