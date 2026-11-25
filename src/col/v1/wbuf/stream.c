/*
 * col/v1/wbuf/stream.c - the live rows of a write buffer in the order of their addresses, one superblock at a time
 * The records go once into a sort that holds a share of maintenance_work_mem and spills the rest, so the memory of a promotion follows its setting and not the rows
 * of the buffer: the batches of the superblocks are decoded from the sorted records as the writer asks for them.
 */
#include "postgres.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "port/pg_bswap.h"
#include "utils/tuplesort.h"
#include "core/memtrack.h"
#include "col/v1/wbuf/int.h"

/* A record in the sort, compared as bytes: its key and its order of reading (big endian: the first record of a repeated address comes first), then its address, its inserter and its payload */
#define REC_SEQ		sizeof(SiftKey)
#define REC_TID		(REC_SEQ + sizeof(uint32))
#define REC_XMIN	(REC_TID + sizeof(ItemPointerData))
#define REC_HEAD	(REC_XMIN + sizeof(TransactionId))

struct SiftCWbufStream
{
	MemoryContext cx;
	SiftCWbufReader *r;
	Tuplesortstate *ts;
	SiftXWait	xwait;			/* who inserted the rows (sift_cwbuf_stream_open) */
	bool		xbad;
	uint32		seq;
	uint64		rows;
	int			nbatches;
	bool		kinds_exact;
	bytea	   *ahead;			/* the record that opens the next superblock */
	bool		have_prev;
	SiftKey		prev;
};

static SiftKey
rec_key(const bytea *rec)
{
	uint64		be;

	memcpy(&be, VARDATA(rec), sizeof(be));
	return pg_ntoh64(be);
}

/* The inserter of a record is accounted for as sift_xstamp_make reads it: the stamp is none when one record has no committed or open inserter, or more of them are open than a stamp holds */
static void
note_inserter(SiftCWbufStream *s, TransactionId xmin, uint8 st)
{
	SiftXWait  *w = &s->xwait;

	if (s->xbad)
		return;
	if ((st != XST_COMMITTED && st != XST_OPEN) || (st == XST_OPEN && !sift_xwait_note(w, xmin)))
	{
		s->xbad = true;
		w->top = InvalidTransactionId;
		return;
	}
	if (!TransactionIdIsValid(w->top) || TransactionIdFollows(xmin, w->top))
		w->top = xmin;
}

static void
stream_put(void *arg, const RawRec *rec)
{
	SiftCWbufStream *s = arg;
	uint8		st = wbuf_xst_of(rec->xmin);
	uint64		key;
	uint32		seq;
	bytea	   *d;
	uint8	   *p;

	/* the tuple of a transaction that did not commit is seen by no one: VACUUM would take its entry out, and the promotion does not write it */
	if (st == XST_ABORTED)
		return;
	note_inserter(s, rec->xmin, st);
	d = palloc(VARHDRSZ + REC_HEAD + rec->len);
	SET_VARSIZE(d, VARHDRSZ + REC_HEAD + rec->len);
	p = (uint8 *) VARDATA(d);
	key = pg_hton64(sift_key_from_tid((ItemPointer) &rec->tid));
	seq = pg_hton32(s->seq++);
	memcpy(p, &key, sizeof(key));
	memcpy(p + REC_SEQ, &seq, sizeof(seq));
	memcpy(p + REC_TID, &rec->tid, sizeof(ItemPointerData));
	memcpy(p + REC_XMIN, &rec->xmin, sizeof(TransactionId));
	memcpy(p + REC_HEAD, rec->payload, rec->len);
	tuplesort_putdatum(s->ts, PointerGetDatum(d), false);
	pfree(d);
}

static void
stream_fill(SiftCWbufStream *s)
{
	WbufLane	lane[SIFT_LANES];

	for (int l = 0; l < SIFT_LANES; l++)
		lane[l] = (WbufLane) {InvalidBlockNumber, 0};
	wbuf_xst_forget();
	wbuf_load_heads(s->r);
	for (int l = 0; l < SIFT_LANES; l++)
		if (BlockNumberIsValid(s->r->it.head[l]))
			wbuf_read_lane(s->r, s->r->it.head[l], &lane[l], stream_put, s);
	tuplesort_performsort(s->ts);
}

/* The next record that is not a repeat of the one before, to be freed by the caller; NULL at the end */
static bytea *
sort_next(SiftCWbufStream *s)
{
	for (;;)
	{
		Datum		d;
		bool		isnull;
		bytea	   *rec;
		SiftKey		key;

		if (!tuplesort_getdatum(s->ts, true, true, &d, &isnull, NULL))
			return NULL;
		rec = (bytea *) DatumGetPointer(d);
		key = rec_key(rec);
		if (s->have_prev && key == s->prev)
		{
			pfree(rec);
			continue;
		}
		s->have_prev = true;
		s->prev = key;
		return rec;
	}
}

