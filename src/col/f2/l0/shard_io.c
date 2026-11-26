/*
 * col/f2/l0/shard_io.c - the hot partitions of the micro-slab (col/f2/l0/shard_dir.h): the shared table of the directories that writers go to without a lock of the partition, the
 * group commit of their appends, the promotion of a partition whose lock the writers contend for, and the flush of a directory.
 */
#include "core/host.h"

#include "access/transam.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"

#include "col/dsmreg.h"
#include "col/f2/f2.h"
#include "col/f2/l0/href_fetch.h"
#include "col/f2/store/store.h"
#include "col/f2/l0/shard_dir.h"
#include "core/f2/manifest/manifest.h"
#include "core/page_io.h"
#include "core/part_lock.h"

#define TABLE_NAME		"sift_l0_gc"
#define TRANCHE_NAME	"sift_l0_claim"
#define HOT_SLOTS		256
#define HOT_PROBES		16
#define NO_BLOCK		InvalidBlockNumber

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftL0Table
{
	SiftShmLock claim_lock;
	SiftL0GroupCommit gc;
	SiftL0ShardShm shm[HOT_SLOTS];
} SiftL0Table;

static SiftL0Table *table;

static SiftL0Table *
table_base(void *raw)
{
	return (SiftL0Table *) TYPEALIGN(PG_CACHE_LINE_SIZE, raw);
}

