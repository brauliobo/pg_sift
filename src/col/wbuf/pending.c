/*
 * col/wbuf/pending.c - the rows an index holds back before they reach a segment: the table
 * of pending rows by index, their flush to the pages, the seal and the
 * flags a vacuum or a promotion reads
 */
#include "postgres.h"
#include "access/relation.h"
#include "utils/relfilenumbermap.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "col/am.h"
#include "col/wbuf/wbuf_int.h"

/*
 * Rows a backend has appended after the first of its transaction wait here, per index (PEND_SLOTS of them), and go to a lane in runs: a run is the
 * rows that fit the tail page, written by one edit, so one WAL record a page and not one a row. They are written before the commit record (am.c:
 * pre-commit and pre-prepare, and when the next savepoint starts: a savepoint that rolls back drops what it buffered), and before this backend reads
 * the index (every reader below starts with flush_for_read): no other backend can see them before then, the heap rows are not visible to it.
 * Their overflow leaves are flagged when they are buffered, as for a row that goes in at once, so a plan made after the statement knows them.
 *
 * A slot belongs to the storage of the index (its file), not to the name: a tool that swaps the files of two tables and of their indexes in the
 * transaction (pg_repack, when the copy takes the place of the table) leaves the rows of a heap with the index that now has the file they were
 * meant for, and the OID they were buffered under has the other one. The OID is kept for a rebuild of that index, which drops them.
 */
#define PEND_SLOTS		8

#define PEND_HEAD		(sizeof(ItemPointerData) + sizeof(uint32))

/* the top bits of the stored length: the payload starts with the inserter's xid (SIFT_COLR_XMIN), and is that alone (SIFT_COLR_REF). The head keeps its size, so a buffer flushes at the same rows with or without track_changes */
#define PEND_XMIN		0x80000000u

#define PEND_REF		0x40000000u

#define PEND_FLAGS		(PEND_XMIN | PEND_REF)

static inline uint16
pend_flags(uint32 stored)
{
	return ((stored & PEND_XMIN) ? SIFT_COLR_XMIN : 0) | ((stored & PEND_REF) ? SIFT_COLR_REF : 0);
}

typedef struct PendIndex
{
	Oid			index;			/* InvalidOid: free */
	Oid			tablespace;		/* pg_class.reltablespace of the index, with loc the key of its file when the rows were buffered */
	RelFileLocator loc;
	uint32		ovf;
	uint32		jnull;
	bool		ref;			/* some row is a REF record: the buffer it goes to is marked before it is written */
	StringInfoData buf;			/* per row: tid, payload length (with PEND_FLAGS), payload */
} PendIndex;

static PendIndex pend[PEND_SLOTS];

static int	pend_used;

static StringInfoData pend_enc;

static void
pend_clear(PendIndex *p)
{
	p->index = InvalidOid;
	p->ovf = p->jnull = 0;
	p->ref = false;
	resetStringInfo(&p->buf);
	pend_used--;
}

/* Does an index (this one is a sift_col index, or none) have the file of the slot? */
static Oid
pend_file_index(const PendIndex *p)
{
	return RelidByRelfilenumber(p->tablespace, p->loc.relNumber);
}

/*
 * The index that has the file of a slot that holds rows now (its OID: the same as when they were buffered unless the files were swapped); false
 * when none is left. A slot whose file no index has is the one of an index that was dropped (the rows are in a heap that is gone with it, or is not
 * indexed), or whose file was replaced: a rebuild (build.c) discarded the slot, so what is left is a copy of the pages, which holds none of the
 * rows (ALTER INDEX SET TABLESPACE), and the slot goes with the OID it was buffered under.
 */
bool
sift_cwbuf_pending(Oid *index)
{
	for (int i = 0; i < PEND_SLOTS; i++)
		if (OidIsValid(pend[i].index))
		{
			/*
			 * pg_squeeze applies the changes it decodes while the decoding session reads the catalogs as of the past: the copy it buffered for is not in them and no
			 * file has an owner there, so neither says the slot is dead (the rows are lost if it is cleared). The slot is flushed under the OID it was buffered under.
			 */
			if (HistoricSnapshotActive())
			{
				*index = pend[i].index;
				return true;
			}
			*index = pend_file_index(&pend[i]);
			if (OidIsValid(*index))
				return true;
			if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(pend[i].index)))
			{
				*index = pend[i].index;
				return true;
			}
			pend_clear(&pend[i]);
		}
	return false;
}

