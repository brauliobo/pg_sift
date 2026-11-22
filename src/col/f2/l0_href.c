/*
 * col/f2/l0_href.c - the heap-reference L0 slab (col/f2/l0_href.h).
 */
#include "core/host.h"

#include "col/f2/l0_href.h"
#include "col/f2/l0_shard_dir.h"
#include "core/f2/meta.h"
#include "core/xid_cache.h"

#ifndef SIFT_HREF_STANDALONE
#include "storage/lmgr.h"

#include "col/f2/l0_href_fetch.h"
#include "core/f2/fsm.h"
#include "core/f2/manifest.h"
#include "core/page_io.h"
#include "core/part_lock.h"
#endif

#define SL(pg)	((SiftL0SlabPageData *) SiftPageGetData(pg))

FullTransactionId
sift_l0_href_fxid(TransactionId xid)
{
	return sift_xid_fxid_at(ReadNextFullTransactionId(), xid);
}

void
sift_l0_href_format(char *pg, BlockNumber prev, uint16 chain_pages, uint32 chain_rows)
{
	SiftL0SlabPageData *sl = SL(pg);

	Assert(sift_page_layout_ok(pg));
	memset(sl, 0, sizeof(*sl));
	sl->magic = SIFT_L0_SLAB_MAGIC;
	sl->flags = SIFT_L0_SLAB_REV | SIFT_L0_SLAB_HEAPREF | SIFT_L0_SLAB_NOCRC_OPEN;
	sl->prev_blk = prev;
	sl->chain_pages = chain_pages;
	sl->chain_rows = chain_rows;
	sl->min_xid = sl->max_xid = InvalidFullTransactionId;
	SiftPageSetUsed(pg, sizeof(*sl));
}

void
sift_l0_href_add(char *pg, const SiftL0HeapRef *refs, uint32 k)
{
	SiftL0SlabPageData *sl = SL(pg);
	FullTransactionId next = ReadNextFullTransactionId(),
				lo = InvalidFullTransactionId,
				hi = InvalidFullTransactionId;

	Assert(k > 0 && k <= sift_l0_href_room(pg) && (sl->flags & SIFT_L0_SLAB_NOCRC_OPEN) != 0);
	memcpy(SIFT_L0_HREF_PTR(pg) + sl->page_rows, refs, k * sizeof(SiftL0HeapRef));
	for (uint32 i = 0; i < k; i++)
	{
		FullTransactionId f;

		if (!TransactionIdIsNormal(refs[i].xmin))
			continue;
		f = sift_xid_fxid_at(next, refs[i].xmin);
		if (!FullTransactionIdIsValid(lo) || FullTransactionIdPrecedes(f, lo))
			lo = f;
		if (!FullTransactionIdIsValid(hi) || FullTransactionIdFollows(f, hi))
			hi = f;
	}
	if (FullTransactionIdIsValid(lo) && (!FullTransactionIdIsValid(sl->min_xid) || FullTransactionIdPrecedes(lo, sl->min_xid)))
		sl->min_xid = lo;
	if (FullTransactionIdIsValid(hi) && (!FullTransactionIdIsValid(sl->max_xid) || FullTransactionIdFollows(hi, sl->max_xid)))
		sl->max_xid = hi;
	sl->page_rows += (uint16) k;
	sl->chain_rows += k;
	SiftPageSetUsed(pg, sizeof(*sl) + sl->page_rows * sizeof(SiftL0HeapRef));
}

void
sift_l0_href_seal(char *pg)
{
	SL(pg)->flags = (SL(pg)->flags | SIFT_L0_SLAB_SEALED) & ~SIFT_L0_SLAB_NOCRC_OPEN;
	sift_page_seal(pg);
}

