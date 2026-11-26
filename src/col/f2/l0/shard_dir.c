/*
 * l0_shard_dir.c - the sharded active L0 slab (l0_shard_dir.h).
 */
#include "col/f2/l0/shard_dir.h"
#include "core/f2/meta.h"
#include "core/xid_cache.h"

#define NO_BLOCK	InvalidBlockNumber

/* ---- page images ---- */

/* The page as the host initialized it (type, partition and generation in the special space): its payload is emptied */
static void
page_init(char *pg)
{
	PageHeader	ph = (PageHeader) pg;

	Assert(sift_page_layout_ok(pg));
	memset(pg + SIFT_PAGE_DATA_OFFSET, 0, ph->pd_special - SIFT_PAGE_DATA_OFFSET);
	ph->pd_lower = SIFT_PAGE_DATA_OFFSET;
}

void
sift_l0_slab_format(char *pg, BlockNumber prev, uint16 chain_pages, uint32 chain_rows)
{
	SiftL0SlabPageData *sl = SIFT_L0_SL(pg);

	page_init(pg);
	((PageHeader) pg)->pd_lower = SIFT_L0_PAGE_DATA_OFFSET + sizeof(SiftL0SlabPageData);
	sl->magic = SIFT_L0_SLAB_MAGIC;
	sl->flags = SIFT_L0_SLAB_REV | SIFT_L0_SLAB_NOCRC_OPEN;
	sl->prev_blk = prev;
	sl->chain_pages = chain_pages;
	sl->chain_rows = chain_rows;
	sl->min_xid = sl->max_xid = InvalidFullTransactionId;
}

static inline uint32
page_free(const char *pg)
{
	const PageHeaderData *ph = (const PageHeaderData *) pg;

	return ph->pd_upper - ph->pd_lower;
}

bool
sift_l0_slab_fits(const char *pg, uint32 bytes)
{
	return page_free(pg) >= bytes;
}

static void
widen_xids(FullTransactionId *lo, FullTransactionId *hi, FullTransactionId xid)
{
	if (!FullTransactionIdIsValid(*lo) || FullTransactionIdPrecedes(xid, *lo))
		*lo = xid;
	if (!FullTransactionIdIsValid(*hi) || FullTransactionIdPrecedes(*hi, xid))
		*hi = xid;
}

void
sift_l0_slab_add(char *pg, const SiftL0BatchHeader *rec)
{
	PageHeader	ph = (PageHeader) pg;
	SiftL0SlabPageData *sl = SIFT_L0_SL(pg);

	Assert(rec->bytes % 8 == 0 && rec->bytes >= SIFT_L0_COMPACT_HDR && (sl->flags & SIFT_L0_SLAB_NOCRC_OPEN) != 0);
	memcpy(pg + ph->pd_lower, rec, rec->bytes);
	ph->pd_lower += rec->bytes;
	sl->nbatches++;
	sl->page_rows += rec->nrows;
	sl->chain_rows += rec->nrows;
	widen_xids(&sl->min_xid, &sl->max_xid, sift_l0_href_fxid(rec->xmin));
}

static void
slab_seal(char *pg)
{
	SIFT_L0_SL(pg)->flags = (SIFT_L0_SL(pg)->flags | SIFT_L0_SLAB_SEALED) & ~SIFT_L0_SLAB_NOCRC_OPEN;
	sift_page_seal(pg);
}

void
sift_l0_pd_format(char *pg, BlockNumber prev, uint16 chain_pages)
{
	SiftPendDelPageData *pd = SIFT_L0_PD(pg);

	page_init(pg);
	((PageHeader) pg)->pd_lower = SIFT_L0_PAGE_DATA_OFFSET + offsetof(SiftPendDelPageData, recs);
	pd->magic = SIFT_PEND_DEL_MAGIC;
	pd->flags = sift_pd_set_chain_pages(SIFT_PEND_DEL_REV | SIFT_PEND_DEL_SHARD_DIR | SIFT_PEND_DEL_PLAIN, chain_pages);
	pd->prev_blk = prev;
	pd->min_xid = pd->max_xid = InvalidFullTransactionId;
}

