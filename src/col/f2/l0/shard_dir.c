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

static void
shm_install(SiftL0ShardShm *shm, BlockNumber dir_blk, const SiftL0ShardDirData *d)
{
	for (uint32 s = 0; s < SIFT_L0_SHARD_ABS_MAX; s++)
	{
		pg_atomic_write_u32(&shm->tail[s], d->slab_tail[s]);
		pg_atomic_write_u32(&shm->pd_tail[s], d->pd_tail[s]);
	}
	pg_atomic_write_u32(&shm->nshards, d->nshards);
	pg_atomic_write_u32(&shm->heapref, (d->dir_flags & SIFT_L0_DIR_HEAPREF) != 0);
	pg_atomic_write_u64(&shm->dir_gen, d->dir_gen);
	pg_atomic_write_membarrier_u32(&shm->dir_blk, dir_blk);
}

void
sift_l0_shm_load(const SiftL0PageIO *io, SiftL0ShardShm *shm, BlockNumber dir_blk)
{
	SiftL0ShardDirData d = sift_l0_dir_read(io, dir_blk);

	shm_install(shm, dir_blk, &d);
}

void
sift_l0_shm_unload(SiftL0ShardShm *shm)
{
	pg_atomic_write_membarrier_u32(&shm->dir_blk, InvalidBlockNumber);
}

/* ---- the shared table of the hot partitions ---- */

void
sift_l0_hot_init(SiftL0HotTable *t, int tranche)
{
	sift_shm_lock_init(&t->claim_lock, tranche);
	for (int i = 0; i < SIFT_L0_HOT_SLOTS; i++)
	{
		SiftL0ShardShm *e = &t->shm[i];

		pg_atomic_init_u64(&e->dir_gen, 0);
		pg_atomic_init_u32(&e->dir_blk, NO_BLOCK);
		pg_atomic_init_u32(&e->nshards, 0);
		pg_atomic_init_u32(&e->flush_wanted, SIFT_L0_FLUSH_NONE);
		pg_atomic_init_u32(&e->claimed, 0);
		for (int s = 0; s < SIFT_L0_SHARD_ABS_MAX; s++)
		{
			pg_atomic_init_u32(&e->tail[s], NO_BLOCK);
			pg_atomic_init_u32(&e->pd_tail[s], NO_BLOCK);
		}
	}
}

static uint32
key_hash(const RelFileLocator *loc, uint64 part_id)
{
	uint64		h = ((uint64) loc->relNumber << 32 | loc->dbOid) ^ (part_id * UINT64CONST(0x9e3779b97f4a7c15));

	h ^= h >> 33;
	h *= UINT64CONST(0xff51afd7ed558ccd);
	h ^= h >> 33;
	return (uint32) h;
}

static bool
key_is(const SiftL0ShardShm *e, const RelFileLocator *loc, uint64 part_id)
{
	return e->part_id == part_id && RelFileLocatorEquals(e->locator, *loc);
}

void
sift_l0_hot_unpin(SiftL0ShardShm *e)
{
	pg_atomic_fetch_sub_u32(&e->active, 1);
}

/* A pin and a reclaim see each other: the pin is counted before the generation is read again, the reclaim sets an odd generation before it reads the pins */
static bool
pin_at(SiftL0ShardShm *e, uint32 gen)
{
	pg_atomic_fetch_add_u32(&e->active, 1);
	if (pg_atomic_read_u32(&e->claimed) == gen)
		return true;
	sift_l0_hot_unpin(e);
	return false;
}

/* The probe ends at the first entry never used: a key is set only in the first entry of its probe that has none of the keys before it */
SiftL0ShardShm *
sift_l0_hot_find(SiftL0HotTable *t, const RelFileLocator *loc, uint64 part_id)
{
	uint32		h = key_hash(loc, part_id);

	for (uint32 i = 0; i < SIFT_L0_HOT_PROBES; i++)
	{
		SiftL0ShardShm *e = &t->shm[(h + i) % SIFT_L0_HOT_SLOTS];
		uint32		gen = pg_atomic_read_u32(&e->claimed);

		if (gen == 0)
			return NULL;
		pg_read_barrier();
		if ((gen & 1) == 0 && key_is(e, loc, part_id))
			return pin_at(e, gen) ? e : NULL;
	}
	return NULL;
}

/* The entry goes to the partition unless a backend holds it (the claim lock is held): the writers of the partition it leaves find no entry and go to the lock of their partition */
static bool
key_take(SiftL0ShardShm *e, const RelFileLocator *loc, uint64 part_id)
{
	uint32		gen = pg_atomic_read_u32(&e->claimed);

	pg_atomic_write_membarrier_u32(&e->claimed, gen | 1);
	if (pg_atomic_read_u32(&e->active) != 0)
	{
		pg_atomic_write_u32(&e->claimed, gen);
		return false;
	}
	pg_atomic_write_u32(&e->dir_blk, NO_BLOCK);
	pg_atomic_write_u32(&e->flush_wanted, SIFT_L0_FLUSH_NONE);
	e->locator = *loc;
	e->part_id = part_id;
	pg_write_barrier();
	pg_atomic_write_u32(&e->claimed, Max(gen + 2, 2));
	return true;
}