/* The slot of the file of the index, else the one buffered under its OID whose file no index has now (the pages were copied to another file): it follows the index */
static PendIndex *
pend_find(Relation index)
{
	for (int i = 0; i < PEND_SLOTS; i++)
		if (OidIsValid(pend[i].index) && RelFileLocatorEquals(pend[i].loc, index->rd_locator))
			return &pend[i];
	for (int i = 0; i < PEND_SLOTS; i++)
		if (pend[i].index == RelationGetRelid(index) && !OidIsValid(pend_file_index(&pend[i])))
		{
			pend[i].tablespace = index->rd_rel->reltablespace;
			pend[i].loc = index->rd_locator;
			return &pend[i];
		}
	return NULL;
}

void
sift_cwbuf_discard(Oid index)
{
	for (int i = 0; i < PEND_SLOTS; i++)
		if (OidIsValid(pend[i].index) && (!OidIsValid(index) || pend[i].index == index))
			pend_clear(&pend[i]);
}
#ifdef USE_ASSERT_CHECKING
/*
 * The invariant of the buffered rows (am.c, batch_xact): the heap row of each is alive or dead for good only after the index has its entry. Nothing
 * frees the slot of an in-progress row; the rows of an aborted savepoint are dropped, and an ON CONFLICT statement does not buffer, since the one row
 * that dies before its transaction ends is a speculative insertion that lost. A slot found unused here means VACUUM freed it first: the entry
 * would point at whatever takes the slot.
 */
