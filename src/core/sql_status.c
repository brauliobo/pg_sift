/*
 * core/sql_status.c - sift.status(rel): everything about one sift index, sift_col index or sift_table (or every one the caller may look at, rel NULL), one row each, from the metapage, the
 * directory or the manifest and the headers of the parts: no data page is read. Format 1 and Format 2.0 alike; NULL is not applicable or not known, 0 is measured zero.
 *
 * The function never fails for a relation: one that is invalid, locked by another session, quarantined, of a newer format or unreadable is a row too, with what can be said of it. Each relation is
 * read in a subtransaction of its own, so a damaged one costs its row and not the scrape; a cancel is not swallowed. state is, in this order, invalid, locked, quarantined, failing (a background step
 * failed since the last one that ended well), maintaining (a promotion, a merge or a VACUUM holds it) or ready; Format 1 is ready: upgrading it is optional.
 *
 * maint_due is the first, in the order of the spec, of the reasons that need no threshold of a worker: a slab or a pending deletion waits, a compaction plan is not empty, a part has the dead
 * rows the rewrite wants, a scope summary is stale, a partition is splitting, or the free end of the file is what a tail shrink takes. Where the part headers are read (granules, slack, skip_bytes)
 * the cost is one header page for each part. dirty_rows of Format 2.0 is the rows of the slab of each partition (the headers of its pages) and the deletes its pending chain holds, so a partition
 * with neither costs nothing; granules of a Format 1 sift_col is the units its directory lists, the number the planner prices.
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type_d.h"
#include "datatype/timestamp.h"
#include "funcapi.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/syscache.h"

#include "col/f2/col_meta.h"
#include "col/f2/compact.h"
#include "col/f2/f2_scope.h"
#include "col/f2/l0_slab.h"
#include "col/f2/part_format.h"
#include "core/f2/fsm.h"
#include "core/f2/manifest.h"
#include "core/inspect.h"
#include "core/memtrack.h"
#include "core/quarantine.h"
#include "core/shm_manifest.h"
#include "core/sql_api.h"
#include "core/store.h"
#include "col/v1/seg.h"
#include "table/v1/table_v1.h"

PG_FUNCTION_INFO_V1(sift_status);

/* columns of sift.status, in the order of sql/6.0/15-unified-api.sql */
enum StatusCol
{
	ST_RELATION,
	ST_TABLE_NAME,
	ST_AM,
	ST_FORMAT_MAJOR,
	ST_FORMAT_MINOR,
	ST_FORMAT_NOTE,
	ST_STATE,
	ST_QUARANTINED,
	ST_QUARANTINE_REASON,
	ST_LIVE_ROWS,
	ST_DEAD_ROWS,
	ST_DIRTY_ROWS,
	ST_DEAD_PCT,
	ST_SIZE_BYTES,
	ST_HEAP_BYTES,
	ST_B_PER_ROW,
	ST_COMPRESSION_RATIO,
	ST_UTIL_PCT,
	ST_DATA_BYTES,
	ST_META_BYTES,
	ST_SCOPE_BYTES,
	ST_SKIP_BYTES,
	ST_FREE_BYTES,
	ST_REUSABLE_BYTES,
	ST_SLACK_BYTES,
	ST_PARTITIONS,
	ST_L0_SLABS,
	ST_L0_PARTS,
	ST_L1_PARTS,
	ST_L2_PARTS,
	ST_HOT_PARTS,
	ST_COLD_PARTS,
	ST_GRANULES,
	ST_SCOPES,
	ST_VBLOCKS_LEFT,
	ST_MAINT_RUNNING,
	ST_MAINT_DUE,
	ST_MAINT_FAILURES,
	ST_LAST_MAINT_AT,
	ST_LAST_SHRINK_AT,
	ST_SHRINK_BLOCKED_BY,
	ST_SHM_MANIFEST_CACHED,
	ST_SHM_MANIFEST_BYTES,
	ST_SHM_HIT_PCT,
	ST_NCOLS
};

