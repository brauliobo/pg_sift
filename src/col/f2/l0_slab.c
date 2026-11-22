/*
 * col/l0_slab.c - the OLTP micro-slab of a partition (col/l0_slab.h).
 */
#include "core/host.h"

#include "access/xlog.h"
#include "lib/stringinfo.h"
#include "storage/lmgr.h"

#include "col/f2/f2_cache.h"
#include "col/f2/compact.h"
#include "col/f2/l0_href_fetch.h"
#include "col/f2/l0_slab.h"
#include "core/damage.h"
#include "core/page_io.h"

#define SL(p)	((SiftL0SlabPageData *) SiftPageGetData(p))
#define SLAB_ROOM	(SIFT_PAGE_PAYLOAD - sizeof(SiftL0SlabPageData))

static void
slab_seal(Page page, uint32 used)
{
	SiftPageSetUsed(page, used);
	sift_page_seal(page);
}

static void
slab_init(Page page, uint64 gen, uint32 part_id, BlockNumber next)
{
	sift_page_init_part(page, SIFT_PT_L0_SLAB, gen, part_id);
	SL(page)->magic = SIFT_L0_SLAB_MAGIC;
	SL(page)->next_blk = next;
	SL(page)->min_xid = SL(page)->max_xid = InvalidFullTransactionId;
}

/* The page of a chain, checked: share-locked to read it, exclusively to change it */
static Buffer
lock_slab(Relation rel, BlockNumber blk, uint32 part_id, int lockmode)
{
	Buffer		buf = sift_blk_read(rel, blk, lockmode);
	Page		page = BufferGetPage(buf);

	sift_page_expect(rel, page, blk, SIFT_PT_L0_SLAB, 0, part_id);
	if (SL(page)->magic != SIFT_L0_SLAB_MAGIC || !sift_page_sum_ok(page) || SiftPageGetUsed(page) < sizeof(SiftL0SlabPageData))
		SIFT_CORRUPT(rel, "Block %u: the slab of partition %u is damaged.", blk, part_id);
	return buf;
}

static void
widen(Page page, const SiftL0BatchHeader *b)
{
	FullTransactionId f = sift_xid_to_fxid(b->xmin);

	if (!FullTransactionIdIsValid(SL(page)->min_xid) || FullTransactionIdPrecedes(f, SL(page)->min_xid))
		SL(page)->min_xid = f;
	if (!FullTransactionIdIsValid(SL(page)->max_xid) || FullTransactionIdFollows(f, SL(page)->max_xid))
		SL(page)->max_xid = f;
}

static void
add_batch(Page page, const char *rec)
{
	const SiftL0BatchHeader *b = (const SiftL0BatchHeader *) rec;
	uint32		used = SiftPageGetUsed(page);

	memcpy(SiftPageGetData(page) + used, rec, b->bytes);
	SL(page)->nbatches++;
	SL(page)->page_rows += b->nrows;
	widen(page, b);
	slab_seal(page, used + b->bytes);
}


/* A compact batch: after its header, for each row the address (the first raw, then the zigzag difference to the previous), two bits for the state of each column of the row (a value, a null, a null that keeps a tag), the values, and the elements of its arrays, each behind a state byte */
enum
{
	CELL_VALUE,
	CELL_NULL,
	CELL_TAGGED
};

static inline uint64
zigzag(int64 v)
{
	return ((uint64) v << 1) ^ (uint64) (v >> 63);
}

static inline int64
unzigzag(uint64 v)
{
	return (int64) (v >> 1) ^ -(int64) (v & 1);
}

static void
put_varint(StringInfo s, uint64 v)
{
	for (; v >= 0x80; v >>= 7)
		appendStringInfoChar(s, (char) (v | 0x80));
	appendStringInfoChar(s, (char) v);
}

