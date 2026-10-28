/*
 * col/wbuf.c - write buffers of a column index: aminsert appends one row record to one of
 * SIFT_LANES lane chains of the active segment, each append one generic WAL record per page touched. The lane machinery (creation, tail
 * locking, growth, sealing, walks) is core/lane.c's, shared with the BM25 doc records of bm25/wbuf.c; this file is the record.
 *
 * A record is SiftColRecHead and `len` payload bytes, 4-byte aligned. The payload of a row, for every top level leaf in id order, is a flag
 * byte (COLV_NULL, COLV_OVERFLOW) and, unless NULL, the value: INT a zigzag varint, F4 and F8 their raw bytes, STR a varint length and the
 * bytes, GROUP the varint element count (the high nibble of the flag byte of an overflow value is its JSON type, SIFT_COLTAG_*). The
 * elements of the children follow, child by child (id order), count values each, every value with its own flag byte. A row that does not fit the tail page is split in pieces of its payload (first without
 * SIFT_COLR_CONT, the rest with it) written while the tail is held, lock coupling to the new page, so a row is always adjacent in one lane.
 * A writer that dies between two pieces leaves a torn row (pieces without the last one): it never committed, readers skip it.
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "common/hashfn.h"
#include "access/relation.h"
#include "common/int.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/sinval.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/wbuf.h"
#include "col/pg.h"
#include "col/point.h"
#include "core/lane.h"
#include "core/varint.h"

#define SIFT_COLR_DELETED	0x0001
#define SIFT_COLR_CONT		0x0002	/* more payload of the preceding row: not counted */

#define COLV_NULL			0x01
#define COLV_OVERFLOW		0x02
#define COLV_TAG_SHIFT		4			/* the high nibble of a value's flag byte: its SIFT_COLTAG_* (an overflow value's type, a kind; 0 in what 3.0 wrote, which ignores it) */

typedef struct SiftColRecHead
{
	ItemPointerData tid;
	uint16		flags;
	uint32		len;			/* payload bytes of this piece */
} SiftColRecHead;

StaticAssertDecl(sizeof(SiftColRecHead) == 12, "the record head is 12 bytes");

#define HDRSIZE			sizeof(SiftLanePageHead)
#define RECSIZE(len)	TYPEALIGN(4, sizeof(SiftColRecHead) + (Size) (len))
#define EMPTY_ROOM		(SIFT_PAGE_PAYLOAD - HDRSIZE)

#ifdef SIFT_TESTING
int			sift_cwbuf_fail_grow = 0;
#endif

static uint32
limit_pages(Relation index)
{
	return Max(2 * SIFT_LANES, (uint64) sift_col_options(index)->write_buffer_limit * 1024 / BLCKSZ);
}

/* The records of a page copy: the cursor and the checked record at it */
typedef struct PageCursor
{
	Relation	index;
	Page		page;
	Size		pos;			/* record offset in the page */
	Size		used;
} PageCursor;

static void
cursor_set(PageCursor *c, Page page)
{
	c->page = page;
	c->pos = 0;
	c->used = SiftPageGetUsed(page) - HDRSIZE;
}

/* The record at c->pos, checked; *size its length */
static const SiftColRecHead *
rec_at(PageCursor *c, Size *size)
{
	const SiftColRecHead *h = (const SiftColRecHead *) (SiftLaneRecs(c->page, HDRSIZE) + c->pos);

	if (c->pos + sizeof(SiftColRecHead) > c->used)
		SIFT_CORRUPT(c->index, "A record header at byte %zu overruns the %zu used bytes of a write buffer page.", c->pos, c->used);
	*size = RECSIZE(h->len);
	if (c->pos + *size > c->used)
		SIFT_CORRUPT(c->index, "A record of %zu bytes at byte %zu overruns the %zu used bytes of a write buffer page.",
				*size, c->pos, c->used);
	return h;
}

static void
put_value(StringInfo buf, const SiftColDef *def, const SiftColValue *v)
{
	uint8		flag = (v->isnull || v->overflow ? COLV_NULL : 0) | (v->overflow ? COLV_OVERFLOW : 0) | v->tag << COLV_TAG_SHIFT;

	appendStringInfoChar(buf, flag);
	if (flag & COLV_NULL)
		return;
	if (def->kind == SIFT_COLK_GROUP)
	{
		Assert(v->v.i >= 0);
		sift_col_put_varint(buf, v->v.i);
		return;
	}
	switch (def->store)
	{
		case SIFT_COLS_INT:
			sift_col_put_varint(buf, sift_zigzag(v->v.i));
			break;
		case SIFT_COLS_F4:
			{
				float4		f = (float4) v->v.f;

				appendBinaryStringInfo(buf, (char *) &f, sizeof(f));
				break;
			}
		case SIFT_COLS_F8:
			appendBinaryStringInfo(buf, (char *) &v->v.f, sizeof(double));
			break;
		case SIFT_COLS_STR:
			sift_col_put_varint(buf, v->len);
			appendBinaryStringInfo(buf, v->v.s, v->len);
			break;
	}
}

/* Element count of a group leaf's value in a row (absent array: none) */
static inline uint64
elem_count(const SiftColValue *v)
{
	return v->isnull ? 0 : v->v.i;
}

/* is the slot a JSON null (a NULL that is not a missing key)? */
static inline bool
value_jnull(const SiftColValue *v)
{
	return v->isnull && !v->overflow && v->tag == SIFT_COLTAG_JNULL;
}