/* An entry never used, else the first one of a cold partition that no backend holds, else the first one no backend holds */
SiftL0ShardShm *
sift_l0_hot_claim(SiftL0HotTable *t, const RelFileLocator *loc, uint64 part_id)
{
	uint32		h = key_hash(loc, part_id);
	SiftL0ShardShm *found = NULL,
			   *cold = NULL,
			   *idle = NULL;

	sift_shm_lock(&t->claim_lock);
	for (uint32 i = 0; i < SIFT_L0_HOT_PROBES && found == NULL; i++)
	{
		SiftL0ShardShm *e = &t->shm[(h + i) % SIFT_L0_HOT_SLOTS];

		if (pg_atomic_read_u32(&e->claimed) == 0)
		{
			cold = cold != NULL ? cold : e;
			break;
		}
		if (key_is(e, loc, part_id))
			found = e;
		else if (pg_atomic_read_u32(&e->active) == 0 && pg_atomic_read_u32(&e->dir_blk) == NO_BLOCK)
			cold = cold != NULL ? cold : e;
		else if (pg_atomic_read_u32(&e->active) == 0)
			idle = idle != NULL ? idle : e;
	}
	if (found == NULL && cold != NULL && key_take(cold, loc, part_id))
		found = cold;
	if (found == NULL && idle != NULL && key_take(idle, loc, part_id))
		found = idle;
	if (found != NULL)
		pg_atomic_fetch_add_u32(&found->active, 1);
	sift_shm_unlock(&t->claim_lock);
	return found;
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
place_slab(char *pg, SiftL0GcSlot *const *items, uint32 n, Lane *ln, bool *changed)
{
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

/* pg NULL: the shard has no PEND_DEL page yet */
static void
place_pd(char *pg, SiftL0GcSlot *const *items, uint32 n, Lane *ln, bool *changed)
{
	while (pg != NULL && ln->pd_done < n && sift_l0_pd_fits(pg, items[ln->pd_done]->h.ndels))
	{
		sift_l0_pd_add(pg, SIFT_L0_GC_DELS(items[ln->pd_done]), items[ln->pd_done]->h.ndels);
		*changed = true;
		ln->pd_done = next_with_dels(items, n, ln->pd_done + 1);
	}
	ln->pd_full = ln->pd_done < n;
}

/* What one round places on the pages: returns whether it placed anything */
static bool
place_pages(char *slab, char *pd, SiftL0GcSlot *const *items, uint32 n, Lane *ln)
{
	bool		changed = false;

	ln->slab_full = ln->pd_full = false;
	ln->pd_done = next_with_dels(items, n, ln->pd_done);
	if (ln->slab_done < n)
		place_slab(slab, items, n, ln, &changed);
	if (ln->pd_done < n)
		place_pd(pd, items, n, ln, &changed);
	return changed;
}

/* One WAL record for the slab page and the PEND_DEL page together */
static void
place_round(const SiftL0ShardCtx *c, SiftL0GcSlot *const *items, uint32 n, uint32 s, Lane *ln)
{
	BlockNumber pdb = pg_atomic_read_u32(&c->shm->pd_tail[s]);
	char	   *slab,
			   *pd;

	c->io->record_begin(c->io->ctx);
	slab = c->io->record_page(c->io->ctx, pg_atomic_read_u32(&c->shm->tail[s]), SIFT_PT_L0_SLAB, false);
	pd = pdb == NO_BLOCK ? NULL : c->io->record_page(c->io->ctx, pdb, SIFT_PT_PEND_DEL, false);
	if (place_pages(slab, pd, items, n, ln))
		c->io->record_done(c->io->ctx);
	else
		c->io->record_abort(c->io->ctx);
}

static void
copy_page(const SiftL0PageIO *io, BlockNumber blk, SiftPageType type, char *out)
{
	memcpy(out, io->read_lock(io->ctx, blk, type), BLCKSZ);
	io->read_unlock(io->ctx, blk);
}

/*
 * The successor pages the batch takes: the rounds run on copies of the tails first. They are taken from the allocator before any record, so that an error of the allocator leaves no
 * request placed that the combiner would give a retry to
 */
static uint32
count_successors(const SiftL0ShardCtx *c, SiftL0GcSlot *const *items, uint32 n, uint32 s, Lane ln)
{
	PGAlignedBlock slab,
				pd;
	BlockNumber pdb = pg_atomic_read_u32(&c->shm->pd_tail[s]);
	bool		has_pd = pdb != NO_BLOCK;
	uint32		need = 0;

	copy_page(c->io, pg_atomic_read_u32(&c->shm->tail[s]), SIFT_PT_L0_SLAB, slab.data);
	if (has_pd)
		copy_page(c->io, pdb, SIFT_PT_PEND_DEL, pd.data);
	while (place_pages(slab.data, has_pd ? pd.data : NULL, items, n, &ln), ln.slab_full || ln.pd_full)
	{
		if (ln.slab_full)
			format_shard_slab(slab.data, ln.href);
		if (ln.pd_full && !has_pd)
			memcpy(pd.data, slab.data, BLCKSZ);
		if (ln.pd_full)
			sift_l0_pd_format(pd.data, NO_BLOCK, 1);
		has_pd |= ln.pd_full;
		need += ln.slab_full + ln.pd_full;
	}
	return need;
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
rotate_slab(const SiftL0ShardCtx *c, uint32 s, bool href, BlockNumber nb)
{
	const SiftL0PageIO *io = c->io;
	BlockNumber old = pg_atomic_read_u32(&c->shm->tail[s]);
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
rotate_pd(const SiftL0ShardCtx *c, uint32 s, BlockNumber nb)
{
	const SiftL0PageIO *io = c->io;
	BlockNumber old = pg_atomic_read_u32(&c->shm->pd_tail[s]);
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
	BlockNumber succ[SIFT_L0_FRESH_MAX];
	uint32		nsucc,
				used = 0;

	Assert(s < pg_atomic_read_u32(&c->shm->nshards));
	if (items[0]->h.expect_slab_blk != pg_atomic_read_u32(&c->shm->dir_blk))
	{
		set_results(items, n, SIFT_L0_GC_STALE);
		return;
	}
	nsucc = count_successors(c, items, n, s, ln);
	Assert(nsucc <= lengthof(succ));
	for (uint32 i = 0; i < nsucc; i++)
		succ[i] = c->io->alloc(c->io->ctx);
	for (;;)
	{
		place_round(c, items, n, s, &ln);
		if (!ln.slab_full && !ln.pd_full)
			break;
		if (ln.slab_full)
			rotate_slab(c, s, ln.href, succ[used++]);
		if (ln.pd_full)
			rotate_pd(c, s, succ[used++]);
	}
	Assert(used == nsucc);
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

static uint32
lock_shards(SiftL0Flush *f, uint32 *held)
{
	return sift_l0_gc_lock_shards(f->gc, &f->shm->locator, f->shm->part_id, pg_atomic_read_u32(&f->shm->nshards), held);
}

void
sift_l0_flush_seal(SiftL0Flush *f, SiftL0AnchorFn commit_anchor, void *cctx)
{
	uint32		held[SIFT_L0_SHARD_ABS_MAX],
				nheld;
	SealTarget	targets[2 * SIFT_L0_SHARD_ABS_MAX + 1];
	SiftL0ShardDirData old,
				v1;
	uint32		n;

	f->old_dir = pg_atomic_read_u32(&f->shm->dir_blk);
	old = sift_l0_dir_read(f->io, f->old_dir);
	f->dir_v1 = sift_l0_dir_create(f->io, old.nshards, f->old_dir, old.dir_gen + 1, old.dir_flags, f->floor);
	v1 = sift_l0_dir_read(f->io, f->dir_v1);
	commit_anchor(cctx, f->dir_v1);
	nheld = lock_shards(f, held);
	shm_install(f->shm, f->dir_v1, &v1);
	pg_atomic_write_u32(&f->shm->flush_wanted, SIFT_L0_FLUSH_NONE);
	sift_l0_gc_unlock_shards(f->gc, held, nheld);
	old = sift_l0_dir_read(f->io, f->old_dir);
	n = seal_targets(&old, targets);
	targets[n++] = (SealTarget) {f->old_dir, false};
	seal_all(f->io, targets, n);
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
sift_l0_dir_release(const SiftL0PageIO *io, BlockNumber anchor, bool follow_draining)
{
	SiftL0ScanVisitor drained = {.page = release_page,.ctx = (void *) io};

	sift_l0_scan(io, anchor, &drained, follow_draining);
}

void
sift_l0_flush_publish(SiftL0Flush *f, SiftL0AnchorFn publish, void *pctx)
{
	uint32		held[SIFT_L0_SHARD_ABS_MAX],
				nheld = lock_shards(f, held);
	SiftL0ShardDirData live;
	bool		cold;
	BlockNumber v2 = NO_BLOCK;

	Assert(pg_atomic_read_u32(&f->shm->dir_blk) == f->dir_v1);
	sift_l0_shm_unload(f->shm);
	sift_l0_gc_unlock_shards(f->gc, held, nheld);
	live = sift_l0_dir_read(f->io, f->dir_v1);
	cold = dir_empty(f->io, &live);
	if (!cold)
	{
		v2 = f->io->alloc(f->io->ctx);
		live.draining_slab_blk = NO_BLOCK;
		live.dir_gen++;
		write_dir(f->io, v2, &live, f->floor);
	}
	publish(pctx, v2);
	if (!cold)
		shm_install(f->shm, v2, &live);
	sift_l0_dir_release(f->io, f->old_dir, true);
	if (cold)
		sift_l0_dir_release(f->io, f->dir_v1, false);
	else
		f->io->release(f->io->ctx, f->dir_v1);
}