static uint64
get_varint(const char **p, const char *end)
{
	uint64		v = 0;

	for (int shift = 0; shift < 64; shift += 7)
	{
		uint8		b;

		if (*p >= end)
			break;
		b = (uint8) *(*p)++;
		v |= (uint64) (b & 0x7F) << shift;
		if (!(b & 0x80))
			return v;
	}
	SIFT_DAMAGE_DATA(NULL, errmsg("sift: a compact slab batch ends inside a number"));
}

static int
cell_state(const SiftCell *c)
{
	return !c->isnull ? CELL_VALUE : c->tag != 0 ? CELL_TAGGED : CELL_NULL;
}

static void
put_value(StringInfo s, SiftCellKind kind, const SiftCell *c)
{
	if (kind == SIFT_CELL_INT)
		put_varint(s, zigzag(c->i));
	else if (kind == SIFT_CELL_F8)
		appendBinaryStringInfo(s, (const char *) &c->f, sizeof(double));
	else
	{
		put_varint(s, c->slen);
		appendBinaryStringInfo(s, c->s, c->slen);
	}
}

static void
put_tag(StringInfo s, const SiftCell *c)
{
	appendStringInfoChar(s, (char) c->tag);
	put_varint(s, c->xp != NULL ? (uint64) c->xplen + 1 : 0);
	if (c->xp != NULL)
		appendBinaryStringInfo(s, c->xp, c->xplen);
}

static void
get_cell(const char **p, const char *end, SiftCellKind kind, int state, SiftCell *c)
{
	*c = (SiftCell) {.isnull = state != CELL_VALUE};
	if (state == CELL_VALUE && kind == SIFT_CELL_INT)
		c->i = unzigzag(get_varint(p, end));
	else if (state == CELL_VALUE && kind == SIFT_CELL_F8)
	{
		memcpy(&c->f, *p, sizeof(double));
		*p += sizeof(double);
	}
	else if (state == CELL_VALUE)
	{
		c->slen = (uint32) get_varint(p, end);
		c->s = *p;
		*p += c->slen;
	}
	else if (state == CELL_TAGGED)
	{
		uint64		xp;

		c->tag = (uint8) *(*p)++;
		xp = get_varint(p, end);
		if (xp > 0)
		{
			c->xplen = (uint32) (xp - 1);
			c->xp = *p;
			*p += c->xplen;
		}
	}
	if (*p > end)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a compact slab batch ends inside a value"));
}

static void
put_row_compact(StringInfo s, const SiftRowBuf *rb, int r)
{
	int			ntop = 0;
	char	   *mask;
	int			masklen;

	for (int c = 0; c < rb->ncols; c++)
		ntop += !sift_rowbuf_is_child(rb, c);
	masklen = (2 * ntop + 7) / 8;
	mask = palloc0(masklen);
	for (int c = 0, k = 0; c < rb->ncols; c++)
		if (!sift_rowbuf_is_child(rb, c))
		{
			SiftCell	cell = sift_rowbuf_cell(rb, c, r);

			mask[(2 * k) / 8] |= (char) (cell_state(&cell) << ((2 * k) % 8));
			k++;
		}
	appendBinaryStringInfo(s, mask, masklen);
	for (int c = 0; c < rb->ncols; c++)
		if (!sift_rowbuf_is_child(rb, c))
		{
			SiftCell	cell = sift_rowbuf_cell(rb, c, r);

			if (cell.isnull && cell.tag != 0)
				put_tag(s, &cell);
			else if (!cell.isnull)
				put_value(s, rb->kinds[c], &cell);
		}
	for (int g = 0; g < rb->ncols; g++)
	{
		if (!sift_rowbuf_is_group(rb, g))
			continue;
		for (int c = 0; c < rb->ncols; c++)
			if (rb->parent[c] == g)
				for (uint32 j = rb->estart[g][r]; j < rb->estart[g][r + 1]; j++)
				{
					SiftCell	cell = sift_rowbuf_cell(rb, c, j);

					appendStringInfoChar(s, (char) cell_state(&cell));
					if (cell.isnull && cell.tag != 0)
						put_tag(s, &cell);
					else if (!cell.isnull)
						put_value(s, rb->kinds[c], &cell);
				}
	}
}