static const Oid status_types[ST_NCOLS] = {
	REGCLASSOID, REGCLASSOID, TEXTOID, INT2OID, INT2OID, TEXTOID, TEXTOID, BOOLOID, TEXTOID,
	INT8OID, INT8OID, INT8OID, NUMERICOID,
	INT8OID, INT8OID, NUMERICOID, NUMERICOID, NUMERICOID,
	INT8OID, INT8OID, INT8OID, INT8OID, INT8OID, INT8OID, INT8OID,
	INT4OID, INT4OID, INT4OID, INT4OID, INT4OID, INT4OID, INT4OID, INT8OID, INT2OID, INT8OID,
	BOOLOID, TEXTOID, INT4OID, TIMESTAMPTZOID, TIMESTAMPTZOID, TEXTOID,
	BOOLOID, INT8OID, NUMERICOID
};

typedef struct StatusRow
{
	Datum		v[ST_NCOLS];
	bool		nulls[ST_NCOLS];
} StatusRow;

/* A relation to report: found in pg_class, not yet opened */
typedef struct Candidate
{
	Oid			relid;
	Oid			heap;			/* the table of an index, the relation itself for a sift_table */
	SiftApiAm	am;
	bool		valid;			/* an index that is valid and ready */
} Candidate;

static void
put(StatusRow *row, int col, Datum d)
{
	row->v[col] = d;
	row->nulls[col] = false;
}

static void
put_i64(StatusRow *row, int col, int64 x)
{
	put(row, col, Int64GetDatum(x));
}

static void
put_i32(StatusRow *row, int col, int32 x)
{
	put(row, col, Int32GetDatum(x));
}

static void
put_bool(StatusRow *row, int col, bool x)
{
	put(row, col, BoolGetDatum(x));
}

static void
put_text(StatusRow *row, int col, const char *s)
{
	put(row, col, CStringGetTextDatum(s));
}

static void
put_ts(StatusRow *row, int col, TimestampTz t)
{
	if (t != 0)
		put(row, col, TimestampTzGetDatum(t));
}

static void
put_percent(StatusRow *row, int col, int64 part, int64 whole)
{
	if (whole > 0)
		put(row, col, sift_api_percent((uint64) Max(part, 0), (uint64) whole));
}

static void
put_ratio(StatusRow *row, int col, int64 num, int64 den)
{
	if (den > 0)
		put(row, col, sift_api_ratio((uint64) Max(num, 0), (uint64) den));
}

static void
row_begin(StatusRow *row, const Candidate *c)
{
	memset(row->v, 0, sizeof(row->v));
	memset(row->nulls, true, sizeof(row->nulls));
	put(row, ST_RELATION, ObjectIdGetDatum(c->relid));
	put(row, ST_TABLE_NAME, ObjectIdGetDatum(c->heap));
	put_text(row, ST_AM, sift_api_am_name(c->am));
}

/* What sift_internal.format_note() says, word for word */
static const char *
format_note(SiftApiAm am, const SiftMetaPeek *peek, int64 size_bytes)
{
	if (size_bytes == 0 || peek->status == SIFT_FMT_EMPTY)
		return "empty";
	if (peek->format_major >= 2)
		return "Format 2.0";
	if (am == SIFT_API_BM25)
		return "Format 1.7 (current)";
	if (am == SIFT_API_TABLE)
		return "Format 1.x (fully supported; optional SELECT * FROM sift.upgrade_table(...) upgrades to Format 2.0)";
	return "Format 1.x (fully supported; optional REINDEX INDEX CONCURRENTLY upgrades to Format 2.0)";
}

/* What a Format 2.0 relation holds, from the manifest and the headers of its parts */
typedef enum Due
{
	DUE_NONE,
	DUE_SLAB_FLUSH,
	DUE_PEND_DEL,
	DUE_L0_COMPACT,
	DUE_DEAD_REWRITE,
	DUE_SCOPE_REFRESH,
	DUE_SPLIT,
	DUE_MERGE,
	DUE_TAIL_SHRINK
} Due;

static const char *const due_names[] = {
	[DUE_NONE] = NULL,
	[DUE_SLAB_FLUSH] = "slab_flush",
	[DUE_PEND_DEL] = "pend_del",
	[DUE_L0_COMPACT] = "l0_compact",
	[DUE_DEAD_REWRITE] = "dead_rewrite",
	[DUE_SCOPE_REFRESH] = "scope_refresh",
	[DUE_SPLIT] = "split",
	[DUE_MERGE] = "merge",
	[DUE_TAIL_SHRINK] = "tail_shrink"
};