bool
sift_l0_pd_fits(const char *pg, uint32 ndels)
{
	return page_free(pg) >= ndels * sizeof(SiftL0GcDel);
}

void
sift_l0_pd_add(char *pg, const SiftL0GcDel *dels, uint32 ndels)
{
	PageHeader	ph = (PageHeader) pg;
	SiftPendDelPageData *pd = SIFT_L0_PD(pg);

	FullTransactionId next = ReadNextFullTransactionId();

	memcpy(pg + ph->pd_lower, dels, ndels * sizeof(SiftL0GcDel));
	ph->pd_lower += ndels * sizeof(SiftL0GcDel);
	pd->nrecs += ndels;
	for (uint32 i = 0; i < ndels; i++)
		if ((dels[i].tid & SIFT_PD_TID_SUCC) == 0 && TransactionIdIsNormal(dels[i].xmax))
			widen_xids(&pd->min_xid, &pd->max_xid, sift_xid_fxid_at(next, dels[i].xmax));
}

/* ---- the directory ---- */

bool
sift_l0_page_is_dir(const char *pg)
{
	return (SIFT_L0_SL(pg)->flags & SIFT_L0_SLAB_SHARD_DIR) != 0;
}

static void
dir_format(char *pg, const SiftL0ShardDirData *d, FullTransactionId floor)
{
	sift_l0_slab_format(pg, NO_BLOCK, 1, 0);
	SIFT_L0_SL(pg)->flags = SIFT_L0_SLAB_SHARD_DIR;
	SIFT_L0_SL(pg)->min_xid = floor;
	((PageHeader) pg)->pd_lower += sizeof(SiftL0ShardDirData);
	*SIFT_L0_DIR(pg) = *d;
}

static void
format_shard_slab(char *pg, bool href)
{
	if (href)
		sift_l0_href_format(pg, NO_BLOCK, 1, 0);
	else
		sift_l0_slab_format(pg, NO_BLOCK, 1, 0);
}

/* Formats blocks as full images, SIFT_L0_REC_PAGES to a record */
static void
format_slabs(const SiftL0PageIO *io, const BlockNumber *blks, uint32 n, bool href)
{
	for (uint32 i = 0; i < n; i += SIFT_L0_REC_PAGES)
	{
		io->record_begin(io->ctx);
		for (uint32 j = i; j < Min(i + SIFT_L0_REC_PAGES, n); j++)
			format_shard_slab(io->record_page(io->ctx, blks[j], SIFT_PT_L0_SLAB, true), href);
		io->record_done(io->ctx);
	}
}

static void
write_dir(const SiftL0PageIO *io, BlockNumber blk, const SiftL0ShardDirData *d, FullTransactionId floor)
{
	io->record_begin(io->ctx);
	dir_format(io->record_page(io->ctx, blk, SIFT_PT_L0_SLAB, true), d, floor);
	io->record_done(io->ctx);
}

static SiftL0ShardDirData
dir_fresh(uint32 nshards, uint64 gen, BlockNumber draining, uint16 dir_flags)
{
	SiftL0ShardDirData d;

	memset(&d, 0, sizeof(d));
	d.dir_magic = SIFT_L0_SHARD_DIR_MAGIC;
	d.nshards = nshards;
	d.dir_flags = dir_flags;
	d.dir_gen = gen;
	d.draining_slab_blk = draining;
	for (uint32 s = 0; s < SIFT_L0_SHARD_ABS_MAX; s++)
	{
		d.slab_tail[s] = NO_BLOCK;
		d.pd_tail[s] = NO_BLOCK;
	}
	return d;
}