/* Appends the row's payload; returns the leaves (SIFT_COL_OVF_BIT) it holds an overflow value of, and in *jnull those it holds a JSON null of */
static uint32
encode_row(StringInfo buf, const SiftColSchema *schema, const SiftColRow *row, uint32 *jnull)
{
	uint32		ovf = 0;

	*jnull = 0;
	for (int id = 0; id < schema->ncols; id++)
		if (schema->cols[id].parent < 0)
		{
			put_value(buf, &schema->cols[id], &row->val[id]);
			ovf |= row->val[id].overflow ? SIFT_COL_OVF_BIT(id) : 0;
			*jnull |= value_jnull(&row->val[id]) ? SIFT_COL_OVF_BIT(id) : 0;
		}
	for (int id = 0; id < schema->ncols; id++)
	{
		const SiftColDef *def = &schema->cols[id];

		if (def->parent >= 0)
			for (uint64 j = 0, n = elem_count(&row->val[def->parent]); j < n; j++)
			{
				put_value(buf, def, &row->elem[id][j]);
				ovf |= row->elem[id][j].overflow ? SIFT_COL_OVF_BIT(id) : 0;
				*jnull |= value_jnull(&row->elem[id][j]) ? SIFT_COL_OVF_BIT(id) : 0;
			}
	}
	return ovf;
}

static void
put_piece(Page page, const ItemPointerData *tid, bool cont, const char *payload, Size len)
{
	Size		used = SiftPageGetUsed(page),
				size = RECSIZE(len);
	SiftColRecHead *h = (SiftColRecHead *) (SiftPageGetData(page) + used);

	h->tid = *tid;
	h->flags = cont ? SIFT_COLR_CONT : 0;
	h->len = len;
	memcpy(h + 1, payload, len);
	memset((char *) (h + 1) + len, 0, size - sizeof(SiftColRecHead) - len);
	if (!cont)
		SiftLaneHead(page)->nrecs++;
	SiftPageSetUsed(page, used + size);
}

/* The payload bytes of a piece that fit room: all rem when the record does */
static Size
piece_fit(Size room, Size rem)
{
	Size		avail = TYPEALIGN_DOWN(4, room);

	return avail > sizeof(SiftColRecHead) ? Min(rem, avail - sizeof(SiftColRecHead)) : 0;
}

typedef struct PieceArg
{
	const ItemPointerData *tid;
	bool		cont;
	const char *payload;
	Size		len;
	bool		spans;			/* the row goes on in the next page */
} PieceArg;

static void
fill_piece(Page cp, Page np, Page hp, void *arg)
{
	PieceArg   *a = arg;

	put_piece(cp, a->tid, a->cont, a->payload, a->len);
	if (a->spans)
		SiftLaneHead(cp)->flags |= SIFT_WBUFPAGE_SPANS;
}

/* Writes the row starting at the locked tail cur; returns whether the segment reached write_buffer_limit. Leaves the final tail block in *tail. */
static bool
write_row(Relation index, SiftCWbufHint *h, int lane, Buffer cur, const ItemPointerData *tid, const char *payload, Size len,
		  BlockNumber *tail)
{
	bool		crossed = false,
				first = true;
	Size		done = 0;
	uint32		limit = limit_pages(index);

	for (;;)
	{
		Size		rem = len - done,
					n = piece_fit(SiftLaneRoom(BufferGetPage(cur)), rem);
		bool		place;
		PieceArg	pa;

		if (n == rem)
		{
			SiftPageEdit ed;

			sift_page_edit_begin(&ed, cur, SIFT_PAGE_DATA_OFFSET + HDRSIZE);
			put_piece(ed.page, tid, !first, payload + done, n);
			sift_page_edit_end(index, &ed, 1);
			break;
		}
		/* a row that fits an empty page moves there whole rather than splitting */
		place = n > 0 && !(first && RECSIZE(len) <= EMPTY_ROOM);
		pa = (PieceArg) {tid, !first, payload + done, n, done + n < len};
		cur = sift_lane_grow(index, h->gen, h->hdr, lane, cur, HDRSIZE, limit, place ? fill_piece : NULL, &pa, &crossed);
#ifdef SIFT_TESTING
		if (sift_cwbuf_fail_grow > 0 && --sift_cwbuf_fail_grow == 0)
			elog(ERROR, "sift_col write buffer: injected failure after a page link");	/* check-errors: allow */
#endif
		if (place)
		{
			first = false;
			done += n;
		}
	}
	*tail = BufferGetBlockNumber(cur);
	UnlockReleaseBuffer(cur);
	return crossed;
}

typedef struct OvfArg
{
	uint64		gen;
	uint32		mask;
	uint32		jmask;
} OvfArg;

static void
or_overflow(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	const OvfArg *a = arg;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].gen == a->gen)
		{
			dir->segs[i].reserved |= a->mask;
			dir->segs[i].sum_dl |= a->jmask;
		}
}

uint32
sift_cwbuf_overflow(const SiftDirSnapshot *dir)
{
	uint32		mask = 0;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE)
			mask |= dir->segs[i].reserved;
	return mask;
}

uint32
sift_cwbuf_jnull(const SiftDirSnapshot *dir)
{
	uint32		mask = 0;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE)
			mask |= (uint32) dir->segs[i].sum_dl;
	return mask;
}

/*
 * The plans of the table are invalid, in every session, whether or not the transaction that wrote the value commits: the mask is in the
 * directory for good. The queued invalidation reaches this session at the end of the statement (a transaction that goes on to use a
 * prepared statement), the immediate one the others now.
 */