static void
check_slots_in_use(Relation index, const char *rows, Size total)
{
	/* the transaction may not hold the heap (a swap of storage moved the rows under a table it never locked): the lock is kept to the end of the transaction */
	Relation	heap = relation_open(index->rd_index->indrelid, AccessShareLock);
	Buffer		buf = InvalidBuffer;

	for (const char *cur = rows; cur < rows + total;)
	{
		ItemPointerData tid;
		uint32		len;

		memcpy(&tid, cur, sizeof(tid));
		memcpy(&len, cur + sizeof(tid), sizeof(len));
		len &= ~PEND_FLAGS;
		if (!BufferIsValid(buf) || BufferGetBlockNumber(buf) != ItemPointerGetBlockNumber(&tid))
		{
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			buf = ReadBuffer(heap, ItemPointerGetBlockNumber(&tid));
			LockBuffer(buf, BUFFER_LOCK_SHARE);
		}
		Assert(ItemIdIsUsed(PageGetItemId(BufferGetPage(buf), ItemPointerGetOffsetNumber(&tid))));
		cur += PEND_HEAD + len;
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
	relation_close(heap, NoLock);
}
#endif

/* A pending row as the record it is written as: its values go to the arena of the buffer the row is written to (a row the arena has no room for loses REF) */
typedef struct PendRec
{
	ItemPointerData tid;
	uint16		flags;
	Size		stored;			/* bytes the row takes in the pending buffer after its head */
	Size		len;			/* bytes of the payload of its record */
	const char *payload;
} PendRec;

/* The record of the pending row at cur. Puts to the arena, which may make a segment of shared memory: never inside the critical section of a page edit */
static PendRec
pend_rec(Relation index, const SiftCWbufHint *h, const char *cur, bool *crossed)
{
	PendRec		r = {.payload = cur + PEND_HEAD};
	uint32		stored;

	memcpy(&r.tid, cur, sizeof(r.tid));
	memcpy(&stored, cur + sizeof(r.tid), sizeof(stored));
	r.flags = pend_flags(stored);
	r.stored = r.len = stored & ~PEND_FLAGS;
	*crossed |= wbuf_ref_to_arena(index, h->gen, &r.tid, r.payload, &r.len, &r.flags);
	return r;
}

#define PEND_BATCH	(BLCKSZ / sizeof(SiftColRecHead))	/* records of a page at most */

/* Writes the rows of a slot to the lanes of the index and frees the slot; true when a row crossed write_buffer_limit */
static bool
pend_flush(Relation index, PendIndex *p)
{
	uint32		hash = sift_col_rel(index)->schema->hash;
	SiftCWbufHint h = sift_col_rel(index)->hint;
	Size		total = p->buf.len;
	char	   *rows = palloc(total);
	const char *cur = rows;
	bool		crossed = false;
	bool		ref = p->ref;
	PendRec    *batch = palloc(PEND_BATCH * sizeof(PendRec));
	PendRec		next;
	uint64		next_gen = 0;

	memcpy(rows, p->buf.data, total);
#ifdef USE_ASSERT_CHECKING
	check_slots_in_use(index, rows, total);
#endif
	wbuf_flag_overflow(index, &h, p->ovf, p->jnull);
	pend_clear(p);
	while (cur < rows + total)
	{
		Buffer		tailbuf;
		int			lane;
		Size		room;
		int			n = 1;

		for (;;)
		{
			if (h.gen == 0)
				sift_lane_refresh(index, hash, "column definitions", HDRSIZE, SIFT_COL_FORMAT_KINDS, SIFT_SEG_ROOTS_NOW, sift_col_maint_make_room, &h.gen, &h.hdr, h.tail);
			if (ref)
				wbuf_flag_ref(index, &h);
			if (BufferIsValid(tailbuf = sift_lane_lock_tail(index, h.gen, h.tail, SIFT_LANES, &lane)))
				break;
			h.gen = 0;
		}
		batch[0] = next_gen == h.gen ? next : pend_rec(index, &h, cur, &crossed);
		next_gen = 0;
		room = TYPEALIGN_DOWN(4, SiftLaneRoom(BufferGetPage(tailbuf)));
		if (RECSIZE(batch[0].len) > room)
		{
			BlockNumber tail;

			/* a record that does not fit what is left of the tail page: as a row that goes in at once (a new page, pieces) */
			crossed |= wbuf_write_row(index, &h, lane, tailbuf, &batch[0].tid, batch[0].flags, batch[0].payload, batch[0].len, &tail);
			h.tail[lane] = tail;
			cur += PEND_HEAD + batch[0].stored;
		}
		else
		{
			SiftPageEdit ed;
			const char *end = cur + PEND_HEAD + batch[0].stored;

			room -= RECSIZE(batch[0].len);
			/* the records that fit the page are made before the edit, whose critical section allows no allocation */
			while (end < rows + total)
			{
				PendRec		r = pend_rec(index, &h, end, &crossed);

				if (RECSIZE(r.len) > room)
				{
					next = r;
					next_gen = h.gen;
					break;
				}
				batch[n++] = r;
				room -= RECSIZE(r.len);
				end += PEND_HEAD + r.stored;
			}
			sift_page_edit_begin(&ed, tailbuf, SIFT_PAGE_DATA_OFFSET + HDRSIZE);
			for (int i = 0; i < n; i++)
				wbuf_put_piece(ed.page, &batch[i].tid, batch[i].flags, batch[i].payload, batch[i].len);
			sift_page_edit_end(index, &ed, 1);
			h.tail[lane] = BufferGetBlockNumber(tailbuf);
			UnlockReleaseBuffer(tailbuf);
			cur = end;
		}
	}
	/* the reads above can reload the relcache entry: keep the hint in the entry as it is now */
	sift_col_rel(index)->hint = h;
	pfree(batch);
	pfree(rows);
	return crossed;
}

bool
sift_cwbuf_flush(Relation index)
{
	PendIndex  *p = pend_find(index);

	return p != NULL && pend_flush(index, p);
}

void
wbuf_flush_for_read(Relation index)
{
	if (pend_used > 0)
		(void) sift_cwbuf_flush(index);
}

/*
 * Buffers a row of the index; false when the index has no slot (the caller appends the row at once). *crossed: a flush that the row made
 * (a page of rows is buffered) crossed write_buffer_limit; the hint is the caller's, as for sift_cwbuf_append.
 */
bool
sift_cwbuf_pend(Relation index, SiftCWbufHint *h, const SiftColSchema *schema, const SiftColRow *row, TransactionId xmin, bool ref, bool *crossed)
{
	PendIndex  *p;
	uint32		ovf,
				jnull,
				len;
	uint16		flags = wbuf_rec_flags(xmin, ref);
	uint32		stored;

	*crossed = false;
	if (pend_enc.data == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		initStringInfo(&pend_enc);
		if (pend[0].buf.data == NULL)
			for (int i = 0; i < PEND_SLOTS; i++)
				initStringInfo(&pend[i].buf);
		MemoryContextSwitchTo(old);
	}
	resetStringInfo(&pend_enc);
	if (flags & SIFT_COLR_XMIN)
		appendBinaryStringInfo(&pend_enc, (const char *) &xmin, sizeof(xmin));
	ovf = wbuf_encode_row(&pend_enc, schema, row, &jnull);
	len = pend_enc.len;
	/* a row of several pages goes in as pieces, at once; its scratch space does not stay allocated */
	if (RECSIZE(len) > EMPTY_ROOM)
	{
		if (pend_enc.maxlen > EMPTY_ROOM * 4)
		{
			pfree(pend_enc.data);
			pend_enc.data = NULL;
		}
		return false;
	}
	if ((p = pend_find(index)) == NULL)
	{
		if (pend_used == PEND_SLOTS)
			return false;
		for (p = pend; OidIsValid(p->index); p++)
			;
		p->index = RelationGetRelid(index);
		p->tablespace = index->rd_rel->reltablespace;
		p->loc = index->rd_locator;
		pend_used++;
	}
	if (h->gen == 0)
		sift_lane_refresh(index, schema->hash, "column definitions", HDRSIZE, SIFT_COL_FORMAT_KINDS, SIFT_SEG_ROOTS_NOW, sift_col_maint_make_room, &h->gen, &h->hdr, h->tail);
	wbuf_flag_overflow(index, h, ovf, jnull);
	enlargeStringInfo(&p->buf, PEND_HEAD + len);
	memcpy(p->buf.data + p->buf.len, &row->tid, sizeof(ItemPointerData));
	stored = len | ((flags & SIFT_COLR_XMIN) ? PEND_XMIN : 0) | ((flags & SIFT_COLR_REF) ? PEND_REF : 0);
	memcpy(p->buf.data + p->buf.len + sizeof(ItemPointerData), &stored, sizeof(stored));
	memcpy(p->buf.data + p->buf.len + PEND_HEAD, pend_enc.data, len);
	p->buf.len += PEND_HEAD + len;
	p->buf.data[p->buf.len] = '\0';
	p->ovf |= ovf;
	p->jnull |= jnull;
	p->ref |= (flags & SIFT_COLR_REF) != 0;
	if (p->buf.len >= EMPTY_ROOM)
	{
		sift_col_rel(index)->hint = *h;
		*crossed = pend_flush(index, p);
		*h = sift_col_rel(index)->hint;
	}
	return true;
}

uint64
wbuf_count_rows(Relation index, const SiftSegEntry *entry, bool first_only)
{
	SiftLaneIter it;
	Page		page = palloc(BLCKSZ);
	uint64		n = 0;

	sift_lane_iter_init(&it, index, entry);
	while ((!first_only || n == 0) && sift_lane_iter_next(&it, page, HDRSIZE))
		n += SiftLaneHead(page)->nrecs;
	pfree(page);
	return n;
}

static bool
has_rows(Relation index, const SiftSegEntry *entry)
{
	return wbuf_count_rows(index, entry, true) > 0;
}

/* A buffer of REF records is sealed by the bytes of its rows in the arena (its pages hold 16 bytes a row), the limit of one of values by its pages */
static uint32
seal_limit(Relation index)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	uint32		limit = wbuf_limit_pages(index);

	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == SIFT_SEG_WBUF_OPEN && wbuf_arena_full(index, snap->segs[i].gen, (Size) limit * BLCKSZ))
			limit = 0;
	pfree(snap);
	return limit;
}