static uint32
rec_sb(const bytea *rec)
{
	return sift_block_sb(sift_key_block(rec_key(rec)));
}

void
sift_cwbuf_stream_rewind(SiftCWbufStream *s)
{
	if (s->ahead != NULL)
		pfree(s->ahead);
	s->ahead = NULL;
	s->have_prev = false;
	tuplesort_rescan(s->ts);
}

bool
sift_cwbuf_stream_next(SiftCWbufStream *s, SiftColBatch *b)
{
	bytea	   *rec = s->ahead != NULL ? s->ahead : sort_next(s);
	uint32		sb;

	s->ahead = NULL;
	if (rec == NULL)
		return false;
	sb = rec_sb(rec);
	sift_cbatch_reset(b, sb);
	while (rec != NULL && rec_sb(rec) == sb)
	{
		const uint8 *p = (const uint8 *) VARDATA(rec);
		RawRec		raw = {.deleted = false,.payload = p + REC_HEAD,.len = VARSIZE_ANY_EXHDR(rec) - REC_HEAD};
		SiftColRow	row;

		memcpy(&raw.tid, p + REC_TID, sizeof(ItemPointerData));
		memcpy(&raw.xmin, p + REC_XMIN, sizeof(TransactionId));
		wbuf_decode_row(s->r, &raw, &row);
		sift_cbatch_add(b, &row);
		pfree(rec);
		rec = sort_next(s);
	}
	s->ahead = rec;
	return true;
}

/* The superblocks and the rows of the stream, and whether every row keeps its kinds: only a buffer written before the kinds were kept (SIFT_COL_FORMAT_KINDS) can hold a row that does not, and its rows are decoded to find out */
static void
stream_count(SiftCWbufStream *s)
{
	bytea	   *rec;
	uint32		sb = 0;

	while ((rec = sort_next(s)) != NULL)
	{
		if (s->rows == 0 || rec_sb(rec) != sb)
			s->nbatches++;
		sb = rec_sb(rec);
		s->rows++;
		pfree(rec);
	}
	sift_cwbuf_stream_rewind(s);
	s->kinds_exact = true;
	if (s->r->entry.format < SIFT_COL_FORMAT_KINDS && s->rows > 0)
	{
		SiftColBatch *b = sift_cbatch_create(s->cx, s->r->schema);

		while (sift_cwbuf_stream_next(s, b))
			s->kinds_exact &= sift_cbatch_kinds_exact(b);
		sift_cwbuf_stream_rewind(s);
	}
}

SiftCWbufStream *
sift_cwbuf_stream_open(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, SiftXWait *stamp)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift col write buffer stream");
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftCWbufStream *s = palloc0(sizeof(SiftCWbufStream));
	SiftMemTracker mt_col_write_buffer;
	Size		budget = sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 2);

	sift_mem_init_sub(&mt_col_write_buffer, "col_write_buffer", cx, false, budget);
	s->cx = cx;
	s->r = sift_cwbuf_open(index, entry, schema, cx);
	s->ts = tuplesort_begin_datum(BYTEAOID, ByteaLessOperator, InvalidOid, false, (int) Max(budget / 2048, 64), NULL, TUPLESORT_RANDOMACCESS);
	s->xwait = (SiftXWait) {InvalidTransactionId};
	stream_fill(s);
	sift_mem_assert_within(&mt_col_write_buffer);
	stream_count(s);
	*stamp = s->xwait;
	MemoryContextSwitchTo(old);
	return s;
}

int
sift_cwbuf_stream_batches(const SiftCWbufStream *s)
{
	return s->nbatches;
}

uint64
sift_cwbuf_stream_rows(const SiftCWbufStream *s)
{
	return s->rows;
}

bool
sift_cwbuf_stream_kinds_exact(const SiftCWbufStream *s)
{
	return s->kinds_exact;
}

SiftColBatch **
sift_cwbuf_stream_collect(SiftCWbufStream *s, MemoryContext mcxt)
{
	SiftColBatch **out = MemoryContextAlloc(mcxt, Max(s->nbatches, 1) * sizeof(SiftColBatch *));

	for (int i = 0; i < s->nbatches; i++)
	{
		out[i] = sift_cbatch_create(mcxt, s->r->schema);
		if (!sift_cwbuf_stream_next(s, out[i]))
			SIFT_CORRUPT(s->r->cur.index, "The sorted records of write buffer %llu ended before its %d superblocks.", (unsigned long long) s->r->entry.gen, s->nbatches);
	}
	sift_cwbuf_stream_rewind(s);
	return out;
}

void
sift_cwbuf_stream_close(SiftCWbufStream *s)
{
	tuplesort_end(s->ts);
	MemoryContextDelete(s->cx);
}