const char *
sift_l0_href_verify(const char *pg)
{
	const SiftL0SlabPageData *sl = (const SiftL0SlabPageData *) SiftPageGetData(pg);
	bool		sealed = (sl->flags & SIFT_L0_SLAB_SEALED) != 0;

	if (!sift_page_layout_ok(pg))
		return "the page is not laid out as a page of the index";
	if (SiftPageGetOpaque((Page) pg)->type != SIFT_PT_L0_SLAB || SiftPageGetUsed(pg) < sizeof(*sl) || sl->magic != SIFT_L0_SLAB_MAGIC)
		return "the page is not a slab page";
	if ((sl->flags & (SIFT_L0_SLAB_HEAPREF | SIFT_L0_SLAB_REV | SIFT_L0_SLAB_SHARD_DIR)) != (SIFT_L0_SLAB_HEAPREF | SIFT_L0_SLAB_REV))
		return "the page is not a heap-reference page of a reverse chain";
	if (sl->page_rows > SIFT_L0_HEAPREF_CAP || SiftPageGetUsed(pg) != sizeof(*sl) + sl->page_rows * sizeof(SiftL0HeapRef))
		return "the references of the page do not end where pd_lower says";
	if (sealed ? ((sl->flags & SIFT_L0_SLAB_NOCRC_OPEN) != 0 || !sift_page_sum_ok((Page) pg)) : ((sl->flags & SIFT_L0_SLAB_NOCRC_OPEN) == 0 || sl->crc32c != 0))
		return sealed ? "the checksum of the sealed page is wrong" : "the open page has a checksum or lacks SIFT_L0_SLAB_NOCRC_OPEN";
	for (uint32 i = 0; i < sl->page_rows; i++)
	{
		const SiftL0HeapRef *r = (const SiftL0HeapRef *) (SiftPageGetData(pg) + sizeof(*sl)) + i;

		if (!ItemPointerIsValid(&r->ctid) || (r->flags & ~(SIFT_L0_HREF_MIDTXN | SIFT_L0_HREF_UPDATE_NEW)) != 0)
			return "a reference has no address or a reserved flag";
	}
	return NULL;
}

BlockNumber
sift_l0_href_first(const SiftL0PageIO *io, const SiftL0HeapRef *refs, uint32 n)
{
	BlockNumber blk = io->alloc(io->ctx);
	char	   *pg = io->record_page(io->ctx, blk, true);

	sift_l0_href_format(pg, InvalidBlockNumber, 1, 0);
	sift_l0_href_add(pg, refs, n);
	return blk;
}

BlockNumber
sift_l0_href_chain_append(const SiftL0PageIO *io, BlockNumber tail, const SiftL0HeapRef *refs, uint32 n)
{
	while (n > 0)
	{
		BlockNumber last_blk = tail;
		char	   *last;
		uint32		take;
		int			pages = 1;

		io->record_begin(io->ctx);
		last = io->record_page(io->ctx, tail, false);
		take = Min(n, sift_l0_href_room(last));
		if (take > 0)
			sift_l0_href_add(last, refs, take);
		refs += take;
		n -= take;
		while (n > 0 && pages < SIFT_L0_REC_PAGES)
		{
			BlockNumber nb = io->alloc(io->ctx);
			char	   *np = io->record_page(io->ctx, nb, true);

			sift_l0_href_seal(last);
			sift_l0_href_format(np, last_blk, SL(last)->chain_pages + 1, SL(last)->chain_rows);
			take = Min(n, (uint32) SIFT_L0_HEAPREF_CAP);
			sift_l0_href_add(np, refs, take);
			refs += take;
			n -= take;
			last = np;
			last_blk = nb;
			pages++;
		}
		io->record_done(io->ctx);
		tail = last_blk;
	}
	return tail;
}

typedef struct Segment
{
	BlockNumber blk;
	uint32		n;
	SiftL0HeapRef *refs;
} Segment;