BlockNumber
sift_l0_dir_create(const SiftL0PageIO *io, uint32 nshards, BlockNumber draining, uint64 dir_gen, uint16 dir_flags, FullTransactionId floor)
{
	SiftL0ShardDirData d = dir_fresh(nshards, dir_gen, draining, dir_flags);
	BlockNumber dir_blk = io->alloc(io->ctx);

	Assert(nshards <= SIFT_L0_SHARD_ABS_MAX);
	for (uint32 s = 0; s < nshards; s++)
	{
		d.slab_tail[s] = io->alloc(io->ctx);
		d.slab_pages[s] = 1;
	}
	format_slabs(io, d.slab_tail, nshards, (dir_flags & SIFT_L0_DIR_HEAPREF) != 0);
	write_dir(io, dir_blk, &d, floor);
	return dir_blk;
}

BlockNumber
sift_l0_dir_promote(const SiftL0PageIO *io, BlockNumber cold_tail, uint32 nshards, FullTransactionId floor)
{
	BlockNumber dir_blk = io->alloc(io->ctx);
	const SiftL0SlabPageData *sl = SIFT_L0_SL(io->read_lock(io->ctx, cold_tail, SIFT_PT_L0_SLAB));
	bool		href = (sl->flags & SIFT_L0_SLAB_HEAPREF) != 0;
	SiftL0ShardDirData d = dir_fresh(nshards, 1, NO_BLOCK, href ? SIFT_L0_DIR_HEAPREF : 0);

	d.slab_tail[0] = cold_tail;
	d.slab_pages[0] = sl->chain_pages;
	d.slab_rows[0] = sl->chain_rows;
	io->read_unlock(io->ctx, cold_tail);
	for (uint32 s = 1; s < nshards; s++)
	{
		d.slab_tail[s] = io->alloc(io->ctx);
		d.slab_pages[s] = 1;
	}
	format_slabs(io, d.slab_tail + 1, nshards - 1, href);
	write_dir(io, dir_blk, &d, floor);
	return dir_blk;
}

SiftL0ShardDirData
sift_l0_dir_read(const SiftL0PageIO *io, BlockNumber blk)
{
	SiftL0ShardDirData d = *SIFT_L0_DIR(io->read_lock(io->ctx, blk, SIFT_PT_L0_SLAB));

	io->read_unlock(io->ctx, blk);
	Assert(d.dir_magic == SIFT_L0_SHARD_DIR_MAGIC);
	return d;
}

void
sift_l0_shm_load(const SiftL0PageIO *io, SiftL0ShardShm *shm, BlockNumber dir_blk)
{
	SiftL0ShardDirData d = sift_l0_dir_read(io, dir_blk);

	for (uint32 s = 0; s < SIFT_L0_SHARD_ABS_MAX; s++)
	{
		pg_atomic_write_u32(&shm->tail[s], d.slab_tail[s]);
		pg_atomic_write_u32(&shm->pd_tail[s], d.pd_tail[s]);
	}
	pg_atomic_write_u32(&shm->nshards, d.nshards);
	pg_atomic_write_u32(&shm->heapref, (d.dir_flags & SIFT_L0_DIR_HEAPREF) != 0);
	pg_atomic_write_u64(&shm->dir_gen, d.dir_gen);
	pg_atomic_write_membarrier_u32(&shm->dir_blk, dir_blk);
}

void
sift_l0_shm_unload(SiftL0ShardShm *shm)
{
	pg_atomic_write_membarrier_u32(&shm->dir_blk, InvalidBlockNumber);
}

/* ---- the writers ---- */

typedef struct Lane
{
	uint32		slab_done;
	uint32		pd_done;
	bool		slab_full;
	bool		pd_full;
	bool		href;			/* the slots carry SiftL0HeapRef[] (SIFT_L0_GC_SHARD_HEAPREF) */
} Lane;

static uint32
next_with_dels(SiftL0GcSlot *const *items, uint32 n, uint32 from)
{
	while (from < n && items[from]->h.ndels == 0)
		from++;
	return from;
}

