/*
 * col/wbuf/read.c - the reader of the write buffer: open and close a cursor, join the
 * pieces of a row, decode its values and hand the rows to a scan
 * in the order they were written
 */
#include "postgres.h"
#include "col/wbuf/wbuf_int.h"

SiftCWbufReader *
sift_cwbuf_open(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, MemoryContext mcxt)
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

bool
wbuf_advance_page(SiftCWbufReader *r)
{
	if (!sift_lane_iter_next(&r->it, r->page, HDRSIZE))
		return false;
	wbuf_cursor_set(&r->cur, r->page);
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

/* The next row's record, pieces joined; false at the end of the segment */
bool
wbuf_next_rec(SiftCWbufReader *r, RawRec *rec)
{
	for (;;)
	{
		const SiftColRecHead *h;
		Size		size;

		if (r->cur.pos >= r->cur.used)
		{
			if (!wbuf_advance_page(r))
				return false;
			continue;
		}
		h = wbuf_rec_at(&r->cur, &size);
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
	r->evals = r->evals ? repalloc(r->evals, r->evalcap * sizeof(SiftColValue)) :
		MemoryContextAlloc(r->mcxt, r->evalcap * sizeof(SiftColValue));
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