void
sift_cwbuf_invalidate_plans(Oid heapoid)
{
	SharedInvalidationMessage msg = {.rc = {.id = SHAREDINVALRELCACHE_ID,.dbId = MyDatabaseId,.relId = heapoid}};

	CacheInvalidateRelcacheByRelid(heapoid);
	SendSharedInvalidMessages(&msg, 1);
}

/* Adds the leaves of a row that overflow (mask) or hold a JSON null (jmask) to the entry of the write buffer it goes to; the plans of the table are invalid when that changes it */
static void
flag_overflow(Relation index, SiftCWbufHint *h, uint32 mask, uint32 jmask)
{
	SiftDirSnapshot *snap;

	if (h->ovf_gen != h->gen)
		h->ovf_known = h->jnull_known = 0;
	h->ovf_gen = h->gen;
	if ((mask & ~h->ovf_known) == 0 && (jmask & ~h->jnull_known) == 0)
		return;
	snap = palloc(sizeof(SiftDirSnapshot));
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].gen == h->gen)
		{
			if ((mask & ~snap->segs[i].reserved) || (jmask & ~(uint32) snap->segs[i].sum_dl))
			{
				sift_dir_update(index, &(SiftDirChange) {.fn = or_overflow,.arg = &(OvfArg) {h->gen, mask, jmask},.extra = InvalidBuffer});
				sift_cwbuf_invalidate_plans(index->rd_index->indrelid);
#ifdef SIFT_TESTING
				sift_col_test_inval(index);
#endif
			}
			h->ovf_known = snap->segs[i].reserved | mask;
			h->jnull_known = (uint32) snap->segs[i].sum_dl | jmask;
		}
	pfree(snap);
}

bool
sift_cwbuf_append(Relation index, SiftCWbufHint *h, const SiftColSchema *schema, const SiftColRow *row)
{
	StringInfoData buf;
	Buffer		tailbuf;
	int			lane;
	BlockNumber tail;
	bool		crossed;
	uint32		ovf,
				jnull;

	initStringInfo(&buf);
	ovf = encode_row(&buf, schema, row, &jnull);
	for (;;)
	{
		if (h->gen == 0)
			sift_lane_refresh(index, schema->hash, "column definitions", HDRSIZE, SIFT_COL_FORMAT_KINDS, sift_col_maint_make_room, &h->gen, &h->hdr, h->tail);
		flag_overflow(index, h, ovf, jnull);
		if (BufferIsValid(tailbuf = sift_lane_lock_tail(index, h->gen, h->tail, SIFT_LANES, &lane)))
			break;
		h->gen = 0;
	}
	crossed = write_row(index, h, lane, tailbuf, &row->tid, buf.data, buf.len, &tail);
	h->tail[lane] = tail;
	pfree(buf.data);
	return crossed;
}

/*
 * Rows a backend has appended after the first of its transaction wait here, per index (PEND_SLOTS of them), and go to a lane in runs: a run is the
 * rows that fit the tail page, written by one edit, so one WAL record a page and not one a row. They are written before the commit record (am.c:
 * pre-commit and pre-prepare, and when the next savepoint starts: a savepoint that rolls back drops what it buffered), and before this backend reads
 * the index (every reader below starts with flush_for_read): no other backend can see them before then, the heap rows are not visible to it.
 * Their overflow leaves are flagged when they are buffered, as for a row that goes in at once, so a plan made after the statement knows them.
 */
#define PEND_SLOTS		8
#define PEND_HEAD		(sizeof(ItemPointerData) + sizeof(uint32))

typedef struct PendIndex
{
	Oid			index;			/* InvalidOid: free */
	uint32		ovf;
	uint32		jnull;
	StringInfoData buf;			/* per row: tid, payload length, payload */
} PendIndex;

static PendIndex pend[PEND_SLOTS];
static int	pend_used;
static StringInfoData pend_enc;

Oid
sift_cwbuf_pending(void)
{
	for (int i = 0; i < PEND_SLOTS; i++)
		if (OidIsValid(pend[i].index))
			return pend[i].index;
	return InvalidOid;
}

static PendIndex *
pend_find(Oid index)
{
	for (int i = 0; i < PEND_SLOTS; i++)
		if (pend[i].index == index)
			return &pend[i];
	return NULL;
}

static void
pend_clear(PendIndex *p)
{
	p->index = InvalidOid;
	p->ovf = p->jnull = 0;
	resetStringInfo(&p->buf);
	pend_used--;
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
	Relation	heap = relation_open(index->rd_index->indrelid, NoLock);
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
	flag_overflow(index, &h, p->ovf, p->jnull);
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
			crossed |= write_row(index, &h, lane, tailbuf, &tid, cur + PEND_HEAD, len, &tail);
			h.tail[lane] = tail;
			cur += PEND_HEAD + len;
		}
		else
		{
			SiftPageEdit ed;

			sift_page_edit_begin(&ed, tailbuf, SIFT_PAGE_DATA_OFFSET + HDRSIZE);
			do
			{
				put_piece(ed.page, &tid, false, cur + PEND_HEAD, len);
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
	PendIndex  *p = pend_find(RelationGetRelid(index));

	return p != NULL && pend_flush(index, p);
}

static void
flush_for_read(Relation index)
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
	ovf = encode_row(&pend_enc, schema, row, &jnull);
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
	if ((p = pend_find(RelationGetRelid(index))) == NULL)
	{
		if (pend_used == PEND_SLOTS)
			return false;
		for (p = pend; OidIsValid(p->index); p++)
			;
		p->index = RelationGetRelid(index);
		pend_used++;
	}
	if (h->gen == 0)
		sift_lane_refresh(index, schema->hash, "column definitions", HDRSIZE, SIFT_COL_FORMAT_KINDS, sift_col_maint_make_room, &h->gen, &h->hdr, h->tail);
	flag_overflow(index, h, ovf, jnull);
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

static uint64
count_rows(Relation index, const SiftSegEntry *entry, bool first_only)
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
	return count_rows(index, entry, true) > 0;
}