SiftL0HeapRef *
sift_l0_href_chain_read(const SiftL0PageIO *io, BlockNumber tail, uint32 *n, BlockNumber **pages, uint32 *npages, const char **why, BlockNumber *bad_blk)
{
	Segment		seg[2 * SIFT_L0_SLAB_MAX_PAGES];
	uint32		np = 0,
				total = 0;
	SiftL0HeapRef *out;

	*why = NULL;
	for (BlockNumber blk = tail, cur; blk != InvalidBlockNumber && *why == NULL;)
	{
		const char *pg = io->read_lock(io->ctx, cur = blk);
		const SiftL0SlabPageData *sl = (const SiftL0SlabPageData *) SiftPageGetData(pg);

		*why = np == lengthof(seg) ? "the chain is longer than a chain can be" : sift_l0_href_verify(pg);
		if (*why == NULL)
		{
			/* check-memory: allow the references of one page of a chain, at most its page capacity */
			seg[np] = (Segment) {cur, sl->page_rows, palloc(Max(sl->page_rows, 1) * sizeof(SiftL0HeapRef))};
			memcpy(seg[np].refs, SIFT_L0_HREF_PTR(pg), sl->page_rows * sizeof(SiftL0HeapRef));
			total += sl->page_rows;
			np++;
			blk = sl->prev_blk;
		}
		else
			*bad_blk = cur;
		io->read_unlock(io->ctx, cur);
	}
	/* check-memory: allow the references of one slab, that the flush of a slab keeps small */
	out = palloc(Max(total, 1) * sizeof(SiftL0HeapRef));
	*n = 0;
	if (pages != NULL)
		*pages = palloc(Max(np, 1) * sizeof(BlockNumber));
	for (uint32 i = np; i > 0; i--)
	{
		memcpy(out + *n, seg[i - 1].refs, seg[i - 1].n * sizeof(SiftL0HeapRef));
		*n += seg[i - 1].n;
		if (pages != NULL)
			(*pages)[np - i] = seg[i - 1].blk;
	}
	for (uint32 i = 0; i < np; i++)
		pfree(seg[i].refs);
	if (npages != NULL)
		*npages = np;
	if (*why != NULL)
	{
		pfree(out);
		return NULL;
	}
	return out;
}

#define SIFT_L0_GC_MAX_REFS		(SIFT_L0_GC_MAX_BYTES / sizeof(SiftL0HeapRef))

uint32
sift_l0_href_submit(SiftL0GroupCommit *gc, const SiftL0ShardCtx *ctx, const RelFileLocator *loc, uint64 part_id, uint32 shard, BlockNumber anchor,
					const SiftL0HeapRef *refs, uint32 n, uint32 *applied)
{
	for (uint32 done = *applied = 0; done < n;)
	{
		uint32		take = Min(n - done, (uint32) SIFT_L0_GC_MAX_REFS),
					result;
		SiftL0GcReq req = {.locator = *loc,.part_id = part_id,.shard_id = (uint16) (shard | SIFT_L0_GC_SHARD_HEAPREF),.expect_slab_blk = anchor,
			.expect_pd_blk = InvalidBlockNumber,.rec = refs + done,.rec_bytes = take * sizeof(SiftL0HeapRef)};

		result = sift_l0_gc_submit(gc, &req, sift_l0_shard_apply, (void *) ctx);
		if (result != SIFT_L0_GC_APPLIED)
			return result;
		*applied = done += take;
	}
	return SIFT_L0_GC_APPLIED;
}

#ifndef SIFT_HREF_STANDALONE

/* ---- SiftL0PageIO over buffers ---- */

static BlockNumber
href_alloc(void *ctx)
{
	return sift_fsm_alloc_meta_page(((SiftHrefIo *) ctx)->rel);
}

static void
href_release(void *ctx, BlockNumber blk)
{
	SiftExtent	ext = {blk, 1};

	sift_fsm_release(((SiftHrefIo *) ctx)->rel, &ext, 1, ReadNextFullTransactionId());
}

static void
href_begin(void *ctx)
{
	SiftHrefIo *h = ctx;

	h->x = sift_xlog_start(h->rel);
}

