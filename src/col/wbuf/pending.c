/*
 * col/wbuf/pending.c - the rows an index holds back before they reach a segment: the table
 * of pending rows by index, their flush to the pages, the seal and the
 * flags a vacuum or a promotion reads
 */
#include "postgres.h"
#include "access/relation.h"
#include "utils/relfilenumbermap.h"
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

typedef struct PendIndex
{
	Oid			index;			/* InvalidOid: free */
	Oid			tablespace;		/* pg_class.reltablespace of the index, with loc the key of its file when the rows were buffered */
	RelFileLocator loc;
	uint32		ovf;
	uint32		jnull;
	StringInfoData buf;			/* per row: tid, payload length, payload */
} PendIndex;

static PendIndex pend[PEND_SLOTS];

static int	pend_used;

static StringInfoData pend_enc;

static void
pend_clear(PendIndex *p)
{
	p->index = InvalidOid;
	p->ovf = p->jnull = 0;
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
		ItemPointerData tid;
		uint32		len;
		Size		room;

		for (;;)
		{
			if (h.gen == 0)
				sift_lane_refresh(index, hash, "column definitions", HDRSIZE, SIFT_COL_FORMAT_KINDS, sift_col_maint_make_room, &h.gen, &h.hdr, h.tail);
			if (BufferIsValid(tailbuf = sift_lane_lock_tail(index, h.gen, h.tail, SIFT_LANES, &lane)))
				break;
			h.gen = 0;
		}
		memcpy(&tid, cur, sizeof(tid));
		memcpy(&len, cur + sizeof(tid), sizeof(len));
		room = TYPEALIGN_DOWN(4, SiftLaneRoom(BufferGetPage(tailbuf)));
		if (RECSIZE(len) > room)
		{
			BlockNumber tail;

			/* a record that does not fit what is left of the tail page: as a row that goes in at once (a new page, pieces) */
			crossed |= wbuf_write_row(index, &h, lane, tailbuf, &tid, cur + PEND_HEAD, len, &tail);
			h.tail[lane] = tail;
			cur += PEND_HEAD + len;
		}
		else
		{
			SiftPageEdit ed;

			sift_page_edit_begin(&ed, tailbuf, SIFT_PAGE_DATA_OFFSET + HDRSIZE);
			do
			{
				wbuf_put_piece(ed.page, &tid, false, cur + PEND_HEAD, len);
				room -= RECSIZE(len);
				cur += PEND_HEAD + len;
				if (cur >= rows + total)
					break;
				memcpy(&tid, cur, sizeof(tid));
				memcpy(&len, cur + sizeof(tid), sizeof(len));
			} while (RECSIZE(len) <= room);
			sift_page_edit_end(index, &ed, 1);
			h.tail[lane] = BufferGetBlockNumber(tailbuf);
			UnlockReleaseBuffer(tailbuf);
		}
	}
	/* the reads above can reload the relcache entry: keep the hint in the entry as it is now */
	sift_col_rel(index)->hint = h;
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
sift_cwbuf_pend(Relation index, SiftCWbufHint *h, const SiftColSchema *schema, const SiftColRow *row, bool *crossed)
{
	PendIndex  *p;
	uint32		ovf,
				jnull,
				len;

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
		sift_lane_refresh(index, schema->hash, "column definitions", HDRSIZE, SIFT_COL_FORMAT_KINDS, sift_col_maint_make_room, &h->gen, &h->hdr, h->tail);
	wbuf_flag_overflow(index, h, ovf, jnull);
	appendBinaryStringInfo(&p->buf, (const char *) &row->tid, sizeof(ItemPointerData));
	appendBinaryStringInfo(&p->buf, (const char *) &len, sizeof(len));
	appendBinaryStringInfo(&p->buf, pend_enc.data, len);
	p->ovf |= ovf;
	p->jnull |= jnull;
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

uint64
sift_cwbuf_seal(Relation index, bool force)
{
	wbuf_flush_for_read(index);
	return sift_lane_seal(index, force, HDRSIZE, SIFT_COL_FORMAT_KINDS, wbuf_limit_pages(index), has_rows);
}

/* VACUUM seals an open buffer once it holds a quarter of write_buffer_limit: its rows are promoted and merged with the rest instead of waiting for a seal that may never come */
uint64
sift_cwbuf_seal_vacuum(Relation index)
{
	wbuf_flush_for_read(index);
	return sift_lane_seal(index, false, HDRSIZE, SIFT_COL_FORMAT_KINDS, Max(wbuf_limit_pages(index) / 4, 2 * SIFT_LANES), has_rows);
}

void
sift_cwbuf_flag_sealed(Relation index, const SiftSegEntry *entry)
{
	sift_lane_flag_sealed(index, entry);
}