uint64
sift_cwbuf_seal(Relation index, bool force)
{
	flush_for_read(index);
	return sift_lane_seal(index, force, HDRSIZE, SIFT_COL_FORMAT_KINDS, limit_pages(index), has_rows);
}

/* VACUUM seals an open buffer once it holds a quarter of write_buffer_limit: its rows are promoted and merged with the rest instead of waiting for a seal that may never come */
uint64
sift_cwbuf_seal_vacuum(Relation index)
{
	flush_for_read(index);
	return sift_lane_seal(index, false, HDRSIZE, SIFT_COL_FORMAT_KINDS, Max(limit_pages(index) / 4, 2 * SIFT_LANES), has_rows);
}

void
sift_cwbuf_flag_sealed(Relation index, const SiftSegEntry *entry)
{
	sift_lane_flag_sealed(index, entry);
}

struct SiftCWbufReader
{
	SiftSegEntry entry;
	const SiftColSchema *schema;
	MemoryContext mcxt;
	SiftLaneIter it;
	Page		page;			/* copy of the current page */
	PageCursor	cur;
	uint16		pflags;
	bool		torn;			/* next_rec skipped a row whose last piece is missing */
	/* payload of a row assembled from pieces */
	uint8	   *joined;
	Size		joinedlen;
	Size		joinedcap;
	/* the decoded row */
	SiftColValue *val;
	SiftColValue **elem;
	SiftColValue *evals;
	Size		evalcap;
};

/* A record header with its payload; valid until the next call */
typedef struct RawRec
{
	ItemPointerData tid;
	bool		deleted;
	const uint8 *payload;
	Size		len;
} RawRec;

SiftCWbufReader *
sift_cwbuf_open(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt)
{
	SiftCWbufReader *r;

	flush_for_read(index);
	r = MemoryContextAllocZero(mcxt, sizeof(SiftCWbufReader));
	r->entry = *entry;
	r->schema = schema;
	r->mcxt = mcxt;
	r->page = MemoryContextAlloc(mcxt, BLCKSZ);
	r->cur.index = index;
	r->cur.page = r->page;
	r->val = MemoryContextAllocZero(mcxt, schema->ncols * sizeof(SiftColValue));
	r->elem = MemoryContextAllocZero(mcxt, schema->ncols * sizeof(SiftColValue *));
	sift_lane_iter_init(&r->it, index, &r->entry);
	return r;
}

void
sift_cwbuf_close(SiftCWbufReader *r)
{
	if (r->joined)
		pfree(r->joined);
	if (r->evals)
		pfree(r->evals);
	pfree(r->val);
	pfree(r->elem);
	pfree(r->page);
	pfree(r);
}

static bool
advance_page(SiftCWbufReader *r)
{
	if (!sift_lane_iter_next(&r->it, r->page, HDRSIZE))
		return false;
	cursor_set(&r->cur, r->page);
	r->pflags = SiftLaneHead(r->page)->flags;
	return true;
}

static void
join_append(SiftCWbufReader *r, const uint8 *payload, Size len)
{
	if (r->joinedlen + len > r->joinedcap)
	{
		r->joinedcap = Max(2 * r->joinedcap, r->joinedlen + len);
		r->joined = r->joined ? repalloc(r->joined, r->joinedcap) : MemoryContextAlloc(r->mcxt, r->joinedcap);
	}
	memcpy(r->joined + r->joinedlen, payload, len);
	r->joinedlen += len;
}

/*
 * The row's first piece is the last record of a page that spans: appends the continuation pieces. False for a torn row (the writer died
 * before its last piece): the continuation is missing or what follows the pieces is a row of its own, left to be read next.
 */
static bool
join_pieces(SiftCWbufReader *r)
{
	while (r->cur.pos >= r->cur.used && (r->pflags & SIFT_WBUFPAGE_SPANS))
	{
		const SiftColRecHead *h;
		Size		size;

		if (!BlockNumberIsValid(r->it.nextblk))
			SIFT_CORRUPT(r->cur.index, "A page of write buffer %llu spans past the end of its lane.", (unsigned long long) r->entry.gen);
		advance_page(r);
		if (r->cur.pos >= r->cur.used)
			return false;
		h = rec_at(&r->cur, &size);
		if (!(h->flags & SIFT_COLR_CONT))
			return false;
		r->cur.pos += size;
		join_append(r, (const uint8 *) (h + 1), h->len);
	}
	return true;
}

/* The next row's record, pieces joined; false at the end of the segment */
static bool
next_rec(SiftCWbufReader *r, RawRec *rec)
{
	for (;;)
	{
		const SiftColRecHead *h;
		Size		size;

		if (r->cur.pos >= r->cur.used)
		{
			if (!advance_page(r))
				return false;
			continue;
		}
		h = rec_at(&r->cur, &size);
		if (h->flags & SIFT_COLR_CONT)
			SIFT_CORRUPT(r->cur.index, "A continuation record of segment %llu has no first piece.", (unsigned long long) r->entry.gen);
		r->cur.pos += size;
		rec->tid = h->tid;
		rec->deleted = (h->flags & SIFT_COLR_DELETED) != 0;
		rec->payload = (const uint8 *) (h + 1);
		rec->len = h->len;
		if (r->cur.pos < r->cur.used || !(r->pflags & SIFT_WBUFPAGE_SPANS))
			return true;
		r->joinedlen = 0;
		join_append(r, rec->payload, rec->len);
		if (join_pieces(r))
		{
			rec->payload = r->joined;
			rec->len = r->joinedlen;
			return true;
		}
		r->torn = true;
	}
}

