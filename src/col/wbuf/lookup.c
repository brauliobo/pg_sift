/*
 * col/wbuf/lookup.c - the lookups in the write buffer by heap address: the cache of leaves,
 * the rows a batch of addresses holds, the deletes and the summary
 * and the check of a page
 */
#include "postgres.h"
#include "col/point.h"
#include "col/wbuf/wbuf_int.h"

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
		{
			const uint8 *p = rec.payload,
					   *end = p + rec.len;

			for (int id = 0; id <= lf->col; id++)
				if (schema->cols[id].parent < 0)
					p = wbuf_get_value(r, &schema->cols[id], p, end, &r->val[id]);
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
	while (wbuf_next_rec(r, &rec))
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
		wbuf_decode_row(r, &rec, &row);
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

	wbuf_cursor_set(&c, page);
	while (c.pos < c.used)
	{
		Size		size;
		const SiftColRecHead *h = wbuf_rec_at(&c, &size);

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

	wbuf_flush_for_read(index);
	removed = sift_lane_bulkdelete(index, entry, HDRSIZE, RECSIZE(0), page, collect_rows, flag_row, index, cb, cbstate, stats);
	pfree(page);
	return removed;
}

SiftExtent *
sift_cwbuf_pages(Relation index, const SiftSegEntry *entry, int *n)
{
	wbuf_flush_for_read(index);
	return sift_lane_extents(index, entry, n);
}

void
sift_cwbuf_summary(Relation index, const SiftSegEntry *entry, uint64 *nrows, uint64 *npages)
{
	SiftWbufHeaderData hdr;

	wbuf_flush_for_read(index);
	*nrows = wbuf_count_rows(index, entry, false);
	sift_lane_read_header(index, entry, &hdr);
	*npages = 1 + sift_lane_npages(&hdr);
}

static void
verify_page(void *arg, Page page, BlockNumber blk)
{
	Relation	index = arg;
	PageCursor	c = {.index = index};
	uint64		nrecs = 0;

	wbuf_cursor_set(&c, page);
	while (c.pos < c.used)
	{
		Size		size;
		const SiftColRecHead *h = wbuf_rec_at(&c, &size);

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

	wbuf_flush_for_read(index);
	sift_lane_verify(index, entry, HDRSIZE, verify_page, index);
	r = sift_cwbuf_open(index, entry, schema, CurrentMemoryContext);
	while (sift_cwbuf_next(r, &row, &deleted))
		live += !deleted;
	sift_cwbuf_close(r);
	return live;
}
