/*
 * core/sql_space.c - sift.space(rel): the space of a sift index, a sift_col index or a sift_table in one row, Format 1 and Format 2.0, and the reader of it that sift.status shares (sql_api.h).
 *
 * Format 1 (sift, sift_col) walks the runs the directory and the free list account for, as sift.col_space did; a sift_table of Format 1 counts the free list of its file; Format 2.0 reads the
 * metapage and the allocator (core/f2/fsm.h) for either access method, and the manifest for the parts.
 */
#include "postgres.h"

#include "access/xlog.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "utils/builtins.h"

#include "col/f2/col_meta.h"
#include "col/v1/seg.h"
#include "core/explain.h"
#include "core/f2/fsm.h"
#include "core/f2/meta.h"
#include "core/ops.h"
#include "core/sql_api.h"
#include "core/store.h"
#include "table/v1/table_v1.h"

PG_FUNCTION_INFO_V1(sift_space);

/* columns of sift.space, in the order of sql/6.0/15-unified-api.sql */
enum SpaceCol
{
	SPC_AM,
	SPC_RELATION_BLOCKS,
	SPC_HIGH_WATER,
	SPC_USED_BLOCKS,
	SPC_FREE_BLOCKS,
	SPC_REUSABLE_BLOCKS,
	SPC_PENDING_BLOCKS,
	SPC_FREE_EXTENTS,
	SPC_LARGEST_FREE_EXTENT,
	SPC_PARTS,
	SPC_ROWS,
	SPC_DEAD_ROWS,
	SPC_BYTES,
	SPC_MAINTENANCE_BUSY,
	SPC_LAST_SHRINK,
	SPC_SHRINK_BLOCKED_BY,
	SPC_VBLOCKS_LEFT,
	SPC_NCOLS
};

static const Oid space_types[SPC_NCOLS] = {
	TEXTOID, INT8OID, INT8OID, INT8OID, INT8OID, INT8OID, INT8OID, INT8OID, INT8OID, INT4OID, INT8OID, INT8OID, INT8OID, BOOLOID, TIMESTAMPTZOID, TEXTOID, INT8OID
};

/* Whether a lock of the mode would have to wait for another session, read from the lock manager: a read-only scrape acquires nothing */
static bool
lock_busy(const LOCKTAG *tag, LOCKMODE mode)
{
	int			holders;

	(void) GetLockConflicts(tag, mode, &holders);
	return holders > 0;
}

/* The page lock that a promotion, a merge, VACUUM or a worker holds while it runs */
static bool
page_lock_busy(Relation rel, BlockNumber blk)
{
	LOCKTAG		tag;

	SET_LOCKTAG_PAGE(tag, rel->rd_lockInfo.lockRelId.dbId, rel->rd_lockInfo.lockRelId.relId, blk);
	return lock_busy(&tag, ExclusiveLock);
}

/* The ShareUpdateExclusiveLock of the table that VACUUM and sift.maintain() hold */
static bool
maintenance_lock_busy(Relation rel)
{
	LOCKTAG		tag;

	SET_LOCKTAG_RELATION(tag, rel->rd_lockInfo.lockRelId.dbId, rel->rd_lockInfo.lockRelId.relId);
	return lock_busy(&tag, ShareUpdateExclusiveLock);
}

static void
set_busy(SiftApiSpace *s, bool busy)
{
	s->busy_known = !RecoveryInProgress();
	s->busy = s->busy_known && busy;
}

typedef struct ExtentTally
{
	int64		used;
	int64		reusable;
	int64		pending;
	int64		largest;
	int64		extents;
} ExtentTally;

static void
tally_extent(void *arg, const char *what, uint64 gen, BlockNumber start, uint64 nblocks, bool pending)
{
	ExtentTally *t = arg;

	if (strcmp(what, "free extent") != 0)
	{
		t->used += nblocks;
		return;
	}
	if (pending)
		t->pending += nblocks;
	else
		t->reusable += nblocks;
	t->extents++;
	t->largest = Max(t->largest, (int64) nblocks);
}