#define NEED(r, p, end, n) \
	do { \
		if ((Size) ((end) - (p)) < (Size) (n)) \
			SIFT_CORRUPT((r)->cur.index, "A record of write buffer %llu ends inside a value.", (unsigned long long) (r)->entry.gen); \
	} while (0)

static const uint8 *
get_varint(SiftCWbufReader *r, const uint8 *p, const uint8 *end, uint64 *x)
{
	if ((p = sift_get_varint64_n(p, end, x)) == NULL)
		SIFT_CORRUPT(r->cur.index, "A record of write buffer %llu ends inside a varint.", (unsigned long long) r->entry.gen);
	return p;
}

static const uint8 *
get_value(SiftCWbufReader *r, const SiftColDef *def, const uint8 *p, const uint8 *end, SiftColValue *v)
{
	uint64		x;
	uint8		flag;

	NEED(r, p, end, 1);
	flag = *p++;
	*v = (SiftColValue) {0};
	v->isnull = (flag & (COLV_NULL | COLV_OVERFLOW)) != 0;
	v->overflow = (flag & COLV_OVERFLOW) != 0;
	v->tag = flag >> COLV_TAG_SHIFT;
	if (v->tag > SIFT_COLTAG_MAX || !sift_col_tag_valid(v))
		SIFT_CORRUPT(r->cur.index, "A record of write buffer %llu has the flag byte 0x%02x.", (unsigned long long) r->entry.gen, flag);
	if (r->entry.format < SIFT_COL_FORMAT_KINDS && sift_col_def_has_kinds(def) && v->tag == SIFT_COLTAG_UNKNOWN &&
		!v->overflow && (v->isnull || (def->flags & SIFT_COLD_ASTEXT)))
		v->tag = SIFT_COLTAG_UNSURE;
	if (v->isnull)
		return p;
	if (def->kind == SIFT_COLK_GROUP)
	{
		p = get_varint(r, p, end, &x);
		v->v.i = x;
		return p;
	}
	switch (def->store)
	{
		case SIFT_COLS_INT:
			p = get_varint(r, p, end, &x);
			v->v.i = sift_unzigzag(x);
			break;
		case SIFT_COLS_F4:
			{
				float4		f;

				NEED(r, p, end, sizeof(f));
				memcpy(&f, p, sizeof(f));
				v->v.f = f;
				p += sizeof(f);
				break;
			}
		case SIFT_COLS_F8:
			NEED(r, p, end, sizeof(double));
			memcpy(&v->v.f, p, sizeof(double));
			p += sizeof(double);
			break;
		case SIFT_COLS_STR:
			p = get_varint(r, p, end, &x);
			if (x > SIFT_COL_MAX_VALUE)
				SIFT_CORRUPT(r->cur.index, "A record of write buffer %llu holds a string of %llu bytes.", (unsigned long long) r->entry.gen,
							 (unsigned long long) x);
			v->len = x;
			NEED(r, p, end, v->len);
			v->v.s = (const char *) p;
			p += v->len;
			break;
	}
	return p;
}

/* Room for n element values; the decoded row is rebuilt on every call, so nothing is kept */
static void
evals_reserve(SiftCWbufReader *r, Size n)
{
	if (n <= r->evalcap)
		return;
	r->evalcap = Max(2 * r->evalcap, Max(n, 16));
	r->evals = r->evals ? repalloc(r->evals, r->evalcap * sizeof(SiftColValue)) :
		MemoryContextAlloc(r->mcxt, r->evalcap * sizeof(SiftColValue));
}

static void
decode_row(SiftCWbufReader *r, const RawRec *rec, SiftColRow *row)
{
	const SiftColSchema *s = r->schema;
	const uint8 *p = rec->payload,
			   *end = p + rec->len;
	uint64		total = 0;
	Size		off = 0;

	for (int id = 0; id < s->ncols; id++)
	{
		r->elem[id] = NULL;
		if (s->cols[id].parent < 0)
			p = get_value(r, &s->cols[id], p, end, &r->val[id]);
	}
	/* an element takes at least a byte: the counts cannot exceed what is left */
	for (int id = 0; id < s->ncols; id++)
		if (s->cols[id].parent >= 0)
		{
			uint64		n = elem_count(&r->val[s->cols[id].parent]);

			if (n > (uint64) (end - p))
				SIFT_CORRUPT(r->cur.index, "A record of write buffer %llu counts %llu elements in %zu bytes.",
						(unsigned long long) r->entry.gen, (unsigned long long) n, (Size) (end - p));
			total += n;
		}
	if (total > (uint64) (end - p))
		SIFT_CORRUPT(r->cur.index, "A record of write buffer %llu counts %llu elements in %zu bytes.",
				(unsigned long long) r->entry.gen, (unsigned long long) total, (Size) (end - p));
	evals_reserve(r, Max(total, 1));
	for (int id = 0; id < s->ncols; id++)
		if (s->cols[id].parent >= 0)
		{
			uint64		n = elem_count(&r->val[s->cols[id].parent]);

			r->elem[id] = r->evals + off;
			for (uint64 j = 0; j < n; j++)
				p = get_value(r, &s->cols[id], p, end, &r->evals[off + j]);
			off += n;
		}
	if (p != end)
		SIFT_CORRUPT(r->cur.index, "A record of write buffer %llu has %zu bytes past its last value.",
				(unsigned long long) r->entry.gen, (Size) (end - p));
	row->tid = rec->tid;
	row->ncols = s->ncols;
	row->val = r->val;
	row->elem = r->elem;
}