/* The record of rows [from, to) as a compact batch */
static StringInfo
encode_compact(const SiftRowBuf *rb, int from, int to)
{
	StringInfo	s = makeStringInfo();
	SiftL0BatchHeader *h;

	enlargeStringInfo(s, SIFT_L0_COMPACT_HDR);
	s->len = SIFT_L0_COMPACT_HDR;
	memset(s->data, 0, SIFT_L0_COMPACT_HDR);
	for (int r = from; r < to; r++)
	{
		if (r == from)
			appendBinaryStringInfo(s, (const char *) &rb->tid[r], sizeof(uint64));
		else
			put_varint(s, zigzag((int64) (rb->tid[r] - rb->tid[r - 1])));
		put_row_compact(s, rb, r);
	}
	while (s->len % 8 != 0)
		appendStringInfoChar(s, '\0');
	h = (SiftL0BatchHeader *) s->data;
	h->nrows = to - from;
	h->flags = SIFT_L0_BATCH_ROW_COMPACT;
	h->bytes = s->len;
	h->xmin = rb->xmin[from];
	h->cmin = rb->cmin[from];
	return s;
}

static const char *
get_row_compact(const char *p, const char *end, SiftRowBuf *rb, const SiftPartSpec *spec, const SiftL0BatchHeader *b, uint64 *tid, bool first)
{
	SiftCell   *cells = palloc0(Max(rb->ncols, 1) * sizeof(SiftCell));
	SiftCell  **elem = palloc0(Max(rb->ncols, 1) * sizeof(SiftCell *));
	const uint8 *mask;
	uint8		key[SIFT_BOUND_BYTES];
	int			ntop = 0;

	if (first)
	{
		memcpy(tid, p, sizeof(uint64));
		p += sizeof(uint64);
	}
	else
		*tid += (uint64) unzigzag(get_varint(&p, end));
	for (int c = 0; c < rb->ncols; c++)
		ntop += !sift_rowbuf_is_child(rb, c);
	mask = (const uint8 *) p;
	p += (2 * ntop + 7) / 8;
	for (int c = 0, k = 0; c < rb->ncols; c++)
		if (!sift_rowbuf_is_child(rb, c))
		{
			get_cell(&p, end, rb->kinds[c], (mask[(2 * k) / 8] >> ((2 * k) % 8)) & 3, &cells[c]);
			k++;
		}
	for (int g = 0; g < rb->ncols; g++)
	{
		uint32		cnt = cells[g].isnull ? 0 : (uint32) cells[g].i;

		if (!sift_rowbuf_is_group(rb, g))
			continue;
		for (int c = 0; c < rb->ncols; c++)
			if (rb->parent[c] == g)
			{
				elem[c] = palloc(Max(cnt, 1) * sizeof(SiftCell));
				for (uint32 j = 0; j < cnt; j++)
				{
					int			state = (uint8) *p++;

					get_cell(&p, end, rb->kinds[c], state, &elem[c][j]);
				}
			}
	}
	sift_row_key(spec, cells, *tid, key);
	sift_rowbuf_add(rb, *tid, b->xmin, b->cmin, 0, key, cells, (const SiftCell *const *) elem);
	for (int c = 0; c < rb->ncols; c++)
		if (elem[c] != NULL)
			pfree(elem[c]);
	pfree(elem);
	pfree(cells);
	return p;
}

static void
read_compact(SiftRowBuf *rb, const SiftPartSpec *spec, const SiftL0BatchHeader *b)
{
	const char *p = (const char *) b + SIFT_L0_COMPACT_HDR,
			   *end = (const char *) b + b->bytes;
	uint64		tid = 0;

	for (int r = 0; r < b->nrows; r++)
		p = get_row_compact(p, end, rb, spec, b, &tid, r == 0);
}