static char *
href_page(void *ctx, BlockNumber blk, bool full)
{
	SiftHrefIo *h = ctx;
	Buffer		buf;

	for (int i = 0; i < h->x.n; i++)
		if (BufferGetBlockNumber(h->x.buf[i]) == blk)
			return h->x.page[i];
	if (full)
	{
		Page		pg = sift_xlog_register(&h->x, sift_blk_new(h->rel, blk), GENERIC_XLOG_FULL_IMAGE);

		sift_page_init_part(pg, SIFT_PT_L0_SLAB, sift_manifest_new_gen(h->rel, 1), h->part_id);
		return pg;
	}
	buf = sift_blk_read(h->rel, blk, BUFFER_LOCK_EXCLUSIVE);
	sift_page_expect(h->rel, BufferGetPage(buf), blk, SIFT_PT_L0_SLAB, 0, h->part_id);
	return sift_xlog_register(&h->x, buf, 0);
}

static void
href_done(void *ctx)
{
	sift_xlog_done(&((SiftHrefIo *) ctx)->x);
}

static void
href_abort(void *ctx)
{
	SiftHrefIo *h = ctx;

	sift_xlog_abort(&h->x);
	for (int i = 0; i < h->x.n; i++)
		UnlockReleaseBuffer(h->x.buf[i]);
}

static const char *
href_rdlock(void *ctx, BlockNumber blk)
{
	SiftHrefIo *h = ctx;
	Page		pg;

	h->rd = sift_blk_read(h->rel, blk, BUFFER_LOCK_SHARE);
	pg = BufferGetPage(h->rd);
	sift_page_expect(h->rel, pg, blk, SIFT_PT_L0_SLAB, 0, h->part_id);
	return pg;
}

static void
href_rdunlock(void *ctx, BlockNumber blk)
{
	UnlockReleaseBuffer(((SiftHrefIo *) ctx)->rd);
}

void
sift_href_io_init(SiftHrefIo *h, Relation rel, uint32 part_id)
{
	*h = (SiftHrefIo) {.rel = rel,.part_id = part_id,.rd = InvalidBuffer};
	h->io = (SiftL0PageIO) {.ctx = h,.alloc = href_alloc,.release = href_release,.record_begin = href_begin,.record_page = href_page,
		.record_done = href_done,.record_abort = href_abort,.read_lock = href_rdlock,.read_unlock = href_rdunlock};
}

/* ---- the writers of one partition, under its lock: no group commit, no shared memory (a backend that is alone writes here) ---- */

static const SiftPartitionEntry *
entry_of(Relation rel, uint64 part_id, const uint8 *key_lo, SiftManifestRead *mr)
{
	*mr = sift_manifest_read_range(rel, key_lo, NULL);
	if (mr->n == 0 || mr->entries[0].part_id != part_id)
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) part_id)));
	return &mr->entries[0];
}

/* The chain at tail takes n more references: it is a chain of references, not sealed, and the rows and pages stay within what a flush is asked at */
static bool
chain_takes(Relation rel, uint64 part_id, BlockNumber tail, uint32 n)
{
	Buffer		buf = sift_blk_read(rel, tail, BUFFER_LOCK_SHARE);
	const SiftL0SlabPageData *sl = (const SiftL0SlabPageData *) SiftPageGetData(BufferGetPage(buf));
	bool		ok = (sl->flags & (SIFT_L0_SLAB_HEAPREF | SIFT_L0_SLAB_SEALED)) == SIFT_L0_SLAB_HEAPREF &&
		sl->chain_rows < SIFT_L0_HEAPREF_FLUSH_ROWS && sl->chain_pages < SIFT_L0_HEAPREF_MAX_PAGES && sl->chain_rows + n <= SIFT_L0_HEAPREF_MAX_PAGES * SIFT_L0_HEAPREF_CAP;

	sift_page_expect(rel, BufferGetPage(buf), tail, SIFT_PT_L0_SLAB, 0, (uint32) part_id);
	UnlockReleaseBuffer(buf);
	return ok;
}