static void
table_init(SIFT_DSM_INIT_ARGS)
{
	SiftL0Table *t = table_base(ptr);

	memset(t, 0, sizeof(*t));
	sift_shm_lock_init(&t->claim_lock, sift_tranche_new(TRANCHE_NAME));
	sift_l0_gc_init(&t->gc);
	for (int i = 0; i < HOT_SLOTS; i++)
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

/* NULL where the system has no memory for the table: no partition is hot then, and writers keep to the lock of the partition */
static SiftL0Table *
table_try(void)
{
	if (table == NULL)
	{
		void	   *raw = sift_dsm_try(TABLE_NAME, sizeof(SiftL0Table) + PG_CACHE_LINE_SIZE, table_init);

		if (raw == NULL)
			return NULL;
		table = table_base(raw);
		sift_tranche_register(table->claim_lock.tranche, TRANCHE_NAME);
	}
	return table;
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

/* The entry of the partition: entries are never given to another partition, so the probe ends at the first that is free */
static SiftL0ShardShm *
shm_find(SiftL0Table *t, const RelFileLocator *loc, uint64 part_id)
{
	uint32		h = key_hash(loc, part_id);

	for (uint32 i = 0; i < HOT_PROBES; i++)
	{
		SiftL0ShardShm *e = &t->shm[(h + i) % HOT_SLOTS];

		if (pg_atomic_read_u32(&e->claimed) == 0)
			return NULL;
		pg_read_barrier();
		if (key_is(e, loc, part_id))
			return e;
	}
	return NULL;
}

/* NULL when the probe holds no free entry: the partition stays cold */
static SiftL0ShardShm *
shm_claim(SiftL0Table *t, const RelFileLocator *loc, uint64 part_id)
{
	uint32		h = key_hash(loc, part_id);
	SiftL0ShardShm *found = NULL;

	sift_shm_lock(&t->claim_lock);
	for (uint32 i = 0; i < HOT_PROBES && found == NULL; i++)
	{
		SiftL0ShardShm *e = &t->shm[(h + i) % HOT_SLOTS];

		if (pg_atomic_read_u32(&e->claimed) == 0)
		{
			e->locator = *loc;
			e->part_id = part_id;
			pg_write_barrier();
			pg_atomic_write_u32(&e->claimed, 1);
			found = e;
		}
		else if (key_is(e, loc, part_id))
			found = e;
	}
	sift_shm_unlock(&t->claim_lock);
	return found;
}

/* ---- the writers ---- */

static uint32
my_shard(void)
{
	return sift_l0_shard_of(sift_shm_my_procno(), SIFT_L0_SHARD_ABS_MAX);
}

static SiftL0ShardShm *
hot_entry(SiftL0Table *t, Relation rel, uint64 part_id, bool href, BlockNumber *dir)
{
	SiftL0ShardShm *shm = t != NULL ? shm_find(t, &rel->rd_locator, part_id) : NULL;

	*dir = shm != NULL ? pg_atomic_read_u32(&shm->dir_blk) : NO_BLOCK;
	return *dir != NO_BLOCK && (pg_atomic_read_u32(&shm->heapref) != 0) == href ? shm : NULL;
}

/* A directory that wants a flush is flushed: queued for the worker, and by the writer itself at the hard cap */
static void
flush_if_wanted(Relation rel, uint64 part_id, const uint8 *key_lo, SiftL0ShardShm *shm)
{
	uint32		want = pg_atomic_read_u32(&shm->flush_wanted);

	if (want == SIFT_L0_FLUSH_ASYNC && pg_atomic_compare_exchange_u32(&shm->flush_wanted, &want, SIFT_L0_FLUSH_QUEUED))
		sift_l0_slab_request_flush(rel, part_id);
	else if (want == SIFT_L0_FLUSH_NOW)
		(void) sift_l0_slab_flush(rel, part_id, key_lo, &sift_f2_store_open(rel)->spec);
}

uint32
sift_l0_hot_refs(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftL0HeapRef *refs, uint32 n, uint32 *applied)
{
	SiftL0Table *t = table_try();
	BlockNumber dir;
	SiftL0ShardShm *shm = hot_entry(t, rel, part_id, true, &dir);
	SiftHrefIo	h;
	SiftL0ShardCtx c;
	uint32		result;

	*applied = 0;
	if (shm == NULL)
		return SIFT_L0_NOT_HOT;
	sift_href_io_init(&h, rel, (uint32) part_id);
	c = (SiftL0ShardCtx) {.io = &h.io,.shm = shm};
	result = sift_l0_href_submit(&t->gc, &c, &rel->rd_locator, part_id, my_shard(), dir, refs, n, applied);
	if (result == SIFT_L0_GC_APPLIED)
		flush_if_wanted(rel, part_id, key_lo, shm);
	return result;
}

uint32
sift_l0_hot_batches(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftL0BatchHeader *const *recs, uint32 nrecs, uint32 *applied)
{
	SiftL0Table *t = table_try();
	BlockNumber dir;
	SiftL0ShardShm *shm = hot_entry(t, rel, part_id, false, &dir);
	SiftHrefIo	h;
	SiftL0ShardCtx c;

	*applied = 0;
	if (shm == NULL)
		return SIFT_L0_NOT_HOT;
	sift_href_io_init(&h, rel, (uint32) part_id);
	c = (SiftL0ShardCtx) {.io = &h.io,.shm = shm};
	for (; *applied < nrecs; (*applied)++)
	{
		SiftL0GcReq req = {.locator = rel->rd_locator,.part_id = part_id,.shard_id = (uint16) my_shard(),.expect_slab_blk = dir,
			.expect_pd_blk = NO_BLOCK,.rec = recs[*applied],.rec_bytes = recs[*applied]->bytes};
		uint32		result = sift_l0_gc_submit(&t->gc, &req, sift_l0_shard_apply, &c);

		if (result != SIFT_L0_GC_APPLIED)
			return result;
	}
	flush_if_wanted(rel, part_id, key_lo, shm);
	return SIFT_L0_GC_APPLIED;
}

/* ---- the lock of the partition, and when it is too busy ---- */

bool
sift_l0_lock_busy(Relation rel, uint64 part_id)
{
	if (!ConditionalLockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock))
		return true;
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	return false;
}

/* The lock of the partition was busy (or not): after SIFT_COMB_HOT_STREAK busy ones in a row the partition is one the writers fight for */
static bool
partition_is_hot(Relation rel, uint64 part_id, bool busy)
{
	SiftCombQueue *q = &sift_part_stripes()[sift_part_shard_stripe(&rel->rd_locator, part_id, 0)];

	if (!busy)
	{
		if (pg_atomic_read_u32(&q->contend_streak) != 0)
			pg_atomic_write_u32(&q->contend_streak, 0);
	}
	else if (pg_atomic_add_fetch_u32(&q->contend_streak, 1) >= SIFT_COMB_HOT_STREAK)
		pg_atomic_write_u32(&q->hot_epoch, SIFT_COMB_HOT_EPOCHS);
	return sift_comb_queue_hot(q);
}

/* ---- promotion ---- */

/* The oldest transaction that can write to a directory made now: the horizon of the table reads it from the entry's anchor; the shared horizon, as the relation may be an index */
static FullTransactionId
xid_floor(void)
{
	return sift_xid_fxid_at(ReadNextFullTransactionId(), GetOldestNonRemovableTransactionId(NULL));
}

static void
commit_anchor(Relation rel, uint64 part_id, const uint8 *key_lo, BlockNumber anchor)
{
	sift_manifest_commit(rel, &(SiftManifestEdit) {.part_id = part_id,.key_lo = key_lo,.set_l0_slab = true,.new_l0_slab_blk = anchor});
}

static bool
tail_is_open_refs(const SiftL0PageIO *io, BlockNumber tail)
{
	const char *pg = io->read_lock(io->ctx, tail, SIFT_PT_L0_SLAB);
	bool		open = (SIFT_L0_SL(pg)->flags & (SIFT_L0_SLAB_HEAPREF | SIFT_L0_SLAB_SEALED | SIFT_L0_SLAB_SHARD_DIR)) == SIFT_L0_SLAB_HEAPREF;

	io->read_unlock(io->ctx, tail);
	return open;
}

/* The directory in place of the cold chain at anchor (a chain of references that takes appends becomes shard 0; any other is written out first), committed as the anchor */
static void
promote(Relation rel, SiftL0ShardShm *shm, uint64 part_id, const uint8 *key_lo, BlockNumber anchor, bool href)
{
	SiftHrefIo	h;
	BlockNumber dir;
	uint16		flags = href ? SIFT_L0_DIR_HEAPREF : 0;

	sift_href_io_init(&h, rel, (uint32) part_id);
	if (anchor != NO_BLOCK && !(href && tail_is_open_refs(&h.io, anchor)))
	{
		(void) sift_l0_slab_flush(rel, part_id, key_lo, &sift_f2_store_open(rel)->spec);
		anchor = NO_BLOCK;
	}
	sift_href_io_reserve(&h, SIFT_L0_SHARD_ABS_MAX + 1);
	dir = anchor == NO_BLOCK ? sift_l0_dir_create(&h.io, SIFT_L0_SHARD_ABS_MAX, NO_BLOCK, 1, flags, xid_floor()) :
		sift_l0_dir_promote(&h.io, anchor, SIFT_L0_SHARD_ABS_MAX, xid_floor());
	sift_href_io_unreserve(&h);
	commit_anchor(rel, part_id, key_lo, dir);
	sift_l0_shm_load(&h.io, shm, dir);
}

SiftL0Route
sift_l0_settle(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPartitionEntry *e, bool href, bool busy)
{
	SiftL0Table *t = table_try();
	SiftL0ShardShm *shm;
	SiftHrefIo	h;
	bool		is_dir = false,
				other_kind = false;

	if (t == NULL)
		return SIFT_L0_COLD;
	if (e->l0_slab_blk != NO_BLOCK)
	{
		const char *pg;

		sift_href_io_init(&h, rel, (uint32) part_id);
		pg = h.io.read_lock(&h, e->l0_slab_blk, SIFT_PT_L0_SLAB);
		is_dir = sift_l0_page_is_dir(pg);
		other_kind = is_dir && ((SIFT_L0_DIR(pg)->dir_flags & SIFT_L0_DIR_HEAPREF) != 0) != href;
		h.io.read_unlock(&h, e->l0_slab_blk);
	}
	if (other_kind)
		return SIFT_L0_COLD;
	if (!is_dir && !partition_is_hot(rel, part_id, busy))
		return SIFT_L0_COLD;
	if ((shm = shm_claim(t, &rel->rd_locator, part_id)) == NULL)
		return is_dir ? SIFT_L0_REFUSED : SIFT_L0_COLD;
	if (is_dir)
	{
		if (pg_atomic_read_u32(&shm->dir_blk) != e->l0_slab_blk)
			sift_l0_shm_load(&h.io, shm, e->l0_slab_blk);
	}
	else
		promote(rel, shm, part_id, key_lo, e->l0_slab_blk, href);
	return SIFT_L0_HOT;
}

/* ---- the flush of a directory ---- */

typedef struct HotFlush
{
	Relation	rel;
	uint64		part_id;
	const uint8 *key_lo;
	SiftPartDesc part;
	bool		has_part;
} HotFlush;

static void
flush_anchor(void *ctx, BlockNumber anchor)
{
	HotFlush   *hf = ctx;

	commit_anchor(hf->rel, hf->part_id, hf->key_lo, anchor);
}

static void
flush_publish(void *ctx, BlockNumber anchor)
{
	HotFlush   *hf = ctx;

	sift_manifest_commit(hf->rel, &(SiftManifestEdit) {.part_id = hf->part_id,.key_lo = hf->key_lo,.set_l0_slab = true,
		.new_l0_slab_blk = anchor,.add_parts = &hf->part,.nadd = hf->has_part});
}

/*
 * The three phases of the copy-on-write flush, under the lock of the partition: the active slabs are sealed and writers go on into a new directory (the shard stripes held for a few pages),
 * the sealed ones are written out as a part (no stripe), and the part and the live directory are committed together (the stripes again). The pages the directories take are reserved before
 * any stripe is held.
 */
void
sift_l0_hot_flush(Relation rel, uint64 part_id, const uint8 *key_lo, BlockNumber anchor)
{
	SiftL0Table *t = table_try();
	const SiftPartSpec *spec = &sift_f2_store_open(rel)->spec;
	SiftL0ShardShm *shm;
	SiftHrefIo	h;
	SiftL0Flush f;
	HotFlush	hf = {.rel = rel,.part_id = part_id,.key_lo = key_lo};
	SiftRowBuf *rb;

	if (t == NULL || (shm = shm_claim(t, &rel->rd_locator, part_id)) == NULL)
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("sift: the shared table of the hot micro-slabs cannot be had")));
	sift_href_io_init(&h, rel, (uint32) part_id);
	if (pg_atomic_read_u32(&shm->dir_blk) != anchor)
		sift_l0_shm_load(&h.io, shm, anchor);
	f = (SiftL0Flush) {.io = &h.io,.gc = &t->gc,.shm = shm,.floor = xid_floor()};
	sift_href_io_reserve(&h, SIFT_L0_SHARD_ABS_MAX + 2);
	sift_l0_flush_seal(&f, flush_anchor, &hf);
	/* check-memory: allow bounded to one micro-slab: the rows the directory held when it was sealed */
	rb = sift_rowbuf_new(spec);
	sift_l0_slab_read(rel, part_id, f.old_dir, rb, spec, NULL, NULL, NULL);
	hf.has_part = sift_l0_slab_write_part(rel, part_id, spec, rb, &hf.part);
	sift_l0_flush_publish(&f, flush_publish, &hf);
	sift_rowbuf_free(rb);
	sift_href_io_unreserve(&h);
	sift_comb_queue_cool(&sift_part_stripes()[sift_part_shard_stripe(&rel->rd_locator, part_id, 0)]);
}