bool
sift_cwbuf_next(SiftCWbufReader *r, SiftColRow *row, bool *deleted)
{
	RawRec		rec;

	if (!next_rec(r, &rec))
		return false;
	decode_row(r, &rec, row);
	*deleted = rec.deleted;
	return true;
}

static int
tid_cmp(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

/*
 * The keys of one plain leaf of one write buffer, as the records of its lanes were read so far: a hash of the value (the point directory's hash for an integer,
 * PostgreSQL's for bytes) to the heap address. A lookup reads only the records appended since the last one of this backend, the lane chains being append-only
 * (VACUUM flags a record dead in place, which leaves a stale entry the heap check drops). The buffer is named by the file of the index and the generation of its segment.
 */
typedef struct WbufEnt
{
	uint64		h;
	ItemPointerData tid;
	uint32		next;			/* the next entry of the bucket + 1, 0 at its end */
} WbufEnt;

typedef struct WbufLane
{
	BlockNumber blk;			/* where the next read starts: InvalidBlockNumber before the first */
	Size		pos;
} WbufLane;

typedef struct WbufLeaf
{
	RelFileNumber rel;
	uint64		gen;
	int			col;
	uint64		used;			/* for the least recently used */
	WbufLane	lane[SIFT_LANES];
	uint32		n,
				cap,
				nbuckets;
	uint32	   *bucket;
	WbufEnt    *ent;
	uint32		nover,
				capover;
	ItemPointerData *over;		/* rows whose value the leaf could not store: any lookup keeps them */
} WbufLeaf;

#define WBUF_CACHE_LEAVES	16
#define WBUF_CACHE_ROWS		(UINT64CONST(1) << 22)	/* entries over all the leaves, 24 bytes each */

static MemoryContext wbuf_cxt;
static WbufLeaf *wbuf_leaves[WBUF_CACHE_LEAVES];
static uint64 wbuf_clock;

static void
leaf_free(WbufLeaf *lf)
{
	for (int i = 0; i < WBUF_CACHE_LEAVES; i++)
		if (wbuf_leaves[i] == lf)
			wbuf_leaves[i] = NULL;
	if (lf->bucket)
		pfree(lf->bucket);
	if (lf->ent)
		pfree(lf->ent);
	if (lf->over)
		pfree(lf->over);
	pfree(lf);
}

static uint64
leaf_hash(bool str, const SiftColValue *v)
{
	return str ? hash_bytes_extended((const unsigned char *) v->v.s, (int) v->len, 0) : sift_cpoint_hash(v->v.i);
}

static void
leaf_add(WbufLeaf *lf, uint64 h, ItemPointerData tid)
{
	uint32		b;

	if (lf->n == lf->cap)
	{
		lf->cap = Max(64, lf->cap * 2);
		lf->ent = lf->ent ? repalloc(lf->ent, lf->cap * sizeof(WbufEnt)) : MemoryContextAlloc(wbuf_cxt, lf->cap * sizeof(WbufEnt));
	}
	if (lf->n >= lf->nbuckets)
	{
		lf->nbuckets = Max(64, lf->nbuckets * 2);
		lf->bucket = lf->bucket ? repalloc(lf->bucket, lf->nbuckets * sizeof(uint32)) : MemoryContextAlloc(wbuf_cxt, lf->nbuckets * sizeof(uint32));
		memset(lf->bucket, 0, lf->nbuckets * sizeof(uint32));
		for (uint32 i = 0; i < lf->n; i++)
		{
			b = lf->ent[i].h & (lf->nbuckets - 1);
			lf->ent[i].next = lf->bucket[b];
			lf->bucket[b] = i + 1;
		}
	}
	b = h & (lf->nbuckets - 1);
	lf->ent[lf->n] = (WbufEnt) {.h = h,.tid = tid,.next = lf->bucket[b]};
	lf->bucket[b] = ++lf->n;
}

/* The records of lane `lane` appended since the last read of the leaf */
static void
leaf_read_lane(WbufLeaf *lf, SiftCWbufReader *r, const SiftColSchema *schema, int lane, BlockNumber head)
{
	bool		str = schema->cols[lf->col].store == SIFT_COLS_STR;
	WbufLane   *l = &lf->lane[lane];
	BlockNumber startblk = BlockNumberIsValid(l->blk) ? l->blk : head;
	Size		startpos = BlockNumberIsValid(l->blk) ? l->pos : 0;
	BlockNumber wblk = startblk;	/* the watermark: the start of the first row not read (a row whose last piece is not there yet), else the end of the lane */
	Size		wpos = startpos;
	bool		first = true;

	r->it.lane = SIFT_LANES;
	r->it.nextblk = startblk;
	for (;;)
	{
		RawRec		rec;
		BlockNumber rowblk;
		Size		rowpos;

		if (first || r->cur.pos >= r->cur.used)
		{
			if (!advance_page(r))
				break;
			if (first)
				r->cur.pos = startpos;
			first = false;
			continue;
		}
		rowblk = r->it.blk;
		rowpos = r->cur.pos;
		r->torn = false;
		if (!next_rec(r, &rec))
		{
			/* a torn row at the end of the lane may still be completed by its writer: read it again */
			if (r->torn)
			{
				wblk = rowblk;
				wpos = rowpos;
			}
			else
			{
				wblk = r->it.blk;
				wpos = r->cur.pos;
			}
			goto done;
		}
		/* a torn row with a complete row after it was left by a writer that died: skipped for good */
		if (!rec.deleted)
		{
			const uint8 *p = rec.payload,
					   *end = p + rec.len;

			for (int id = 0; id <= lf->col; id++)
				if (schema->cols[id].parent < 0)
					p = get_value(r, &schema->cols[id], p, end, &r->val[id]);
			if (r->val[lf->col].overflow)
			{
				if (lf->nover == lf->capover)
				{
					lf->capover = Max(16, lf->capover * 2);
					lf->over = lf->over ? repalloc(lf->over, lf->capover * sizeof(ItemPointerData)) : MemoryContextAlloc(wbuf_cxt, lf->capover * sizeof(ItemPointerData));
				}
				lf->over[lf->nover++] = rec.tid;
			}
			else if (!r->val[lf->col].isnull)
				leaf_add(lf, leaf_hash(str, &r->val[lf->col]), rec.tid);
		}
		wblk = r->it.blk;
		wpos = r->cur.pos;
	}
	/* the end of the lane (the last page fully read): the next read starts there */
	wblk = r->it.blk;
	wpos = r->cur.pos;
done:
	l->blk = wblk;
	l->pos = wpos;
}

static WbufLeaf *
leaf_get(Relation index, const SiftSegEntry *entry, int col)
{
	WbufLeaf   *lf;
	int			free_slot = -1,
				lru = 0;

	if (wbuf_cxt == NULL)
		wbuf_cxt = AllocSetContextCreate(TopMemoryContext, "sift cache write buffer keys", ALLOCSET_DEFAULT_SIZES);
	for (int i = 0; i < WBUF_CACHE_LEAVES; i++)
	{
		lf = wbuf_leaves[i];
		if (lf == NULL)
			free_slot = i;
		else if (lf->rel == index->rd_locator.relNumber && lf->gen == entry->gen && lf->col == col)
			return lf;
		else if (wbuf_leaves[lru] == NULL || lf->used < wbuf_leaves[lru]->used)
			lru = i;
	}
	if (free_slot < 0)
	{
		leaf_free(wbuf_leaves[lru]);
		free_slot = lru;
	}
	lf = MemoryContextAllocZero(wbuf_cxt, sizeof(WbufLeaf));
	lf->rel = index->rd_locator.relNumber;
	lf->gen = entry->gen;
	lf->col = col;
	for (int i = 0; i < SIFT_LANES; i++)
		lf->lane[i].blk = InvalidBlockNumber;
	wbuf_leaves[free_slot] = lf;
	return lf;
}

/*
 * The heap addresses of the live records of a write buffer whose plain leaf holds one of the constants of pred (EQ or IN; INT: the integer, STR: the bytes): the
 * records are read once by this backend (leaf_read_lane), a lookup is a probe of the hash for each constant. A value the leaf could not store (an overflow)
 * may be any: its record is kept. The hash may name another value: the heap checks the condition. Ascending, each once; n = the count.
 */
ItemPointerData *
sift_cwbuf_lookup(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, const SiftColPred *pred, MemoryContext mcxt, uint32 *n)
{
	int			col = pred->col;
	bool		str = schema->cols[col].store == SIFT_COLS_STR;
	WbufLeaf   *lf = leaf_get(index, entry, col);
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift col write buffer lookup", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(tmp);
	SiftCWbufReader *r = sift_cwbuf_open(index, entry, schema, tmp);
	ItemPointerData *out;
	uint32		m,
				cap,
				total = 0;

	lf->used = ++wbuf_clock;
	for (int l = 0; l < SIFT_LANES; l++)
		if (BlockNumberIsValid(r->it.head[l]))
			leaf_read_lane(lf, r, schema, l, r->it.head[l]);
	sift_cwbuf_close(r);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
	cap = lf->nover + 16;
	out = MemoryContextAlloc(mcxt, cap * sizeof(ItemPointerData));
	/* over is NULL until a row overflows: memcpy takes no NULL source, not even for zero bytes */
	if (lf->nover > 0)
		memcpy(out, lf->over, lf->nover * sizeof(ItemPointerData));
	m = lf->nover;
	for (int j = 0; j < pred->nvals; j++)
	{
		uint64		h = leaf_hash(str, &pred->vals[j]);

		for (uint32 e = lf->bucket ? lf->bucket[h & (lf->nbuckets - 1)] : 0; e != 0; e = lf->ent[e - 1].next)
			if (lf->ent[e - 1].h == h)
			{
				if (m == cap)
					out = repalloc(out, (cap *= 2) * sizeof(ItemPointerData));
				out[m++] = lf->ent[e - 1].tid;
			}
	}
	for (int i = 0; i < WBUF_CACHE_LEAVES; i++)
		if (wbuf_leaves[i])
			total += wbuf_leaves[i]->n;
	if (total > WBUF_CACHE_ROWS)
		for (int i = 0; i < WBUF_CACHE_LEAVES; i++)
			if (wbuf_leaves[i])
				leaf_free(wbuf_leaves[i]);
	if (m > 1)
	{
		uint32		k = 1;

		/* in the order of the heap, a repeated address once */
		qsort(out, m, sizeof(ItemPointerData), tid_cmp);
		for (uint32 i = 1; i < m; i++)
			if (ItemPointerCompare(&out[i], &out[k - 1]) != 0)
				out[k++] = out[i];
		m = k;
	}
	*n = m;
	return out;
}

/* A live record kept for sorting: its payload is at off in the arena */
typedef struct Held
{
	SiftKey		key;
	ItemPointerData tid;
	uint32		seq;			/* order of reading, to keep the first record of a repeated TID */
	uint32		len;
	Size		off;
} Held;

static int
held_cmp(const void *a, const void *b)
{
	const Held *x = a,
			   *y = b;

	return x->key != y->key ? pg_cmp_u64(x->key, y->key) : pg_cmp_u32(x->seq, y->seq);
}

SiftColBatch **
sift_cwbuf_batches(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt, int *n)
{
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift col write buffer batches", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(tmp);
	SiftCWbufReader *r = sift_cwbuf_open(index, entry, schema, tmp);
	StringInfoData arena;
	Held	   *held = palloc(256 * sizeof(Held));
	int			nheld = 0,
				heldcap = 256,
				cap = 0;
	SiftColBatch **out = NULL;
	SiftColBatch *b = NULL;
	RawRec		rec;
	SiftColRow	row;

	initStringInfo(&arena);
	*n = 0;
	while (next_rec(r, &rec))
	{
		if (rec.deleted)
			continue;
		if (nheld == heldcap)
			held = repalloc(held, (heldcap *= 2) * sizeof(Held));
		held[nheld] = (Held) {sift_key_from_tid(&rec.tid), rec.tid, nheld, rec.len, arena.len};
		nheld++;
		appendBinaryStringInfo(&arena, (const char *) rec.payload, rec.len);
	}
	qsort(held, nheld, sizeof(Held), held_cmp);
	for (int i = 0; i < nheld; i++)
	{
		uint32		sb = sift_block_sb(sift_key_block(held[i].key));

		if (i > 0 && held[i].key == held[i - 1].key)
			continue;
		if (b == NULL || b->sb != sb)
		{
			b = sift_cbatch_create(mcxt, schema);
			sift_cbatch_reset(b, sb);
			if (*n == cap)
				out = out ? repalloc(out, (cap *= 2) * sizeof(SiftColBatch *)) :
					MemoryContextAlloc(mcxt, (cap = 8) * sizeof(SiftColBatch *));
			out[(*n)++] = b;
		}
		rec = (RawRec) {held[i].tid, false, (const uint8 *) arena.data + held[i].off, held[i].len};
		decode_row(r, &rec, &row);
		sift_cbatch_add(b, &row);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
	return out;
}

static int
collect_rows(void *arg, Page page, Size *offs, ItemPointerData *tids)
{
	PageCursor	c = {.index = arg};
	int			n = 0;

	cursor_set(&c, page);
	while (c.pos < c.used)
	{
		Size		size;
		const SiftColRecHead *h = rec_at(&c, &size);

		if (!(h->flags & (SIFT_COLR_CONT | SIFT_COLR_DELETED)))
		{
			offs[n] = c.pos;
			tids[n++] = h->tid;
		}
		c.pos += size;
	}
	return n;
}

static void
flag_row(Page page, Size off)
{
	((SiftColRecHead *) (SiftLaneRecs(page, HDRSIZE) + off))->flags |= SIFT_COLR_DELETED;
}

int64
sift_cwbuf_bulkdelete(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, IndexBulkDeleteCallback cb,
					 void *cbstate, IndexBulkDeleteResult *stats)
{
	Page		page = palloc(BLCKSZ);
	int64		removed;

	flush_for_read(index);
	removed = sift_lane_bulkdelete(index, entry, HDRSIZE, RECSIZE(0), page, collect_rows, flag_row, index, cb, cbstate, stats);
	pfree(page);
	return removed;
}

SiftExtent *
sift_cwbuf_pages(Relation index, const SiftSegEntry *entry, int *n)
{
	flush_for_read(index);
	return sift_lane_extents(index, entry, n);
}

void
sift_cwbuf_summary(Relation index, const SiftSegEntry *entry, uint64 *nrows, uint64 *npages)
{
	SiftWbufHeaderData hdr;

	flush_for_read(index);
	*nrows = count_rows(index, entry, false);
	sift_lane_read_header(index, entry, &hdr);
	*npages = 1 + sift_lane_npages(&hdr);
}

static void
verify_page(void *arg, Page page, BlockNumber blk)
{
	Relation	index = arg;
	PageCursor	c = {.index = index};
	uint64		nrecs = 0;

	cursor_set(&c, page);
	while (c.pos < c.used)
	{
		Size		size;
		const SiftColRecHead *h = rec_at(&c, &size);

		if (!ItemPointerIsValid(&h->tid) || (h->flags & ~(SIFT_COLR_DELETED | SIFT_COLR_CONT)))
			SIFT_CORRUPT(index, "A write buffer record has tid (%u,%u) and flags %x.", ItemPointerGetBlockNumberNoCheck(&h->tid),
					ItemPointerGetOffsetNumberNoCheck(&h->tid), h->flags);
		nrecs += !(h->flags & SIFT_COLR_CONT);
		c.pos += size;
	}
	if (nrecs != SiftLaneHead(page)->nrecs)
		SIFT_CORRUPT(index, "Write buffer block %u holds %llu rows, its header %u.", blk, (unsigned long long) nrecs,
				SiftLaneHead(page)->nrecs);
}

uint64
sift_cwbuf_verify(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema)
{
	SiftCWbufReader *r;
	SiftColRow	row;
	bool		deleted;
	uint64		live = 0;

	flush_for_read(index);
	sift_lane_verify(index, entry, HDRSIZE, verify_page, index);
	r = sift_cwbuf_open(index, entry, schema, CurrentMemoryContext);
	while (sift_cwbuf_next(r, &row, &deleted))
		live += !deleted;
	sift_cwbuf_close(r);
	return live;
}