/* A chain that has reached a limit is sealed, as a chain of batches is: the next write finds it so and writes a part */
static void
seal_if_full(SiftHrefIo *h, BlockNumber tail)
{
	const SiftL0SlabPageData *sl;
	char	   *pg;
	bool		full;

	h->io.record_begin(h);
	pg = h->io.record_page(h, tail, false);
	sl = (const SiftL0SlabPageData *) SiftPageGetData(pg);
	full = sl->chain_rows >= SIFT_L0_HEAPREF_FLUSH_ROWS || sl->chain_pages >= SIFT_L0_HEAPREF_MAX_PAGES;
	if (full)
	{
		sift_l0_href_seal(pg);
		h->io.record_done(h);
	}
	else
		h->io.record_abort(h);
}

bool
sift_l0_href_append(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftL0HeapRef *refs, uint32 n)
{
	SiftManifestRead mr;
	SiftHrefIo	h;
	BlockNumber head,
				tail;

	sift_href_io_init(&h, rel, (uint32) part_id);
	LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	head = entry_of(rel, part_id, key_lo, &mr)->l0_slab_blk;
	if (head != InvalidBlockNumber && !chain_takes(rel, part_id, head, n))
	{
		UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
		return false;
	}
	if (head == InvalidBlockNumber)
	{
		uint32		k = Min(n, (uint32) SIFT_L0_HEAPREF_CAP);

		h.io.record_begin(&h);
		head = sift_l0_href_first(&h.io, refs, k);
		h.io.record_done(&h);
		tail = k < n ? sift_l0_href_chain_append(&h.io, head, refs + k, n - k) : head;
		sift_manifest_commit(rel, &(SiftManifestEdit) {.part_id = part_id,.key_lo = key_lo,.set_l0_slab = true,.new_l0_slab_blk = tail});
	}
	else
	{
		tail = sift_l0_href_chain_append(&h.io, head, refs, n);
		if (tail != head)
			sift_manifest_commit(rel, &(SiftManifestEdit) {.part_id = part_id,.key_lo = key_lo,.set_l0_slab = true,.new_l0_slab_blk = tail});
	}
	seal_if_full(&h, tail);
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	return true;
}

bool
sift_l0_href_append_deleting(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftL0HeapRef *refs, uint32 n, const SiftPendDelRec *dels, int ndel)
{
	BlockNumber lock = SIFT_PART_LOCK_BLKNO(part_id);
	SiftManifestRead mr;
	const SiftPartitionEntry *e;
	SiftManifestEdit edit = {.part_id = part_id,.key_lo = key_lo};
	SiftXlog	x;
	bool		fused;
	Buffer		buf = InvalidBuffer;

	LockPage(rel, lock, ExclusiveLock);
	e = entry_of(rel, part_id, key_lo, &mr);
	x = sift_xlog_start(rel);
	fused = e->pend_del_blk == InvalidBlockNumber || sift_pend_del_place(&x, rel, part_id, e->pend_del_blk, dels, ndel);
	if (fused && e->l0_slab_blk == InvalidBlockNumber)
	{
		edit.set_l0_slab = true;
		fused = n <= SIFT_L0_HEAPREF_CAP;
		if (fused)
		{
			BlockNumber blk = sift_fsm_alloc_meta_page(rel);
			Page		pg = sift_xlog_register(&x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE);

			sift_page_init_part(pg, SIFT_PT_L0_SLAB, sift_manifest_new_gen(rel, 1), (uint32) part_id);
			sift_l0_href_format(pg, InvalidBlockNumber, 1, 0);
			sift_l0_href_add(pg, refs, n);
			edit.new_l0_slab_blk = blk;
		}
	}
	else if (fused)
	{
		fused = chain_takes(rel, part_id, e->l0_slab_blk, n);
		if (fused)
		{
			buf = sift_blk_read(rel, e->l0_slab_blk, BUFFER_LOCK_EXCLUSIVE);
			fused = sift_l0_href_room(BufferGetPage(buf)) >= n;
			if (fused)
				sift_l0_href_add(sift_xlog_register(&x, buf, 0), refs, n);
			else
				UnlockReleaseBuffer(buf);
		}
	}
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

#endif							/* !SIFT_HREF_STANDALONE */
