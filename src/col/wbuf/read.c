/*
 * col/wbuf/read.c - the reader of the write buffer: open and close a cursor, join the
 * pieces of a row, decode its values and hand the rows to a scan
 * in the order they were written
 */
#include "postgres.h"
#include "col/wbuf/wbuf_int.h"

/* A reader whose lanes are not yet located: wbuf_load_heads reads the header page for them */
SiftCWbufReader *
wbuf_open_lazy(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt)
{
	SiftCWbufReader *r;

	wbuf_flush_for_read(index);
	r = MemoryContextAllocZero(mcxt, sizeof(SiftCWbufReader));
	r->entry = *entry;
	r->schema = schema;
	r->mcxt = mcxt;
	r->page = MemoryContextAlloc(mcxt, BLCKSZ);
	r->cur.index = index;
	r->cur.page = r->page;
	r->val = MemoryContextAllocZero(mcxt, schema->ncols * sizeof(SiftColValue));
	r->elem = MemoryContextAllocZero(mcxt, schema->ncols * sizeof(SiftColValue *));
	r->it.index = index;
	r->it.entry = &r->entry;
	r->it.nextblk = InvalidBlockNumber;
	for (int l = 0; l < SIFT_LANES; l++)
		r->it.head[l] = InvalidBlockNumber;
	return r;
}

void
wbuf_load_heads(SiftCWbufReader *r)
{
	sift_lane_iter_init(&r->it, r->cur.index, &r->entry);
}

SiftCWbufReader *
sift_cwbuf_open(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt)
{
	SiftCWbufReader *r = wbuf_open_lazy(index, entry, schema, mcxt);

	wbuf_load_heads(r);
	return r;
}

void
sift_cwbuf_close(SiftCWbufReader *r)
{
	if (r->joined)
		pfree(r->joined);
	if (r->evals)
		pfree(r->evals);
	if (r->ref)
		pfree(r->ref);
	if (r->refpay.data)
		pfree(r->refpay.data);
	pfree(r->val);
	pfree(r->elem);
	pfree(r->page);
	pfree(r);
}

bool
wbuf_advance_page(SiftCWbufReader *r)
{
	if (!sift_lane_iter_next(&r->it, r->page, HDRSIZE))
		return false;
	wbuf_cursor_set(&r->cur, r->page);
	r->refready = false;
	r->pflags = SiftLaneHead(r->page)->flags;
	if (r->pagefn)
		r->pagefn(r->pagearg, r->it.blk, r->page, r->cur.used);
	return true;
}

/* The records of a page that start in bytes from to upto and are flagged dead; from is the start of a record (0, or the used bytes of an earlier read of the page) */
uint32
wbuf_page_dead(Relation index, Page page, Size from, Size upto)
{
	PageCursor	c = {.index = index};
	uint32		n = 0;

	wbuf_cursor_set(&c, page);
	c.pos = from;
	while (c.pos < c.used && c.pos < upto)
	{
		Size		size;
		const SiftColRecHead *h = wbuf_rec_at(&c, &size);

		n += (h->flags & SIFT_COLR_DELETED) != 0;
		c.pos += size;
	}
	return n;
}

static void
join_append(SiftCWbufReader *r, const uint8 *payload, Size len)
{
	if (r->joinedlen + len > r->joinedcap)
	{
		r->joinedcap = Max(2 * r->joinedcap, r->joinedlen + len);
		SIFT_REGROW_IN(r->mcxt, r->joined, r->joinedcap);
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
		wbuf_advance_page(r);
		if (r->cur.pos >= r->cur.used)
			return false;
		h = wbuf_rec_at(&r->cur, &size);
		if (!(h->flags & SIFT_COLR_CONT))
			return false;
		r->cur.pos += size;
		join_append(r, (const uint8 *) (h + 1), h->len);
	}
	return true;
}

/* The next row's record, pieces joined, the values of a REF record fetched from the heap (a row the heap has no tuple for is passed over); false at the end of the segment */
bool
wbuf_next_rec(SiftCWbufReader *r, RawRec *rec)
{
	for (;;)
	{
		const SiftColRecHead *h;
		Size		size,
					pos;

		if (r->cur.pos >= r->cur.used)
		{
			if (!wbuf_advance_page(r))
				return false;
			continue;
		}
		pos = r->cur.pos;
		h = wbuf_rec_at(&r->cur, &size);
		if (h->flags & SIFT_COLR_CONT)
			SIFT_CORRUPT(r->cur.index, "A continuation record of segment %llu has no first piece.", (unsigned long long) r->entry.gen);
		r->cur.pos += size;
		rec->tid = h->tid;
		rec->deleted = (h->flags & SIFT_COLR_DELETED) != 0;
		rec->payload = (const uint8 *) (h + 1);
		rec->len = h->len;
		rec->xmin = InvalidTransactionId;
		if (h->flags & SIFT_COLR_XMIN)
		{
			if (rec->len < sizeof(TransactionId))
				SIFT_CORRUPT(r->cur.index, "A record of segment %llu has an xmin flag and %u payload bytes.", (unsigned long long) r->entry.gen, (unsigned) rec->len);
			memcpy(&rec->xmin, rec->payload, sizeof(TransactionId));
			rec->payload += sizeof(TransactionId);
			rec->len -= sizeof(TransactionId);
		}
		/* a record that is a reference is of one piece, and one that VACUUM flagged dead has no tuple to fetch */
		if (h->flags & SIFT_COLR_REF)
		{
			if (rec->deleted || !wbuf_ref_values(r, pos, rec))
				continue;
			return true;
		}
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

const uint8 *
wbuf_get_value(SiftCWbufReader *r, const SiftColDef *def, const uint8 *p, const uint8 *end, SiftColValue *v)
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
	SIFT_REGROW_IN(r->mcxt, r->evals, r->evalcap * sizeof(SiftColValue));
}

void
wbuf_decode_row(SiftCWbufReader *r, const RawRec *rec, SiftColRow *row)
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
			p = wbuf_get_value(r, &s->cols[id], p, end, &r->val[id]);
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
				p = wbuf_get_value(r, &s->cols[id], p, end, &r->evals[off + j]);
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

	if (!wbuf_next_rec(r, &rec))
		return false;
	wbuf_decode_row(r, &rec, row);
	*deleted = rec.deleted;
	return true;
}

/*
 * The live records of the lane that starts at head, from where the last read of l stopped (the start of the lane for a lane not read yet) to its end, passed to fn;
 * l is moved to the end read. The lane chains are append-only (VACUUM flags a record dead in place), so a later read starts there.
 */
void
wbuf_read_lane(SiftCWbufReader *r, BlockNumber head, WbufLane *l, void (*fn) (void *arg, const RawRec *rec), void *arg)
{
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
			if (!wbuf_advance_page(r))
				break;
			if (first)
				r->cur.pos = startpos;
			first = false;
			continue;
		}
		rowblk = r->it.blk;
		rowpos = r->cur.pos;
		r->torn = false;
		if (!wbuf_next_rec(r, &rec))
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
			fn(arg, &rec);
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