/* ---- what a directory holds ---- */

bool
sift_l0_slab_is_dir(Relation rel, uint64 part_id, BlockNumber blk)
{
	Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);
	bool		dir = sift_l0_page_is_dir((const char *) BufferGetPage(buf));

	UnlockReleaseBuffer(buf);
	return dir;
}

void
sift_l0_dir_pages(Relation rel, uint64 part_id, BlockNumber anchor, void (*visit) (void *ctx, BlockNumber blk, const char *pg), void *ctx)
{
	SiftHrefIo	h;
	SiftL0ScanVisitor v = {.page = visit,.ctx = ctx};

	sift_href_io_init(&h, rel, (uint32) part_id);
	sift_l0_scan(&h.io, anchor, &v, true);
}

/* The pages of a directory while its writers stand still: a hot partition's stripes are held, as a writer changes the directory under them and not under the lock of the partition */
void
sift_l0_dir_pages_frozen(Relation rel, uint64 part_id, BlockNumber anchor, void (*visit) (void *ctx, BlockNumber blk, const char *pg), void *ctx)
{
	SiftL0Table *t = table_try();
	SiftL0ShardShm *shm = t != NULL ? shm_find(t, &rel->rd_locator, part_id) : NULL;
	uint32		held[SIFT_L0_SHARD_ABS_MAX],
				nheld = 0;

	if (shm != NULL && pg_atomic_read_u32(&shm->dir_blk) == anchor)
		nheld = sift_l0_gc_lock_shards(&t->gc, &rel->rd_locator, part_id, pg_atomic_read_u32(&shm->nshards), held);
	sift_l0_dir_pages(rel, part_id, anchor, visit, ctx);
	if (nheld > 0)
		sift_l0_gc_unlock_shards(&t->gc, held, nheld);
}