static void
place_slab(const SiftL0ShardCtx *c, SiftL0GcSlot *const *items, uint32 n, uint32 s, Lane *ln, bool *changed)
{
	char	   *pg = c->io->record_page(c->io->ctx, pg_atomic_read_u32(&c->shm->tail[s]), SIFT_PT_L0_SLAB, false);

	for (; ln->slab_done < n; ln->slab_done++)
	{
		const SiftL0GcSlot *it = items[ln->slab_done];
		uint32		nrefs = it->h.slab_bytes / sizeof(SiftL0HeapRef);

		if (it->h.slab_bytes == 0)
			continue;
		if (ln->href ? sift_l0_href_room(pg) < nrefs : !sift_l0_slab_fits(pg, it->h.slab_bytes))
			break;
		if (ln->href)
			sift_l0_href_add(pg, SIFT_L0_GC_REC(it), nrefs);
		else
			sift_l0_slab_add(pg, SIFT_L0_GC_REC(it));
		*changed = true;
	}
	ln->slab_full = ln->slab_done < n;
}

static void
place_pd(const SiftL0ShardCtx *c, SiftL0GcSlot *const *items, uint32 n, uint32 s, Lane *ln, bool *changed)
{
	BlockNumber blk = pg_atomic_read_u32(&c->shm->pd_tail[s]);
	char	   *pg;

	if (blk == NO_BLOCK)
	{
		ln->pd_full = true;
		return;
	}
	pg = c->io->record_page(c->io->ctx, blk, SIFT_PT_PEND_DEL, false);
	while (ln->pd_done < n && sift_l0_pd_fits(pg, items[ln->pd_done]->h.ndels))
	{
		sift_l0_pd_add(pg, SIFT_L0_GC_DELS(items[ln->pd_done]), items[ln->pd_done]->h.ndels);
		*changed = true;
		ln->pd_done = next_with_dels(items, n, ln->pd_done + 1);
	}
	ln->pd_full = ln->pd_done < n;
}

/* One WAL record for the slab page and the PEND_DEL page together */
static void
place_round(const SiftL0ShardCtx *c, SiftL0GcSlot *const *items, uint32 n, uint32 s, Lane *ln)
{
	bool		changed = false;

	ln->slab_full = ln->pd_full = false;
	ln->pd_done = next_with_dels(items, n, ln->pd_done);
	c->io->record_begin(c->io->ctx);
	if (ln->slab_done < n)
		place_slab(c, items, n, s, ln, &changed);
	if (ln->pd_done < n)
		place_pd(c, items, n, s, ln, &changed);
	if (changed)
		c->io->record_done(c->io->ctx);
	else
		c->io->record_abort(c->io->ctx);
}

static void
note_flush(const SiftL0ShardDirData *d, SiftL0ShardShm *shm)
{
	uint64		rows = 0;
	uint32		pages = 0;
	uint32		want,
				cur;

	for (uint32 s = 0; s < d->nshards; s++)
	{
		rows += d->slab_rows[s];
		pages += d->slab_pages[s];
	}
	want = pages >= 2 * SIFT_L0_SLAB_MAX_PAGES ? SIFT_L0_FLUSH_NOW :
		(rows >= 2 * SIFT_L0_SLAB_FLUSH_ROWS || pages >= SIFT_L0_SLAB_MAX_PAGES) ? SIFT_L0_FLUSH_ASYNC : SIFT_L0_FLUSH_NONE;
	cur = pg_atomic_read_u32(&shm->flush_wanted);
	while (want > cur && !pg_atomic_compare_exchange_u32(&shm->flush_wanted, &cur, want))
		;
}