typedef struct F2Tally
{
	int32		partitions;
	int32		l0_slabs;
	int32		l0_parts;
	int32		l1_parts;
	int32		l2_parts;
	int32		hot_parts;
	int32		cold_parts;
	int32		scopes;
	int64		granules;
	int64		dirty_rows;
	int64		data_bytes;
	int64		skip_bytes;
	int64		scope_bytes;
	int64		slack_bytes;
	Due			due;
} F2Tally;

/* A reason noted already comes before d in the Due order: noting d would change nothing */
static bool
due_ranked(const F2Tally *t, Due d)
{
	return t->due != DUE_NONE && t->due < d;
}

static void
due_note(F2Tally *t, Due d)
{
	if (t->due == DUE_NONE || d < t->due)
		t->due = d;
}

/* The stream of a part by its section directory: the granules, the skipping indexes, and the pages of the stream left over by the sections as slack */
static void
part_sizes(const SiftPart *part, F2Tally *t)
{
	uint64		counted = 0;

	for (int i = 0; i < part->hdr.nsec; i++)
	{
		const SiftSecDirEntry *s = &part->secdir[i];

		if (s->flags & SIFT_SEC_FLAG_INLINE)
			continue;
		counted += s->len;
		if (s->tag == SEC_GRANULES)
			t->data_bytes += s->len;
		else if (s->tag == SEC_SKIP_INDEX)
			t->skip_bytes += s->len;
	}
	t->slack_bytes += (int64) (SIFT_PAYLOAD_PAGES(part->hdr.stream_bytes) * BLCKSZ - counted);
	t->granules += part->hdr.ngranules;
}

/* Each reason is noted only when it outranks the one noted already (due_note keeps the first of the Due order): the plans that cost a scan of the parts of the partition are made only for a reason that could still win */
static void
partition_due(F2Tally *t, const SiftPartitionEntry *e, const SiftPartDesc *pd, int np)
{
	int			ngroups = 0,
				ncand = 0;
	bool		has_l0 = false;

	if (e->l0_slab_blk != InvalidBlockNumber)
		due_note(t, DUE_SLAB_FLUSH);
	if (e->pend_del_blk != InvalidBlockNumber)
		due_note(t, DUE_PEND_DEL);
	if (e->flags & SIFT_PARTENTRY_SPLITTING)
		due_note(t, DUE_SPLIT);
	if (np == 0)
		return;
	for (int p = 0; p < np; p++)
		has_l0 |= pd[p].level == 0;
	if (!due_ranked(t, has_l0 ? DUE_L0_COMPACT : DUE_MERGE))
	{
		(void) sift_compact_plan(pd, np, UINT64_MAX, PG_INT64_MIN, &ngroups);
		if (ngroups > 0)
			due_note(t, has_l0 ? DUE_L0_COMPACT : DUE_MERGE);
	}
	if (!due_ranked(t, DUE_DEAD_REWRITE))
	{
		(void) sift_compact_candidates(pd, np, &ncand);
		if (ncand > 0)
			due_note(t, DUE_DEAD_REWRITE);
	}
}

/* What waits for a flush or a fold: the rows of the slab, and the deletes of the pending chain (the successor record of an UPDATE is not one) */
static int64
partition_dirty_rows(Relation rel, const SiftPartitionEntry *e)
{
	int64		dirty = 0;

	if (e->l0_slab_blk != InvalidBlockNumber)
		dirty += (int64) sift_l0_slab_rows(rel, e->part_id, e->l0_slab_blk);
	if (e->pend_del_blk != InvalidBlockNumber)
	{
		int			n;
		SiftPendDelRec *recs = sift_pend_del_read(rel, e->part_id, e->pend_del_blk, &n, NULL, NULL);

		for (int i = 0; i < n; i++)
			dirty += (recs[i].tid & SIFT_PD_TID_SUCC) == 0;
		pfree(recs);
	}
	return dirty;
}