/* The batches of rows [0, n) of rb, one for each run of the same (xmin, cmin) */
static void
encode_batches(const SiftRowBuf *rb, StringInfo *recs, int *nrecs)
{
	SiftColIn  *cols = palloc(Max(rb->ncols, 1) * sizeof(SiftColIn));
	SiftColIn  *gcols = palloc(Max(rb->ncols, 1) * sizeof(SiftColIn));
	SiftRows	all = {0};
	bool		have_all = false;

	*nrecs = 0;
	for (int from = 0; from < rb->n;)
	{
		int			to = from + 1,
					room;
		StringInfo	s;
		SiftL0BatchHeader *h;
		SiftGranuleIn gi;
		uint8		lo[SIFT_BOUND_BYTES],
					hi[SIFT_BOUND_BYTES];

		while (to < rb->n && rb->xmin[to] == rb->xmin[from] && rb->cmin[to] == rb->cmin[from])
			to++;
		if (to - from < SIFT_L0_COMPACT_ROWS)
		{
			recs[(*nrecs)++] = encode_compact(rb, from, to);
			from = to;
			continue;
		}
		if (!have_all)
		{
			all = sift_rowbuf_rows((SiftRowBuf *) rb, cols, &(SiftPartSpec) {.ncols = rb->ncols});
			have_all = true;
		}
		room = sift_rows_granule_len(&all, from);
		to = Min(to, from + room);
		s = makeStringInfo();
		for (int c = 0; c < rb->ncols; c++)
			gcols[c] = sift_colin_slice(&all.cols[c], all.cols[c].group1 > 0 ? (int) all.cols[all.cols[c].group1 - 1].eoff[from] : from);
		gi = (SiftGranuleIn) {.nrows = to - from,.ncols = rb->ncols,.tid = rb->tid + from,.cols = gcols,.min_xid = InvalidFullTransactionId,
		.max_xid = InvalidFullTransactionId};
		for (uint32 i = 0; i < sizeof(SiftL0BatchHeader); i++)
			appendStringInfoChar(s, '\0');
		(void) sift_granule_encode(s, &gi);
		memcpy(lo, rb->keys[from], SIFT_BOUND_BYTES);
		memcpy(hi, rb->keys[from], SIFT_BOUND_BYTES);
		for (int r = from + 1; r < to; r++)
		{
			if (memcmp(rb->keys[r], lo, SIFT_BOUND_BYTES) < 0)
				memcpy(lo, rb->keys[r], SIFT_BOUND_BYTES);
			if (memcmp(rb->keys[r], hi, SIFT_BOUND_BYTES) > 0)
				memcpy(hi, rb->keys[r], SIFT_BOUND_BYTES);
		}
		h = (SiftL0BatchHeader *) s->data;
		h->nrows = to - from;
		h->bytes = s->len;
		h->xmin = rb->xmin[from];
		h->cmin = rb->cmin[from];
		memcpy(h->key_lo, lo, SIFT_BOUND_BYTES);
		memcpy(h->key_hi, hi, SIFT_BOUND_BYTES);
		recs[(*nrecs)++] = s;
		from = to;
	}
}

/* What the chain at head holds: its tail, pages and rows; true when its tail is sealed, so that it takes no more batches before it is flushed */
static bool
chain_totals(Relation rel, uint32 part_id, BlockNumber head, BlockNumber *tail, int *pages, uint32 *rows)
{
	bool		sealed = false;

	*pages = 0;
	*rows = 0;
	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Buffer		buf = lock_slab(rel, blk, part_id, BUFFER_LOCK_SHARE);

		*tail = blk;
		(*pages)++;
		*rows += SL(BufferGetPage(buf))->page_rows;
		sealed = (SL(BufferGetPage(buf))->flags & SIFT_L0_SLAB_SEALED) != 0;
		blk = SL(BufferGetPage(buf))->next_blk;
		UnlockReleaseBuffer(buf);
	}
	return sealed;
}

