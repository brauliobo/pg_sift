/*
 * col/v1/wbuf/lookup.c - the lookups in the write buffer by heap address: the cache of leaves,
 * the rows a batch of addresses holds, the deletes and the summary
 * and the check of a page
 */
#include "postgres.h"
#include "col/v1/point.h"
#include "col/v1/wbuf/wbuf_int.h"
#include "core/cachebudget.h"

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

typedef struct WbufLeaf
{
	RelFileNumber rel;
	uint64		gen;
	int			col;
	uint64		used;			/* for the least recently used */
	MemoryContext cx;			/* the leaf and its arrays: dropping the leaf deletes it */
	bool		pinned;			/* a lookup is reading it */
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

static MemoryContext wbuf_cxt;

static WbufLeaf *wbuf_leaves[WBUF_CACHE_LEAVES];

static void
leaf_free(WbufLeaf *lf)
{
	for (int i = 0; i < WBUF_CACHE_LEAVES; i++)
		if (wbuf_leaves[i] == lf)
			wbuf_leaves[i] = NULL;
	MemoryContextDelete(lf->cx);
}

static WbufLeaf *
leaf_victim(void)
{
	WbufLeaf   *lru = NULL;

	for (int i = 0; i < WBUF_CACHE_LEAVES; i++)
		if (wbuf_leaves[i] && !wbuf_leaves[i]->pinned && (lru == NULL || wbuf_leaves[i]->used < lru->used))
			lru = wbuf_leaves[i];
	return lru;
}

static uint64
leaf_oldest(void)
{
	WbufLeaf   *lf = leaf_victim();

	return lf ? lf->used : UINT64_MAX;
}

static bool
leaf_evict_one(void)
{
	WbufLeaf   *lf = leaf_victim();

	if (lf == NULL)
		return false;
	leaf_free(lf);
	return true;
}

static void
leaf_evict_all(void)
{
	while (leaf_evict_one())
		;
}

static const SiftCacheKind wbuf_kind = {"col_wbuf_lookup", leaf_oldest, leaf_evict_one, leaf_evict_all};

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
		SIFT_REGROW_IN(lf->cx, lf->ent, lf->cap * sizeof(WbufEnt));
	}
	if (lf->n >= lf->nbuckets)
	{
		lf->nbuckets = Max(64, lf->nbuckets * 2);
		SIFT_REGROW_IN(lf->cx, lf->bucket, lf->nbuckets * sizeof(uint32));
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

typedef struct LeafRead
{
	WbufLeaf   *lf;
	SiftCWbufReader *r;
	const SiftColSchema *schema;
} LeafRead;

static void
leaf_read_rec(void *arg, const RawRec *rec)
{
	LeafRead   *lr = arg;
	WbufLeaf   *lf = lr->lf;
	SiftCWbufReader *r = lr->r;
	const SiftColSchema *schema = lr->schema;
	bool		str = schema->cols[lf->col].store == SIFT_COLS_STR;
	const uint8 *p = rec->payload,
			   *end = p + rec->len;

	for (int id = 0; id <= lf->col; id++)
		if (schema->cols[id].parent < 0)
			p = wbuf_get_value(r, &schema->cols[id], p, end, &r->val[id]);
	if (r->val[lf->col].overflow)
	{
		if (lf->nover == lf->capover)
		{
			lf->capover = Max(16, lf->capover * 2);
			SIFT_REGROW_IN(lf->cx, lf->over, lf->capover * sizeof(ItemPointerData));
		}
		lf->over[lf->nover++] = rec->tid;
	}
	else if (!r->val[lf->col].isnull)
		leaf_add(lf, leaf_hash(str, &r->val[lf->col]), rec->tid);
}

static WbufLeaf *
leaf_get(Relation index, const SiftSegEntry *entry, int col)
{
	WbufLeaf   *lf;
	MemoryContext cx;
	int			free_slot = -1,
				lru = -1;

	if (wbuf_cxt == NULL)
	{
		wbuf_cxt = sift_cache_create_context("sift cache write buffer keys");
		sift_cache_register(&wbuf_kind);
	}
	for (int i = 0; i < WBUF_CACHE_LEAVES; i++)
	{
		lf = wbuf_leaves[i];
		if (lf == NULL)
			free_slot = i;
		else if (lf->rel == index->rd_locator.relNumber && lf->gen == entry->gen && lf->col == col)
			return lf;
		else if (!lf->pinned && (lru < 0 || lf->used < wbuf_leaves[lru]->used))
			lru = i;
	}
	for (int i = 0; i < WBUF_CACHE_LEAVES; i++)
		if (wbuf_leaves[i] && !wbuf_leaves[i]->pinned && wbuf_leaves[i]->rel == index->rd_locator.relNumber && wbuf_leaves[i]->gen < entry->gen)
		{
			leaf_free(wbuf_leaves[i]);
			free_slot = i;
		}
	if (free_slot < 0)
	{
		SIFT_INVARIANT(lru >= 0, "write buffer lookup: every cached leaf is being read");
		leaf_free(wbuf_leaves[lru]);
		free_slot = lru;
	}
	lf = MemoryContextAllocZero(cx = AllocSetContextCreate(wbuf_cxt, "sift cache write buffer leaf", 8 * 1024, 8 * 1024, 64 * 1024), sizeof(WbufLeaf));
	lf->cx = cx;
	lf->rel = index->rd_locator.relNumber;
	lf->gen = entry->gen;
	lf->col = col;
	for (int i = 0; i < SIFT_LANES; i++)
		lf->lane[i].blk = InvalidBlockNumber;
	wbuf_leaves[free_slot] = lf;
	return lf;
}

/* Reads the records of the buffer that are new to the leaf: opening the buffer asks the cache for room, so the leaf is pinned by the caller */
static void
leaf_fill(WbufLeaf *lf, Relation index, const SiftSegEntry *entry, const SiftColSchema *schema)
{
	MemoryContext tmp = AllocSetContextCreate(CurrentMemoryContext, "sift col write buffer lookup", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(tmp);
	SiftCWbufReader *r = sift_cwbuf_open(index, entry, schema, tmp);

	for (int l = 0; l < SIFT_LANES; l++)
		if (BlockNumberIsValid(r->it.head[l]))
			wbuf_read_lane(r, r->it.head[l], &lf->lane[l], leaf_read_rec, &(LeafRead) {lf, r, schema});
	sift_cwbuf_close(r);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
}

/* The addresses of the leaf that the constants of pred name, and every row it could not store, in the order of the heap, each once */
static ItemPointerData *
leaf_addresses(const WbufLeaf *lf, bool str, const SiftColPred *pred, MemoryContext mcxt, uint32 *n)
{
	uint32		cap = lf->nover + 16,
				m = lf->nover;
	ItemPointerData *out = MemoryContextAlloc(mcxt, cap * sizeof(ItemPointerData));

	/* over is NULL until a row overflows: memcpy takes no NULL source, not even for zero bytes */
	if (lf->nover > 0)
		memcpy(out, lf->over, lf->nover * sizeof(ItemPointerData));
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
	if (m > 1)
	{
		uint32		k = 1;

		qsort(out, m, sizeof(ItemPointerData), tid_cmp);
		for (uint32 i = 1; i < m; i++)
			if (ItemPointerCompare(&out[i], &out[k - 1]) != 0)
				out[k++] = out[i];
		m = k;
	}
	*n = m;
	return out;
}

/*
 * The heap addresses of the live records of a write buffer whose plain leaf holds one of the constants of pred (EQ or IN; INT: the integer, STR: the bytes): the
 * records are read once by this backend (wbuf_read_lane), a lookup is a probe of the hash for each constant. A value the leaf could not store (an overflow)
 * may be any: its record is kept. The hash may name another value: the heap checks the condition. Ascending, each once; n = the count, rows = the live records of the leaf.
 */
ItemPointerData *
sift_cwbuf_lookup(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, const SiftColPred *pred, MemoryContext mcxt, uint32 *n, uint64 *rows)
{
	WbufLeaf   *lf = leaf_get(index, entry, pred->col);
	ItemPointerData *out;

	/* pinned for the read and the copy: a leaf that fails in either is dropped with its pin, and one that does not is let go below */
	lf->used = sift_cache_tick();
	lf->pinned = true;
	PG_TRY();
	{
		leaf_fill(lf, index, entry, schema);
		*rows = lf->n + lf->nover;
		out = leaf_addresses(lf, schema->cols[pred->col].store == SIFT_COLS_STR, pred, mcxt, n);
	}
	PG_CATCH();
	{
		leaf_free(lf);
		PG_RE_THROW();
	}
	PG_END_TRY();
	lf->pinned = false;
	sift_cache_commit();
	return out;
}

/* What the walkers of the pages of a buffer (VACUUM, verify) are given: the index, and the head of the pages of the buffer */
typedef struct PageWalk
{
	Relation	index;
	Size		hs;
	const SiftColSchema *schema;
} PageWalk;

static int
collect_rows(void *arg, Page page, Size *offs, ItemPointerData *tids)
{
	const PageWalk *w = arg;
	PageCursor	c;
	int			n = 0;

	wbuf_cursor_init(&c, w->index, w->hs);
	wbuf_cursor_set(&c, page);
	while (c.pos < c.used)
	{
		Size		size;
		const SiftColRecHead *h = wbuf_rec_at(&c, &size);

		if (!(h->flags & (SIFT_COLR_CONT | SIFT_COLR_DELETED)))
		{
			offs[n] = c.hs + c.pos;
			tids[n++] = h->tid;
		}
		c.pos += size;
	}
	return n;
}

static void
flag_row(Page page, Size off)
{
	((SiftColRecHead *) (SiftPageGetData(page) + off))->flags |= SIFT_COLR_DELETED;
}

int64
sift_cwbuf_bulkdelete(Relation index, const SiftSegEntry *entry, const SiftColSchema *schema, IndexBulkDeleteCallback cb,
					 void *cbstate, IndexBulkDeleteResult *stats)
{
	Page		page = palloc(BLCKSZ);
	PageWalk	w = {index, wbuf_hs(entry->format), schema};
	int64		removed;

	wbuf_flush_for_read(index);
	removed = sift_lane_bulkdelete(index, entry, w.hs, RECSIZE(0), page, collect_rows, flag_row, &w, cb, cbstate, stats);
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

/* A record whole on its page, the values in it: its key is in the range of the page, the leaf of which the page names */
static void
verify_key(const PageWalk *w, Page page, const SiftColRecHead *h, BlockNumber blk)
{
	const SiftColPageHead *ph = SiftColPageKeys(page);
	int			leaf = (int) ((ph->lane.flags & SIFT_WBUFPAGE_LEAF_MASK) >> SIFT_WBUFPAGE_LEAF_SHIFT) - 1;
	WbufKey		key;

	if (leaf < 0 || leaf >= w->schema->ncols)
		SIFT_CORRUPT(w->index, "Write buffer block %u holds rows and names key leaf %d of %d.", blk, leaf, w->schema->ncols);
	wbuf_payload_key(w->schema, leaf, h->flags, (const char *) (h + 1), h->len, &key);
	if (key.has && (!(ph->lane.flags & SIFT_WBUFPAGE_KEYS) || key.v < ph->kmin || key.v > ph->kmax))
		SIFT_CORRUPT(w->index, "Write buffer block %u holds a row with key " INT64_FORMAT " and its range is " INT64_FORMAT " to " INT64_FORMAT ".", blk, key.v,
					 (ph->lane.flags & SIFT_WBUFPAGE_KEYS) ? ph->kmin : 0, (ph->lane.flags & SIFT_WBUFPAGE_KEYS) ? ph->kmax : 0);
}

static void
verify_page(void *arg, Page page, BlockNumber blk)
{
	const PageWalk *w = arg;
	Relation	index = w->index;
	PageCursor	c;
	uint64		nrecs = 0;

	wbuf_cursor_init(&c, index, w->hs);
	wbuf_cursor_set(&c, page);
	while (c.pos < c.used)
	{
		Size		size;
		const SiftColRecHead *h = wbuf_rec_at(&c, &size);

		if (!ItemPointerIsValid(&h->tid) || (h->flags & ~(SIFT_COLR_DELETED | SIFT_COLR_CONT | SIFT_COLR_XMIN | SIFT_COLR_REF)))
			SIFT_CORRUPT(index, "A write buffer record has tid (%u,%u) and flags %x.", ItemPointerGetBlockNumberNoCheck(&h->tid),
					ItemPointerGetOffsetNumberNoCheck(&h->tid), h->flags);
		nrecs += !(h->flags & SIFT_COLR_CONT);
		if (w->hs == sizeof(SiftColPageHead) && !(h->flags & (SIFT_COLR_CONT | SIFT_COLR_REF)) && !(SiftLaneHead(page)->flags & SIFT_WBUFPAGE_KEYUNK) &&
			!((SiftLaneHead(page)->flags & SIFT_WBUFPAGE_SPANS) && c.pos + size == c.used))
			verify_key(w, page, h, blk);
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
	PageWalk	w = {index, wbuf_hs(entry->format), schema};

	wbuf_flush_for_read(index);
	sift_lane_verify(index, entry, w.hs, verify_page, &w);
	r = sift_cwbuf_open(index, entry, schema, CurrentMemoryContext);
	while (sift_cwbuf_next(r, &row, &deleted))
		live += !deleted;
	sift_cwbuf_close(r);
	return live;
}