/* sift and sift_col of Format 1: one directory, one free list */
static void
space_directory(const SiftApiRel *r, SiftApiSpace *s)
{
	SiftDirSnapshot *snap = sift_dir_read(r->rel);
	ExtentTally t = {0};
	SiftIndexStats st = {0};

	sift_store_extent_map(r->rel, tally_extent, &t);
	if (r->am == SIFT_API_BM25)
		sift_bm25_stats(r->rel, &st);
	else
		sift_col_stats(r->rel, &st);
	s->high_water = snap->meta.high_water;
	s->used_blocks = t.used;
	s->free_blocks = t.reusable + t.pending;
	s->reusable_blocks = t.reusable;
	s->pending_blocks = t.pending;
	s->free_extents = t.extents;
	s->largest_free_extent = t.largest;
	s->has_largest = true;
	s->parts = snap->nsegs;
	s->rows = st.rows + st.buffered_rows;
	s->dead_rows = st.dead_rows;
	s->has_buffered = true;
	s->buffered_rows = st.buffered_rows;
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			s->bytes += snap->segs[i].bytes;
	s->last_shrink = snap->meta.last_shrink;
	s->shrink_blocked_by = sift_api_shrink_blocked_by(snap->meta.shrink_blocked);
	set_busy(s, sift_maintenance_busy(r->rel));
}