uint64
sift_l0_slab_rows(Relation rel, uint64 part_id, BlockNumber head)
{
	BlockNumber tail;
	int			pages;
	uint32		rows;

	(void) chain_totals(rel, (uint32) part_id, head, &tail, &pages, &rows);
	return rows;
}

SiftChainStamp
sift_l0_slab_stamp(Relation rel, uint64 part_id, BlockNumber head)
{
	SiftChainStamp st = {0};

	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Buffer		buf = lock_slab(rel, blk, (uint32) part_id, BUFFER_LOCK_SHARE);
		const SiftL0SlabPageData *sl = SL(BufferGetPage(buf));

		st.lsn += PageGetLSN(BufferGetPage(buf));
		st.count += sl->page_rows;
		st.pages++;
		if (!FullTransactionIdIsValid(st.max_xid) || FullTransactionIdFollows(sl->max_xid, st.max_xid))
			st.max_xid = sl->max_xid;
		blk = sl->next_blk;
		UnlockReleaseBuffer(buf);
	}
	return st;
}

static bool
batch_fits(Page page, const char *rec)
{
	return SiftPageGetUsed(page) + ((const SiftL0BatchHeader *) rec)->bytes <= SIFT_PAGE_PAYLOAD;
}

/* Adds the batch to the tail page of the chain (pages, nrows: what the chain holds before it), which is sealed when the chain has reached its limits */
static void
place_batch(Page pg, const char *rec, int pages, uint32 nrows)
{
	add_batch(pg, rec);
	if (pages >= SIFT_L0_SLAB_MAX_PAGES || nrows + ((const SiftL0BatchHeader *) rec)->nrows >= SIFT_L0_SLAB_FLUSH_ROWS)
	{
		SL(pg)->flags |= SIFT_L0_SLAB_SEALED;
		slab_seal(pg, SiftPageGetUsed(pg));
	}
}

/* The chain at head (none: empty) is not sealed and nbatches more pages keep it within SIFT_L0_SLAB_MAX_PAGES */
static bool
chain_has_room(Relation rel, uint32 part_id, BlockNumber head, int nbatches)
{
	BlockNumber tail;
	int			pages = 0;
	uint32		nrows;

	return (head == InvalidBlockNumber || !chain_totals(rel, part_id, head, &tail, &pages, &nrows)) && pages + nbatches <= SIFT_L0_SLAB_MAX_PAGES;
}

/*
 * A chain that holds as many rows or pages as it may is written out as an L0 part before the rows go on, so that the micro-slab turns over every SIFT_L0_SLAB_FLUSH_ROWS rows and the
 * commits that follow land in the fresh slab, where a transaction that found the chain full wrote a part and a manifest commit of its own. The caller holds the lock of the partition.
 */
static bool
rotate_full_chain(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPartSpec *spec, BlockNumber head, int nbatches)
{
	if (head == InvalidBlockNumber || chain_has_room(rel, (uint32) part_id, head, nbatches))
		return false;
	return sift_l0_slab_flush(rel, part_id, key_lo, spec);
}

bool
sift_l0_slab_place(SiftXlog *x, Relation rel, uint64 part_id, BlockNumber head, const SiftRowBuf *rows)
{
	/* check-memory: allow the rows of one transaction batch, which work_mem bounds */
	StringInfo *recs = palloc((rows->n + 1) * sizeof(StringInfo));
	int			nrecs,
				pages = 0;
	uint32		nrows = 0;
	BlockNumber tail = InvalidBlockNumber;
	Buffer		buf;

	encode_batches(rows, recs, &nrecs);
	if (nrecs != 1 || chain_totals(rel, (uint32) part_id, head, &tail, &pages, &nrows))
		return false;
	buf = lock_slab(rel, tail, (uint32) part_id, BUFFER_LOCK_EXCLUSIVE);
	if (!batch_fits(BufferGetPage(buf), recs[0]->data))
	{
		UnlockReleaseBuffer(buf);
		return false;
	}
	place_batch(sift_xlog_register(x, buf, 0), recs[0]->data, pages, nrows);
	return true;
}