static void
count_rows(void *ctx, BlockNumber blk, const char *pg)
{
	if (SiftPageGetOpaque((Page) pg)->type == SIFT_PT_L0_SLAB && !sift_l0_page_is_dir(pg))
		*(uint64 *) ctx += SIFT_L0_SL(pg)->page_rows;
}

uint64
sift_l0_dir_rows(Relation rel, uint64 part_id, BlockNumber anchor)
{
	uint64		rows = 0;

	sift_l0_dir_pages(rel, part_id, anchor, count_rows, &rows);
	return rows;
}

static void
add_stamp(void *ctx, BlockNumber blk, const char *pg)
{
	SiftChainStamp *st = ctx;
	const SiftL0SlabPageData *sl = SIFT_L0_SL(pg);

	st->lsn += PageGetLSN((Page) pg);
	st->pages++;
	if (SiftPageGetOpaque((Page) pg)->type != SIFT_PT_L0_SLAB || sift_l0_page_is_dir(pg))
		return;
	st->count += sl->page_rows;
	if (!FullTransactionIdIsValid(st->max_xid) || FullTransactionIdFollows(sl->max_xid, st->max_xid))
		st->max_xid = sl->max_xid;
}

SiftChainStamp
sift_l0_dir_stamp(Relation rel, uint64 part_id, BlockNumber anchor)
{
	SiftChainStamp st = {0};

	sift_l0_dir_pages(rel, part_id, anchor, add_stamp, &st);
	return st;
}
