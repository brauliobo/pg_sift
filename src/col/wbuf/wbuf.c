/*
 * col/wbuf/wbuf.c - the write buffer of a column index: rows encoded into pages of the
 * buffer, their pieces and overflow values, the append of a row
 * the files next to it hold the pending rows, the reader and the lookups
 */
#include "postgres.h"
#include "utils/inval.h"
#include "col/am.h"
#include "col/wbuf/wbuf_int.h"

StaticAssertDecl(sizeof(SiftColRecHead) == 12, "the record head is 12 bytes");
#ifdef SIFT_TESTING
int			sift_cwbuf_fail_grow = 0;
#endif

uint32
wbuf_limit_pages(Relation index)
{
	return Max(2 * SIFT_LANES, (uint64) sift_col_options(index)->write_buffer_limit * 1024 / BLCKSZ);
}

void
wbuf_cursor_set(PageCursor *c, Page page)
{
	c->page = page;
	c->pos = 0;
	c->used = SiftPageGetUsed(page) - HDRSIZE;
}

/* The record at c->pos, checked; *size its length */
const SiftColRecHead *
wbuf_rec_at(PageCursor *c, Size *size)
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

/* is the slot a JSON null (a NULL that is not a missing key)? */
static inline bool
value_jnull(const SiftColValue *v)
{
	return v->isnull && !v->overflow && v->tag == SIFT_COLTAG_JNULL;
}

/* Appends the row's payload; returns the leaves (SIFT_COL_OVF_BIT) it holds an overflow value of, and in *jnull those it holds a JSON null of */
uint32
wbuf_encode_row(StringInfo buf, const SiftColSchema *schema, const SiftColRow *row, uint32 *jnull)
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

void
wbuf_put_piece(Page page, const ItemPointerData *tid, bool cont, const char *payload, Size len)
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

	wbuf_put_piece(cp, a->tid, a->cont, a->payload, a->len);
	if (a->spans)
		SiftLaneHead(cp)->flags |= SIFT_WBUFPAGE_SPANS;
}

/* Writes the row starting at the locked tail cur; returns whether the segment reached write_buffer_limit. Leaves the final tail block in *tail. */
bool
wbuf_write_row(Relation index, SiftCWbufHint *h, int lane, Buffer cur, const ItemPointerData *tid, const char *payload, Size len,
		  BlockNumber *tail)
{
	bool		crossed = false,
				first = true;
	Size		done = 0;
	uint32		limit = wbuf_limit_pages(index);

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
			wbuf_put_piece(ed.page, tid, !first, payload + done, n);
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
void
wbuf_flag_overflow(Relation index, SiftCWbufHint *h, uint32 mask, uint32 jmask)
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
	ovf = wbuf_encode_row(&buf, schema, row, &jnull);
	for (;;)
	{
		if (h->gen == 0)
			sift_lane_refresh(index, schema->hash, "column definitions", HDRSIZE, SIFT_COL_FORMAT_KINDS, sift_col_maint_make_room, &h->gen, &h->hdr, h->tail);
		wbuf_flag_overflow(index, h, ovf, jnull);
		if (BufferIsValid(tailbuf = sift_lane_lock_tail(index, h->gen, h->tail, SIFT_LANES, &lane)))
			break;
		h->gen = 0;
	}
	crossed = wbuf_write_row(index, h, lane, tailbuf, &row->tid, buf.data, buf.len, &tail);
	h->tail[lane] = tail;
	pfree(buf.data);
	return crossed;
}