bool
sift_l0_slab_first(SiftXlog *x, Relation rel, uint64 part_id, const SiftRowBuf *rows, BlockNumber *blk)
{
	/* check-memory: allow the rows of one transaction batch, which work_mem bounds */
	StringInfo *recs = palloc((rows->n + 1) * sizeof(StringInfo));
	int			nrecs;
	Page		pg;

	encode_batches(rows, recs, &nrecs);
	if (nrecs != 1 || recs[0]->len > SLAB_ROOM)
		return false;
	*blk = sift_fsm_alloc_meta_page(rel);
	pg = sift_xlog_register(x, sift_blk_new(rel, *blk), GENERIC_XLOG_FULL_IMAGE);
	slab_init(pg, sift_manifest_new_gen(rel, 1), (uint32) part_id, InvalidBlockNumber);
	slab_seal(pg, sizeof(SiftL0SlabPageData));
	add_batch(pg, recs[0]->data);
	return true;
}

bool
sift_l0_slab_append(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftRowBuf *rows, const SiftPartSpec *spec)
{
	/* check-memory: allow the rows of one transaction batch, which work_mem bounds */
	StringInfo *recs = palloc((rows->n + 1) * sizeof(StringInfo));
	int			nrecs;
	SiftManifestRead mr;
	BlockNumber head,
				tail = InvalidBlockNumber;

	encode_batches(rows, recs, &nrecs);
	for (int i = 0; i < nrecs; i++)
		if (recs[i]->len > SLAB_ROOM)
			return false;
	LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	mr = sift_manifest_read_range(rel, key_lo, NULL);
	if (mr.n == 0 || mr.entries[0].part_id != part_id)
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) part_id)));
	head = mr.entries[0].l0_slab_blk;
	if (rotate_full_chain(rel, part_id, key_lo, spec, head, nrecs))
		head = sift_manifest_read_range(rel, key_lo, NULL).entries[0].l0_slab_blk;
	if (!chain_has_room(rel, (uint32) part_id, head, nrecs))
	{
		UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
		return false;
	}
	for (int i = 0; i < nrecs; i++)
	{
		int			pages = 0;
		uint32		nrows = 0;
		bool		placed = false;

		if (head != InvalidBlockNumber)
		{
			Buffer		buf;
			Page		pg;
			SiftXlog	x;

			(void) chain_totals(rel, (uint32) part_id, head, &tail, &pages, &nrows);
			buf = lock_slab(rel, tail, (uint32) part_id, BUFFER_LOCK_EXCLUSIVE);
			x = sift_xlog_start(rel);
			pg = sift_xlog_register(&x, buf, 0);
			if (batch_fits(pg, recs[i]->data))
			{
				place_batch(pg, recs[i]->data, pages, nrows);
				sift_xlog_done(&x);
				placed = true;
			}
			else
			{
				sift_xlog_abort(&x);
				UnlockReleaseBuffer(buf);
			}
		}
		if (!placed)
		{
			BlockNumber blk = sift_fsm_alloc_meta_page(rel);
			uint64		gen = sift_manifest_new_gen(rel, 1);
			SiftXlog	x = sift_xlog_start(rel);
			Page		pg = sift_xlog_register(&x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE);

			slab_init(pg, gen, (uint32) part_id, InvalidBlockNumber);
			slab_seal(pg, sizeof(SiftL0SlabPageData));
			add_batch(pg, recs[i]->data);
			sift_xlog_done(&x);
			if (head == InvalidBlockNumber)
			{
				SiftManifestEdit e = {.part_id = part_id,.key_lo = key_lo,.set_l0_slab = true,.new_l0_slab_blk = blk};

				sift_manifest_commit(rel, &e);
				head = blk;
			}
			else
			{
				Buffer		buf = lock_slab(rel, tail, (uint32) part_id, BUFFER_LOCK_EXCLUSIVE);
				SiftXlog	x2 = sift_xlog_start(rel);
				Page		pg2 = sift_xlog_register(&x2, buf, 0);

				SL(pg2)->next_blk = blk;
				SL(pg2)->flags |= SIFT_L0_SLAB_SEALED;
				slab_seal(pg2, SiftPageGetUsed(pg2));
				sift_xlog_done(&x2);
			}
		}
	}
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	return true;
}