/* The full tail gets a successor and a seal; the directory page is locked for the few bytes that name the new tail, never Block 0 */
static void
rotate_slab(const SiftL0ShardCtx *c, uint32 s, bool href)
{
	const SiftL0PageIO *io = c->io;
	BlockNumber old = pg_atomic_read_u32(&c->shm->tail[s]);
	BlockNumber nb = io->alloc(io->ctx);
	char	   *np,
			   *op,
			   *dp;
	SiftL0SlabPageData *osl;
	SiftL0ShardDirData *dir;

	io->record_begin(io->ctx);
	np = io->record_page(io->ctx, nb, SIFT_PT_L0_SLAB, true);
	op = io->record_page(io->ctx, old, SIFT_PT_L0_SLAB, false);
	dp = io->record_page(io->ctx, pg_atomic_read_u32(&c->shm->dir_blk), SIFT_PT_L0_SLAB, false);
	osl = SIFT_L0_SL(op);
	if (href)
	{
		sift_l0_href_seal(op);
		sift_l0_href_format(np, old, osl->chain_pages + 1, osl->chain_rows);
	}
	else
	{
		slab_seal(op);
		sift_l0_slab_format(np, old, osl->chain_pages + 1, osl->chain_rows);
	}
	dir = SIFT_L0_DIR(dp);
	dir->slab_tail[s] = nb;
	dir->slab_pages[s] = osl->chain_pages + 1;
	dir->slab_rows[s] = osl->chain_rows;
	note_flush(dir, c->shm);
	io->record_done(io->ctx);
	pg_atomic_write_u32(&c->shm->tail[s], nb);
}

static void
rotate_pd(const SiftL0ShardCtx *c, uint32 s)
{
	const SiftL0PageIO *io = c->io;
	BlockNumber old = pg_atomic_read_u32(&c->shm->pd_tail[s]);
	BlockNumber nb = io->alloc(io->ctx);
	uint16		pages = 1;
	char	   *np,
			   *op,
			   *dp;
	SiftL0ShardDirData *dir;

	io->record_begin(io->ctx);
	np = io->record_page(io->ctx, nb, SIFT_PT_PEND_DEL, true);
	if (old != NO_BLOCK)
	{
		op = io->record_page(io->ctx, old, SIFT_PT_PEND_DEL, false);
		pages = sift_pd_chain_pages(SIFT_L0_PD(op)->flags) + 1;
		SIFT_L0_PD(op)->flags |= SIFT_PEND_DEL_SEALED;
	}
	dp = io->record_page(io->ctx, pg_atomic_read_u32(&c->shm->dir_blk), SIFT_PT_L0_SLAB, false);
	sift_l0_pd_format(np, old, pages);
	dir = SIFT_L0_DIR(dp);
	dir->pd_tail[s] = nb;
	dir->pd_pages[s] = pages;
	io->record_done(io->ctx);
	pg_atomic_write_u32(&c->shm->pd_tail[s], nb);
}

static void
set_results(SiftL0GcSlot *const *items, uint32 n, uint32 result)
{
	for (uint32 i = 0; i < n; i++)
		items[i]->h.hdr.result = result;
}

void
sift_l0_shard_apply(void *ctx, SiftL0GcSlot *const *items, uint32 n)
{
	const SiftL0ShardCtx *c = ctx;
	uint32		s = items[0]->h.shard_id & SIFT_L0_GC_SHARD_MASK;
	Lane		ln = {.href = (items[0]->h.shard_id & SIFT_L0_GC_SHARD_HEAPREF) != 0};

	Assert(s < pg_atomic_read_u32(&c->shm->nshards));
	if (items[0]->h.expect_slab_blk != pg_atomic_read_u32(&c->shm->dir_blk))
	{
		set_results(items, n, SIFT_L0_GC_STALE);
		return;
	}
	for (;;)
	{
		place_round(c, items, n, s, &ln);
		if (!ln.slab_full && !ln.pd_full)
			break;
		if (ln.slab_full)
			rotate_slab(c, s, ln.href);
		if (ln.pd_full)
			rotate_pd(c, s);
	}
	set_results(items, n, SIFT_L0_GC_APPLIED);
}

/* ---- the readers ---- */