/* The headers of the parts of one partition are read, counted and let go before the next partition's: the memory of the tally follows the partition with the most parts, not the relation */
static void
tally_partitions(Relation rel, F2Tally *t)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	MemoryContext scratch = sift_mem_allocset_create(CurrentMemoryContext, "sift status partition");

	t->partitions = mr.n;
	for (int i = 0; i < mr.n; i++)
	{
		const SiftPartitionEntry *e = &mr.entries[i];
		MemoryContext old = MemoryContextSwitchTo(scratch);
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, e, &np);

		t->l0_slabs += e->l0_slab_blk != InvalidBlockNumber;
		t->hot_parts += e->hot_parts;
		t->dirty_rows += partition_dirty_rows(rel, e);
		for (int p = 0; p < np; p++)
		{
			t->l0_parts += pd[p].level == 0;
			t->l1_parts += pd[p].level == 1;
			t->l2_parts += pd[p].level >= 2;
			t->cold_parts += (pd[p].flags & SIFT_PART_FLAG_TIME_COLD) != 0;
			if (!(pd[p].ext_flags & SIFT_PART_EXT_REMOTE))
				part_sizes(sift_part_open_desc(rel, &pd[p], (uint32) e->part_id), t);
		}
		partition_due(t, e, pd, np);
		MemoryContextSwitchTo(old);
		MemoryContextReset(scratch);
	}
	MemoryContextDelete(scratch);
}

static void
tally_scopes(const SiftApiRel *r, const SiftColMetaPageData *meta, F2Tally *t)
{
	int			nref;
	SiftManifestScopeRef *refs = sift_manifest_read_scopes(r->rel, meta, &nref);

	for (int i = 0; i < nref; i++)
	{
		if (!(refs[i].flags & SIFT_SCOPEREF_ACTIVE) || refs[i].header_blk == InvalidBlockNumber)
			continue;
		t->scopes++;
		t->scope_bytes += (int64) sift_part_open(r->rel, refs[i].header_blk, refs[i].meta_gen, 0)->hdr.stream_bytes;
		if (r->am == SIFT_API_COL && !sift_f2_scope_current(r->rel, refs[i].scope_ord))
			due_note(t, DUE_SCOPE_REFRESH);
	}
}

/* What the registry of the manifests in shared memory holds of the relation: whether it has the manifest, and the share of the pins of its snapshot that found one (NULL before the first) */
static void
fill_shm(StatusRow *row, Relation rel)
{
	SiftShmIndexMeta peeked;
	uint64		hits,
				misses;
	bool		cached = sift_shm_manifest_enabled() && sift_shm_manifest_peek(&rel->rd_locator, &peeked);
	int64		bytes = 0;

	put_bool(row, ST_SHM_MANIFEST_CACHED, cached);
	if (cached)
	{
		const SiftShmManifestSnap *snap = sift_shm_snap_pin(&rel->rd_locator);

		if (snap != NULL)
		{
			bytes = (int64) pg_atomic_read_u64(&((SiftShmManifestSnap *) snap)->snap_bytes);
			sift_shm_snap_unpin(snap);
		}
		if (sift_shm_manifest_pin_stat(&rel->rd_locator, &hits, &misses))
			put_percent(row, ST_SHM_HIT_PCT, (int64) hits, (int64) (hits + misses));
	}
	put_i64(row, ST_SHM_MANIFEST_BYTES, bytes);
}