bool
sift_l0_slab_append_deleting(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftRowBuf *rows, const SiftPendDelRec *dels, int ndel, const SiftPartSpec *spec)
{
	BlockNumber lock = SIFT_PART_LOCK_BLKNO(part_id);
	const SiftPartitionEntry *e;
	SiftManifestEdit edit = {.part_id = part_id,.key_lo = key_lo};
	SiftXlog	x;
	bool		fused;

	LockPage(rel, lock, ExclusiveLock);
	e = sift_plan_entry(sift_plan_cache_get(rel, CurrentMemoryContext), part_id);
	if (rotate_full_chain(rel, part_id, key_lo, spec, e->l0_slab_blk, 1))
		e = sift_plan_entry(sift_plan_cache_get(rel, CurrentMemoryContext), part_id);
	x = sift_xlog_start(rel);
	fused = e->pend_del_blk == InvalidBlockNumber || sift_pend_del_place(&x, rel, part_id, e->pend_del_blk, dels, ndel);
	if (fused && e->l0_slab_blk == InvalidBlockNumber)
	{
		edit.set_l0_slab = true;
		fused = sift_l0_slab_first(&x, rel, part_id, rows, &edit.new_l0_slab_blk);
	}
	else if (fused)
		fused = sift_l0_slab_place(&x, rel, part_id, e->l0_slab_blk, rows);
	if (fused && e->pend_del_blk == InvalidBlockNumber)
	{
		edit.set_pend_del = true;
		edit.new_pend_del_blk = sift_pend_del_first(&x, rel, part_id, dels, ndel);
	}
	if (fused)
	{
		sift_xlog_done(&x);
		if (edit.set_l0_slab || edit.set_pend_del)
			sift_manifest_commit(rel, &edit);
	}
	else
	{
		sift_xlog_abort(&x);
		for (int i = 0; i < x.n; i++)
			UnlockReleaseBuffer(x.buf[i]);
	}
	UnlockPage(rel, lock, ExclusiveLock);
	return fused;
}

static void
read_batches(Relation rel, uint64 part_id, BlockNumber head, SiftRowBuf *rb, const SiftPartSpec *spec, BlockNumber **pages, int *npages)
{
	int			np = 0,
				cap = SIFT_L0_SLAB_MAX_PAGES;

	if (pages != NULL)
		*pages = palloc(cap * sizeof(BlockNumber));
	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Buffer		buf = lock_slab(rel, blk, (uint32) part_id, BUFFER_LOCK_SHARE);
		Page		pg = BufferGetPage(buf);
		uint32		used = SiftPageGetUsed(pg),
					off = sizeof(SiftL0SlabPageData);

		for (int i = 0; i < SL(pg)->nbatches; i++)
		{
			const SiftL0BatchHeader *b = (const SiftL0BatchHeader *) (SiftPageGetData(pg) + off);
			SiftGranuleView v;
			SiftRowVer *vers;
			bool		compact;

			if (off + SIFT_L0_COMPACT_HDR > used)
				SIFT_CORRUPT(rel, "Block %u: batch %d of the slab is outside the page.", blk, i);
			compact = (b->flags & SIFT_L0_BATCH_ROW_COMPACT) != 0;
			if (b->bytes % 8 != 0 || off + b->bytes > used || b->bytes <= (compact ? SIFT_L0_COMPACT_HDR : sizeof(SiftL0BatchHeader)))
				SIFT_CORRUPT(rel, "Block %u: batch %d of the slab is outside the page.", blk, i);
			if (compact)
			{
				read_compact(rb, spec, b);
				off += b->bytes;
				continue;
			}
			v = sift_granule_open(rel, (const char *) b + sizeof(SiftL0BatchHeader), b->bytes - sizeof(SiftL0BatchHeader), NULL);
			/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
			vers = palloc(b->nrows * sizeof(SiftRowVer));
			for (int r = 0; r < b->nrows; r++)
				vers[r] = (SiftRowVer) {b->xmin, b->cmin};
			sift_rowbuf_add_granule(rb, &v, vers, spec, NULL);
			off += b->bytes;
		}
		if (pages != NULL)
		{
			if (np == cap)
				*pages = repalloc(*pages, (cap *= 2) * sizeof(BlockNumber));
			(*pages)[np] = blk;
		}
		np++;
		blk = SL(pg)->next_blk;
		UnlockReleaseBuffer(buf);
	}
	if (npages != NULL)
		*npages = np;
}