static void
scan_slab_chain(const SiftL0PageIO *io, BlockNumber tail, const SiftL0ScanVisitor *v)
{
	for (BlockNumber blk = tail; blk != NO_BLOCK;)
	{
		const char *pg = io->read_lock(io->ctx, blk, SIFT_PT_L0_SLAB);
		const SiftL0SlabPageData *sl = SIFT_L0_SL(pg);
		BlockNumber prev = sl->prev_blk;
		uint32		end = ((const PageHeaderData *) pg)->pd_lower;

		if (v->page)
			v->page(v->ctx, blk, pg);
		if ((sl->flags & SIFT_L0_SLAB_HEAPREF) != 0)
		{
			if (v->refs)
				v->refs(v->ctx, SIFT_L0_HREF_PTR(pg), sl->page_rows);
			io->read_unlock(io->ctx, blk);
			blk = prev;
			continue;
		}
		for (uint32 off = SIFT_L0_PAGE_DATA_OFFSET + sizeof(SiftL0SlabPageData); off < end;)
		{
			const SiftL0BatchHeader *rec = (const SiftL0BatchHeader *) (pg + off);

			if (v->batch)
				v->batch(v->ctx, rec);
			off += rec->bytes;
		}
		io->read_unlock(io->ctx, blk);
		blk = prev;
	}
}

static void
scan_pd_chain(const SiftL0PageIO *io, BlockNumber tail, const SiftL0ScanVisitor *v)
{
	for (BlockNumber blk = tail; blk != NO_BLOCK;)
	{
		const char *pg = io->read_lock(io->ctx, blk, SIFT_PT_PEND_DEL);
		const SiftPendDelPageData *pd = SIFT_L0_PD(pg);
		BlockNumber prev = pd->prev_blk;

		if (v->page)
			v->page(v->ctx, blk, pg);
		if (v->dels)
			v->dels(v->ctx, pd->recs, pd->nrecs);
		io->read_unlock(io->ctx, blk);
		blk = prev;
	}
}

void
sift_l0_scan(const SiftL0PageIO *io, BlockNumber anchor, const SiftL0ScanVisitor *v, bool follow_draining)
{
	const char *pg = io->read_lock(io->ctx, anchor, SIFT_PT_L0_SLAB);
	bool		is_dir = sift_l0_page_is_dir(pg);
	SiftL0ShardDirData d;

	if (is_dir)
	{
		d = *SIFT_L0_DIR(pg);
		if (v->page)
			v->page(v->ctx, anchor, pg);
	}
	io->read_unlock(io->ctx, anchor);
	if (!is_dir)
	{
		scan_slab_chain(io, anchor, v);
		return;
	}
	for (uint32 s = 0; s < d.nshards; s++)
	{
		scan_slab_chain(io, d.slab_tail[s], v);
		scan_pd_chain(io, d.pd_tail[s], v);
	}
	if (follow_draining && d.draining_slab_blk != NO_BLOCK)
		sift_l0_scan(io, d.draining_slab_blk, v, true);
}

/* ---- the flush ---- */

typedef struct SealTarget
{
	BlockNumber blk;
	bool		pd;
} SealTarget;

static uint32
seal_targets(const SiftL0ShardDirData *d, SealTarget *out)
{
	uint32		n = 0;

	for (uint32 s = 0; s < d->nshards; s++)
	{
		out[n++] = (SealTarget) {d->slab_tail[s], false};
		if (d->pd_tail[s] != NO_BLOCK)
			out[n++] = (SealTarget) {d->pd_tail[s], true};
	}
	return n;
}

static void
seal_page(char *pg, bool pd)
{
	if (pd)
		SIFT_L0_PD(pg)->flags |= SIFT_PEND_DEL_SEALED;
	else if ((SIFT_L0_SL(pg)->flags & SIFT_L0_SLAB_HEAPREF) != 0)
		sift_l0_href_seal(pg);
	else
		slab_seal(pg);
}