static void
fill_f2(StatusRow *row, const SiftApiRel *r, const SiftApiSpace *sp)
{
	SiftColMetaPageData meta = sift_col_meta_read(r->rel);
	F2Tally		t = {0};
	int64		size = sp->relation_blocks * BLCKSZ;
	int64		free_bytes = sp->free_blocks * BLCKSZ;

	tally_partitions(r->rel, &t);
	tally_scopes(r, &meta, &t);
	if (sp->reusable_blocks >= SIFT_TAIL_SHRINK_MIN_BLOCKS && sp->reusable_blocks * 100 >= (int64) SIFT_TAIL_SHRINK_FREE_PCT * sp->high_water)
		due_note(&t, DUE_TAIL_SHRINK);
	put_i64(row, ST_DATA_BYTES, t.data_bytes);
	put_i64(row, ST_META_BYTES, Max(0, size - t.data_bytes - t.scope_bytes - t.skip_bytes - free_bytes - t.slack_bytes));
	put_i64(row, ST_SCOPE_BYTES, t.scope_bytes);
	put_i64(row, ST_SKIP_BYTES, t.skip_bytes);
	put_i64(row, ST_SLACK_BYTES, t.slack_bytes);
	put_percent(row, ST_UTIL_PCT, size - free_bytes - t.slack_bytes, size);
	put_i32(row, ST_PARTITIONS, t.partitions);
	put_i32(row, ST_L0_SLABS, t.l0_slabs);
	put_i32(row, ST_L0_PARTS, t.l0_parts);
	put_i32(row, ST_L1_PARTS, t.l1_parts);
	put_i32(row, ST_L2_PARTS, t.l2_parts);
	put_i32(row, ST_HOT_PARTS, t.hot_parts);
	put_i32(row, ST_COLD_PARTS, t.cold_parts);
	put_i64(row, ST_GRANULES, t.granules);
	put_i64(row, ST_DIRTY_ROWS, t.dirty_rows);
	put(row, ST_SCOPES, Int16GetDatum((int16) t.scopes));
	if (t.due != DUE_NONE)
		put_text(row, ST_MAINT_DUE, due_names[t.due]);
	put_i32(row, ST_MAINT_FAILURES, (int32) meta.failures_consec);
	put_ts(row, ST_LAST_MAINT_AT, meta.maint_ok_at);
	fill_shm(row, r->rel);
}

/* The segments and buffers of a Format 1 relation: what its directory lists */
static void
fill_v1(StatusRow *row, const SiftApiRel *r, const SiftApiSpace *sp)
{
	bool		table = r->am == SIFT_API_TABLE;
	const SiftDirSnapshot *dir = table ? sift_tbl_rel(r->rel)->dir : sift_dir_read(r->rel);
	int32		open = 0,
				sealed = 0,
				immutable = 0;
	int64		size = sp->relation_blocks * BLCKSZ;
	int64		free_bytes = sp->free_blocks * BLCKSZ;
	uint32		failures = table ? 0 : dir->meta.flags & SIFT_META_FAILURES_MASK;

	for (int i = 0; i < dir->nsegs; i++)
	{
		const SiftSegEntry *e = &dir->segs[i];

		if (table && (e->flags & SIFT_TBL_SEG_TOMB))
			continue;
		immutable += e->kind == SIFT_SEG_IMMUTABLE;
		sealed += e->kind == SIFT_SEG_WBUF_SEALED;
		open += e->kind == SIFT_SEG_WBUF_OPEN;
	}
	put_i64(row, ST_DATA_BYTES, sp->bytes);
	put_i64(row, ST_META_BYTES, Max(0, size - sp->bytes - free_bytes));
	put_percent(row, ST_UTIL_PCT, size - free_bytes, size);
	put_i32(row, ST_L0_SLABS, open);
	put_i32(row, ST_L0_PARTS, sealed);
	put_i32(row, ST_L1_PARTS, immutable);
	if (r->am == SIFT_API_COL)
		put_i64(row, ST_GRANULES, (int64) sift_cseg_dir_units(dir));
	if (sealed > 0)
		put_text(row, ST_MAINT_DUE, due_names[DUE_SLAB_FLUSH]);
	put_i32(row, ST_MAINT_FAILURES, (int32) failures);
	if (!table)
		put_ts(row, ST_LAST_MAINT_AT, dir->meta.maint_ok_at);
}

/* heap_bytes of an index: the file of its table, where the table is not locked against a reader */
static void
fill_heap_bytes(StatusRow *row, const Candidate *c, int64 size)
{
	Relation	heap;

	if (c->heap == c->relid || !ConditionalLockRelationOid(c->heap, AccessShareLock))
		return;
	heap = try_relation_open(c->heap, NoLock);
	if (heap == NULL)
	{
		UnlockRelationOid(c->heap, AccessShareLock);
		return;
	}
	put_i64(row, ST_HEAP_BYTES, (int64) RelationGetNumberOfBlocks(heap) * BLCKSZ);
	put_ratio(row, ST_COMPRESSION_RATIO, (int64) RelationGetNumberOfBlocks(heap) * BLCKSZ, size);
	relation_close(heap, AccessShareLock);
}