void
sift_l0_slab_read(Relation rel, uint64 part_id, BlockNumber head, SiftRowBuf *rb, const SiftPartSpec *spec, Snapshot snap, BlockNumber **pages, int *npages)
{
	if (sift_l0_slab_is_href(rel, part_id, head))
		sift_l0_href_read(rel, part_id, head, rb, spec, snap, NULL, pages, npages);
	else
		read_batches(rel, part_id, head, rb, spec, pages, npages);
}

void
sift_l0_slab_publish(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPartSpec *spec, SiftRowBuf *rb, const BlockNumber *pages, int npages)
{
	SiftColIn  *cols = palloc(Max(spec->ncols, 1) * sizeof(SiftColIn));
	SiftExtent *rel_ext = palloc(Max(npages, 1) * sizeof(SiftExtent));
	SiftPartDesc d;
	SiftManifestEdit e = {.part_id = part_id,.key_lo = key_lo,.set_l0_slab = true,.new_l0_slab_blk = InvalidBlockNumber,.add_parts = &d,.nadd = rb->n > 0};

	if (rb->n > 0)
	{
		SiftCompressTier tier = rb->n >= SIFT_L0_TO_L1_DIRECT_ROWS ? SIFT_TIER_WARM_L1 : SIFT_TIER_HOT_L0;
		SiftRows	r;

		sift_rowbuf_sort(rb, spec);
		sift_rowbuf_cluster(rb, spec, tier);
		r = sift_rowbuf_rows(rb, cols, spec);
		d = sift_part_write_rows(rel, (uint32) part_id, sift_manifest_new_gen(rel, 1), rb->n >= SIFT_L0_TO_L1_DIRECT_ROWS ? 1 : 0, tier, &r);
	}
	for (int i = 0; i < npages; i++)
		rel_ext[i] = (SiftExtent) {pages[i], 1};
	e.release = rel_ext;
	e.nrelease = npages;
	sift_manifest_commit(rel, &e);
	pfree(rel_ext);
	pfree(cols);
}

bool
sift_l0_slab_flush(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPartSpec *spec)
{
	SiftManifestRead mr;
	SiftRowBuf *rb;
	BlockNumber *pages;
	int			npages;

	SIFT_INVARIANT(!RecoveryInProgress(), "sift: a micro-slab is flushed on a primary, a standby reads it in memory (col/f2/f2_slab_view.h)");
	if (sift_l0_href_flush(rel, part_id, key_lo, spec))
		return true;
	LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	mr = sift_manifest_read_range(rel, key_lo, NULL);
	if (mr.n == 0 || mr.entries[0].part_id != part_id)
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) part_id)));
	if (mr.entries[0].l0_slab_blk == InvalidBlockNumber)
	{
		UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
		return false;
	}
	rb = sift_rowbuf_new(spec);
	read_batches(rel, part_id, mr.entries[0].l0_slab_blk, rb, spec, &pages, &npages);
	sift_l0_slab_publish(rel, part_id, key_lo, spec, rb, pages, npages);
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	sift_rowbuf_free(rb);
	return true;
}