static void
seal_all(const SiftL0PageIO *io, const SealTarget *t, uint32 n)
{
	for (uint32 i = 0; i < n; i += SIFT_L0_REC_PAGES)
	{
		io->record_begin(io->ctx);
		for (uint32 j = i; j < Min(i + SIFT_L0_REC_PAGES, n); j++)
			seal_page(io->record_page(io->ctx, t[j].blk, t[j].pd ? SIFT_PT_PEND_DEL : SIFT_PT_L0_SLAB, false), t[j].pd);
		io->record_done(io->ctx);
	}
}

void
sift_l0_flush_seal(SiftL0Flush *f, SiftL0AnchorFn commit_anchor, void *cctx)
{
	uint32		held[SIFT_L0_SHARD_ABS_MAX];
	uint32		nheld = sift_l0_gc_lock_shards(f->gc, &f->shm->locator, f->shm->part_id,
											   pg_atomic_read_u32(&f->shm->nshards), held);
	SealTarget	targets[2 * SIFT_L0_SHARD_ABS_MAX + 1];
	SiftL0ShardDirData old;
	uint32		n;

	f->old_dir = pg_atomic_read_u32(&f->shm->dir_blk);
	old = sift_l0_dir_read(f->io, f->old_dir);
	n = seal_targets(&old, targets);
	targets[n++] = (SealTarget) {f->old_dir, false};
	seal_all(f->io, targets, n);
	f->dir_v1 = sift_l0_dir_create(f->io, old.nshards, f->old_dir, old.dir_gen + 1, old.dir_flags, f->floor);
	commit_anchor(cctx, f->dir_v1);
	sift_l0_shm_load(f->io, f->shm, f->dir_v1);
	pg_atomic_write_u32(&f->shm->flush_wanted, SIFT_L0_FLUSH_NONE);
	sift_l0_gc_unlock_shards(f->gc, held, nheld);
}

static void
release_page(void *ctx, BlockNumber blk, const char *pg)
{
	const SiftL0PageIO *io = ctx;

	io->release(io->ctx, blk);
}

static bool
chain_empty(const SiftL0PageIO *io, BlockNumber tail)
{
	const char *pg = io->read_lock(io->ctx, tail, SIFT_PT_L0_SLAB);
	bool		empty = SIFT_L0_SL(pg)->chain_rows == 0;

	io->read_unlock(io->ctx, tail);
	return empty;
}

static bool
dir_empty(const SiftL0PageIO *io, const SiftL0ShardDirData *d)
{
	for (uint32 s = 0; s < d->nshards; s++)
		if (d->pd_tail[s] != NO_BLOCK || !chain_empty(io, d->slab_tail[s]))
			return false;
	return true;
}

void
sift_l0_flush_publish(SiftL0Flush *f, SiftL0AnchorFn publish, void *pctx)
{
	uint32		held[SIFT_L0_SHARD_ABS_MAX];
	uint32		nheld = sift_l0_gc_lock_shards(f->gc, &f->shm->locator, f->shm->part_id,
											   pg_atomic_read_u32(&f->shm->nshards), held);
	SiftL0ShardDirData live = sift_l0_dir_read(f->io, f->dir_v1);
	bool		cold = dir_empty(f->io, &live);
	BlockNumber v2 = NO_BLOCK;
	SiftL0ScanVisitor drained = {.page = release_page,.ctx = (void *) f->io};

	Assert(pg_atomic_read_u32(&f->shm->dir_blk) == f->dir_v1);
	if (!cold)
	{
		v2 = f->io->alloc(f->io->ctx);
		live.draining_slab_blk = NO_BLOCK;
		live.dir_gen++;
		write_dir(f->io, v2, &live, f->floor);
	}
	publish(pctx, v2);
	if (cold)
		sift_l0_shm_unload(f->shm);
	else
		sift_l0_shm_load(f->io, f->shm, v2);
	sift_l0_gc_unlock_shards(f->gc, held, nheld);
	sift_l0_scan(f->io, f->old_dir, &drained, true);
	if (cold)
		sift_l0_scan(f->io, f->dir_v1, &drained, false);
	else
		f->io->release(f->io->ctx, f->dir_v1);
}