static const char *
state_of(const SiftApiSpace *sp, int64 failures)
{
	return failures > 0 ? "failing" : sp->busy ? "maintaining" : "ready";
}

static void
fill_common(StatusRow *row, const Candidate *c, const SiftApiRel *r, const SiftApiSpace *sp)
{
	int64		size = sp->relation_blocks * BLCKSZ;
	int64		live = sp->rows - sp->dead_rows;

	put_i64(row, ST_LIVE_ROWS, live);
	put_i64(row, ST_DEAD_ROWS, sp->dead_rows);
	if (sp->has_buffered)
		put_i64(row, ST_DIRTY_ROWS, sp->buffered_rows);
	put_percent(row, ST_DEAD_PCT, sp->dead_rows, sp->rows);
	put_i64(row, ST_SIZE_BYTES, size);
	fill_heap_bytes(row, c, size);
	put_ratio(row, ST_B_PER_ROW, size, live);
	put_i64(row, ST_FREE_BYTES, sp->free_blocks * BLCKSZ);
	put_i64(row, ST_REUSABLE_BYTES, sp->reusable_blocks * BLCKSZ);
	if (r->am == SIFT_API_TABLE)
		put_i64(row, ST_VBLOCKS_LEFT, sp->vblocks_left);
	if (sp->busy_known)
		put_bool(row, ST_MAINT_RUNNING, sp->busy);
	put_ts(row, ST_LAST_SHRINK_AT, sp->last_shrink);
	if (sp->shrink_blocked_by != NULL)
		put_text(row, ST_SHRINK_BLOCKED_BY, sp->shrink_blocked_by);
}

static void
put_format(StatusRow *row, const SiftApiRel *r, int64 size)
{
	if (r->peek.status == SIFT_FMT_V1 || r->peek.status == SIFT_FMT_V2 || r->peek.status == SIFT_FMT_OTHER)
	{
		put(row, ST_FORMAT_MAJOR, Int16GetDatum((int16) r->peek.format_major));
		put(row, ST_FORMAT_MINOR, Int16GetDatum((int16) r->peek.format_minor));
	}
	put_text(row, ST_FORMAT_NOTE, r->peek.status == SIFT_FMT_OTHER ? psprintf("Format %u.%u, written by a newer release", r->peek.format_major, r->peek.format_minor)
			 : r->peek.status == SIFT_FMT_BAD ? "block 0 is not a metapage of pg_sift" : format_note(r->am, &r->peek, size));
}

static bool
is_quarantined(const SiftApiRel *r)
{
	return (r->peek.flags & SIFT_META_QUARANTINED) != 0 || (r->rel->rd_rel->relkind == RELKIND_INDEX && sift_index_quarantined(r->rel));
}

static void
put_quarantine(StatusRow *row, const SiftApiRel *r, bool quarantined)
{
	char	   *reason = quarantined && r->rel->rd_rel->relkind == RELKIND_INDEX ? sift_quarantine_reason(r->rel) : NULL;

	put_bool(row, ST_QUARANTINED, quarantined);
	if (reason != NULL)
		put_text(row, ST_QUARANTINE_REASON, reason);
}

/* The row of one relation; false for one the caller may not look at or that is gone */
static bool
status_one(const Candidate *c, StatusRow *row)
{
	SiftApiRel	r = {.am = c->am};
	SiftApiSpace sp;
	int64		size;
	bool		quarantined;

	if (!sift_may_inspect(c->heap))
		return false;
	row_begin(row, c);
	if (!c->valid)
	{
		put_text(row, ST_STATE, "invalid");
		return true;
	}
	if (!ConditionalLockRelationOid(c->relid, AccessShareLock))
	{
		put_text(row, ST_STATE, "locked");
		return true;
	}
	r.rel = try_relation_open(c->relid, NoLock);
	if (r.rel == NULL || RELATION_IS_OTHER_TEMP(r.rel))
	{
		UnlockRelationOid(c->relid, AccessShareLock);
		return false;
	}
	r.peek = sift_meta_peek_format(r.rel);
	r.f2 = r.peek.status == SIFT_FMT_V2;
	size = (int64) RelationGetNumberOfBlocks(r.rel) * BLCKSZ;
	put_format(row, &r, size);
	quarantined = r.peek.status != SIFT_FMT_BAD && is_quarantined(&r);
	put_quarantine(row, &r, quarantined);
	if (r.peek.status == SIFT_FMT_OTHER || r.peek.status == SIFT_FMT_BAD || quarantined)
	{
		put_text(row, ST_STATE, quarantined ? "quarantined" : "invalid");
		put_i64(row, ST_SIZE_BYTES, size);
	}
	else
	{
		sp = sift_api_space(&r);
		fill_common(row, c, &r, &sp);
		if (r.f2)
			fill_f2(row, &r, &sp);
		else if (r.peek.status == SIFT_FMT_V1)
			fill_v1(row, &r, &sp);
		put_text(row, ST_STATE, state_of(&sp, row->nulls[ST_MAINT_FAILURES] ? 0 : DatumGetInt32(row->v[ST_MAINT_FAILURES])));
	}
	relation_close(r.rel, AccessShareLock);
	return true;
}