uint64
sift_cwbuf_seal(Relation index, bool force)
{
	wbuf_flush_for_read(index);
	return sift_lane_seal(index, force, HDRSIZE, SIFT_COL_FORMAT_KINDS, SIFT_SEG_ROOTS_NOW, seal_limit(index), has_rows);
}

uint64
sift_cwbuf_rotate(Relation index)
{
	wbuf_flush_for_read(index);
	return sift_lane_rotate(index, HDRSIZE, SIFT_COL_FORMAT_KINDS, SIFT_SEG_ROOTS_NOW);
}

/* A quarter of a unit: rows this few are not worth a segment of their own for a reader of the units, but a buffer that holds them is read row by row by every probe */
static bool
quarter_unit(Relation index, const SiftSegEntry *entry)
{
	return wbuf_count_rows(index, entry, false) >= SIFT_COL_UNIT_MAX_ROWS / 4;
}

/*
 * VACUUM seals an open buffer once it holds a quarter of write_buffer_limit or a quarter of a unit of rows (a point column's row is a few bytes: 45,000 of them stay under the pages
 * and every probe of a join reads each one): its rows are promoted and merged with the rest instead of waiting for a seal that may never come
 */
uint64
sift_cwbuf_seal_vacuum(Relation index)
{
	uint64		gen;

	wbuf_flush_for_read(index);
	gen = sift_lane_seal(index, false, HDRSIZE, SIFT_COL_FORMAT_KINDS, SIFT_SEG_ROOTS_NOW, Max(wbuf_limit_pages(index) / 4, 2 * SIFT_LANES), has_rows);
	return gen ? gen : sift_lane_seal(index, true, HDRSIZE, SIFT_COL_FORMAT_KINDS, SIFT_SEG_ROOTS_NOW, 0, quarter_unit);
}

void
sift_cwbuf_flag_sealed(Relation index, const SiftSegEntry *entry)
{
	sift_lane_flag_sealed(index, entry);
}
