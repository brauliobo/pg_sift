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
#include "common/int.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/sinval.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/wbuf.h"
#include "col/pg.h"
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
put_varint(StringInfo buf, uint64 x)
{
	enlargeStringInfo(buf, 10);
	buf->len = (char *) sift_put_varint((uint8 *) buf->data + buf->len, x) - buf->data;
	buf->data[buf->len] = '\0';
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
		put_varint(buf, v->v.i);
		return;
	}
	switch (def->store)
	{
		case SIFT_COLS_INT:
			put_varint(buf, sift_zigzag(v->v.i));
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
			put_varint(buf, v->len);
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
static void
invalidate_plans(Relation index)
{
	SharedInvalidationMessage msg = {.rc = {.id = SHAREDINVALRELCACHE_ID,.dbId = MyDatabaseId,.relId = index->rd_index->indrelid}};

	CacheInvalidateRelcacheByRelid(msg.rc.relId);
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
				invalidate_plans(index);
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
	return sift_lane_seal(index, force, HDRSIZE, SIFT_COL_FORMAT_KINDS, limit_pages(index), has_rows);
}

/* VACUUM seals an open buffer once it holds a quarter of write_buffer_limit: its rows are promoted and merged with the rest instead of waiting for a seal that may never come */
uint64
sift_cwbuf_seal_vacuum(Relation index)
{
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
	SiftCWbufReader *r = MemoryContextAllocZero(mcxt, sizeof(SiftCWbufReader));

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
	int64		removed = sift_lane_bulkdelete(index, entry, HDRSIZE, RECSIZE(0), page, collect_rows, flag_row, index, cb, cbstate,
											   stats);

	pfree(page);
	return removed;
}

SiftExtent *
sift_cwbuf_pages(Relation index, const SiftSegEntry *entry, int *n)
{
	return sift_lane_extents(index, entry, n);
}

void
sift_cwbuf_summary(Relation index, const SiftSegEntry *entry, uint64 *nrows, uint64 *npages)
{
	SiftWbufHeaderData hdr;

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

	sift_lane_verify(index, entry, HDRSIZE, verify_page, index);
	r = sift_cwbuf_open(index, entry, schema, CurrentMemoryContext);
	while (sift_cwbuf_next(r, &row, &deleted))
		live += !deleted;
	sift_cwbuf_close(r);
	return live;
}