static void
row_unreadable(StatusRow *row, const Candidate *c, const char *message)
{
	row_begin(row, c);
	put_text(row, ST_STATE, "invalid");
	put_text(row, ST_FORMAT_NOTE, psprintf("unreadable: %s", message));
}

/* One relation in a subtransaction of its own: an error rolls back what it took and is the row; a cancel goes on */
static bool
status_guarded(const Candidate *c, StatusRow *row)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	volatile bool emit = false;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);
	PG_TRY();
	{
		emit = status_one(c, row);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED)
			ReThrowError(edata);
		row_unreadable(row, c, edata->message);
		FreeErrorData(edata);
		emit = true;
	}
	PG_END_TRY();
	return emit;
}

/* The relation as a candidate: an index of sift or sift_col, or a sift_table; NULL for any other */
static Candidate *
candidate_of(const Form_pg_class cls)
{
	Candidate  *c;
	SiftApiAm	am;
	Oid			heap;
	bool		valid;
	bool		index = cls->relkind == RELKIND_INDEX;

	if (!(index || cls->relkind == RELKIND_RELATION || cls->relkind == RELKIND_MATVIEW) || !sift_api_am_by_oid(cls->relam, &am))
		return NULL;
	if (index == (am == SIFT_API_TABLE))
		return NULL;
	heap = cls->oid;
	valid = true;
	if (index)
	{
		HeapTuple	itup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(cls->oid));

		if (!HeapTupleIsValid(itup))
			return NULL;
		heap = ((Form_pg_index) GETSTRUCT(itup))->indrelid;
		valid = ((Form_pg_index) GETSTRUCT(itup))->indisvalid && ((Form_pg_index) GETSTRUCT(itup))->indisready;
		ReleaseSysCache(itup);
	}
	c = palloc0(sizeof(Candidate));
	c->relid = cls->oid;
	c->am = am;
	c->heap = heap;
	c->valid = valid;
	return c;
}

static List *
one_candidate(Oid relid)
{
	HeapTuple	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	Candidate  *c;

	if (!HeapTupleIsValid(tup))
		return NIL;
	c = candidate_of((Form_pg_class) GETSTRUCT(tup));
	ReleaseSysCache(tup);
	return c == NULL ? NIL : list_make1(c);
}

static List *
all_candidates(void)
{
	Relation	pg_class = table_open(RelationRelationId, AccessShareLock);
	TableScanDesc scan = table_beginscan_catalog(pg_class, 0, NULL);
	HeapTuple	tup;
	List	   *found = NIL;

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Candidate  *c = candidate_of((Form_pg_class) GETSTRUCT(tup));

		if (c != NULL)
			found = lappend(found, c);
	}
	table_endscan(scan);
	table_close(pg_class, AccessShareLock);
	return found;
}

Datum
sift_status(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *found;

	InitMaterializedSRF(fcinfo, 0);
	sift_api_check_desc(rsi->setDesc, status_types, ST_NCOLS);
	found = PG_ARGISNULL(0) ? all_candidates() : one_candidate(PG_GETARG_OID(0));
	foreach_ptr(Candidate, c, found)
	{
		StatusRow	row;

		if (status_guarded(c, &row))
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, row.v, row.nulls);
	}
	return (Datum) 0;
}