/* a sift_table of Format 1: the segments of columns hold the rows, the free list the blocks nothing holds */
static void
space_table_v1(const SiftApiRel *r, SiftApiSpace *s)
{
	SiftTblRel *st = sift_tbl_rel(r->rel);
	SiftFreeStats fs = {0};
	sift_tbl_refresh_full(r->rel, st);
	if (RelationGetNumberOfBlocks(r->rel) > 0)
		fs = sift_store_free_stats(r->rel);
	s->high_water = st->dir->meta.high_water;
	s->free_blocks = fs.blocks;
	s->reusable_blocks = fs.reusable;
	s->pending_blocks = fs.blocks - fs.reusable;
	s->used_blocks = s->high_water - fs.blocks;
	s->free_extents = fs.records;
	s->parts = st->dir->nsegs;
	for (int i = 0; i < st->dir->nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir->segs[i];
		SiftColSegment *seg;

		if (e->kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(r->rel, e, CurrentMemoryContext);
		s->rows += e->ndocs;
		s->dead_rows += e->ndocs - sift_cseg_live_rows(seg);
		s->bytes += e->bytes;
		sift_cseg_close(seg);
	}
	s->last_shrink = st->dir->meta.last_shrink;
	s->shrink_blocked_by = sift_api_shrink_blocked_by(st->dir->meta.shrink_blocked);
	s->has_vblocks = true;
	s->vblocks_left = (int64) MaxBlockNumber - (int64) SIFT_TBL_NEXT_VBLOCK(&st->dir->meta);
	set_busy(s, !RecoveryInProgress() && maintenance_lock_busy(r->rel));
}

/* sift_col and sift_table of Format 2.0: the metapage, the allocator and the manifest */
static void
space_f2(const SiftApiRel *r, SiftApiSpace *s)
{
	SiftColMetaPageData meta = sift_col_meta_read(r->rel);
	SiftFsmStats st = sift_fsm_check(r->rel);

	for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
	{
		s->reusable_blocks += st.class_blocks[k];
		s->free_extents += st.class_extents[k];
		if (st.class_extents[k] > 0)
			s->largest_free_extent = SIFT_FSM_CLASS_BLOCKS(k);
	}
	s->has_largest = true;
	s->free_extents += st.quarantine_extents;
	s->pending_blocks = st.quarantine_blocks;
	s->free_blocks = s->reusable_blocks + s->pending_blocks;
	s->high_water = meta.high_water;
	s->used_blocks = s->high_water - s->free_blocks;
	s->parts = (int32) sift_api_f2_count(r->rel).parts;
	s->rows = (int64) meta.total_rows;
	s->dead_rows = (int64) meta.total_dead;
	s->bytes = (int64) meta.total_bytes;
	s->last_shrink = meta.last_shrink_at;
	s->shrink_blocked_by = sift_api_shrink_blocked_by(meta.shrink_blocked);
	if (r->am == SIFT_API_TABLE)
	{
		s->has_vblocks = true;
		s->vblocks_left = (int64) MaxBlockNumber - (int64) meta.next_vblock;
	}
	set_busy(s, page_lock_busy(r->rel, SIFT_F2_MAINT_LOCK_BLKNO));
}

SiftApiSpace
sift_api_space(const SiftApiRel *r)
{
	SiftApiSpace s = {0};

	s.relation_blocks = RelationGetNumberOfBlocks(r->rel);
	if (r->peek.status == SIFT_FMT_EMPTY)
	{
		s.busy_known = true;
		s.has_vblocks = r->am == SIFT_API_TABLE;
		s.vblocks_left = (int64) MaxBlockNumber;
		return s;
	}
	if (r->f2)
		space_f2(r, &s);
	else if (r->am == SIFT_API_TABLE)
		space_table_v1(r, &s);
	else
		space_directory(r, &s);
	return s;
}

Datum
sift_space(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	SiftApiRel	r;
	SiftApiSpace s;
	Datum		v[SPC_NCOLS] = {0};
	bool		nulls[SPC_NCOLS] = {false};

	InitMaterializedSRF(fcinfo, 0);
	sift_api_check_desc(rsi->setDesc, space_types, SPC_NCOLS);
	r = sift_api_open(PG_GETARG_OID(0), AccessShareLock);
	s = sift_api_space(&r);
	v[SPC_AM] = CStringGetTextDatum(sift_api_am_name(r.am));
	v[SPC_RELATION_BLOCKS] = Int64GetDatum(s.relation_blocks);
	v[SPC_HIGH_WATER] = Int64GetDatum(s.high_water);
	v[SPC_USED_BLOCKS] = Int64GetDatum(s.used_blocks);
	v[SPC_FREE_BLOCKS] = Int64GetDatum(s.free_blocks);
	v[SPC_REUSABLE_BLOCKS] = Int64GetDatum(s.reusable_blocks);
	v[SPC_PENDING_BLOCKS] = Int64GetDatum(s.pending_blocks);
	v[SPC_FREE_EXTENTS] = Int64GetDatum(s.free_extents);
	nulls[SPC_LARGEST_FREE_EXTENT] = !s.has_largest;
	v[SPC_LARGEST_FREE_EXTENT] = Int64GetDatum(s.largest_free_extent);
	v[SPC_PARTS] = Int32GetDatum(s.parts);
	v[SPC_ROWS] = Int64GetDatum(s.rows);
	v[SPC_DEAD_ROWS] = Int64GetDatum(s.dead_rows);
	v[SPC_BYTES] = Int64GetDatum(s.bytes);
	nulls[SPC_MAINTENANCE_BUSY] = !s.busy_known;
	v[SPC_MAINTENANCE_BUSY] = BoolGetDatum(s.busy);
	nulls[SPC_LAST_SHRINK] = s.last_shrink == 0;
	v[SPC_LAST_SHRINK] = TimestampTzGetDatum(s.last_shrink);
	nulls[SPC_SHRINK_BLOCKED_BY] = s.shrink_blocked_by == NULL;
	if (s.shrink_blocked_by != NULL)
		v[SPC_SHRINK_BLOCKED_BY] = CStringGetTextDatum(s.shrink_blocked_by);
	nulls[SPC_VBLOCKS_LEFT] = !s.has_vblocks;
	v[SPC_VBLOCKS_LEFT] = Int64GetDatum(s.vblocks_left);
	tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	sift_api_close(&r, AccessShareLock);
	return (Datum) 0;
}
