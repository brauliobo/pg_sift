/*
 * table/v1/store.c - the layout of a sift_table relation (docs/COL-TAM.md section 2): the per-relation cache, write
 * buffers (row format) and the tombstone segment. Pages and the directory are those of core/store/store.h.
 *
 * The page chains (a write buffer's data pages, the tombstone log) are the lane 0 of the lane segments of core/lane.h, which also
 * seals them and takes the segment directory swaps; what differs from the record segments of the BM25 and column indexes is the
 * page format (slots of rows, tombstone entries), the single lane (a row must keep its vblock = position in the chain) and that
 * rows go to any unsealed page a backend holds, not just the tail. Lock order as there: the chain tail, the newly allocated page,
 * then the segment header page. Appenders of rows hold their target page only; a reader never holds two buffers.
 */
#include "postgres.h"

#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/ipc.h"
#include "core/cachebudget.h"
#include "core/dispatch.h"
#include "core/maint/maint.h"
#include "core/wal_conflict.h"
#include "core/extlock.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "table/v1/keyidx.h"
#include "table/v1/table.h"
#include "table/v1/store_int.h"

int			sift_tbl_write_buffer_limit = 4096;	/* kB */
bool		sift_tbl_wide_rows = true;	/* a row that does not fit a write buffer goes to tail pages (format 11) instead of being refused */
int			sift_tbl_vblock_floor = 0;	/* testing: the first virtual block of a new write buffer at least */
int			sift_tbl_max_segments = SIFT_DIR_POLICY_MAX;	/* the directory entries a table may use (lowered by a test setting) */
int			sift_tbl_init_gate = 0;	/* testing: the first writer waits for this advisory lock before it writes the tombstone segment */
int			sift_tbl_seal_gate = 0;	/* testing: the writer that closed a write buffer waits for this advisory lock before it flags the pages sealed */

typedef struct CacheEnt
{
	Oid			relid;
	SiftTblRel *st;
	uint64		used;			/* sift_cache_tick() of the last ask for it */
} CacheEnt;

static HTAB *cache_tab;
static List *retired;			/* contexts of cache entries replaced while a scan may still use them: freed at transaction end */

static void
cache_xact(XactEvent event, void *arg)
{
	HASH_SEQ_STATUS seq;
	CacheEnt   *e;

	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT && event != XACT_EVENT_PREPARE)
		return;
	foreach_ptr(MemoryContextData, cxt, retired)
		MemoryContextDelete(cxt);
	list_free(retired);
	retired = NIL;
	if (cache_tab == NULL)
		return;
	hash_seq_init(&seq, cache_tab);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (e->st->invalid)
		{
			MemoryContextDelete(e->st->cxt);
			hash_search(cache_tab, &e->relid, HASH_REMOVE, NULL);
		}
		else
		{
			sift_tbl_fetch_trim(e->st);
			sift_tomb_xact_end(&e->st->tomb);
		}
}

static CacheEnt *
cache_oldest_ent(void)
{
	HASH_SEQ_STATUS seq;
	CacheEnt   *e;
	CacheEnt   *victim = NULL;

	hash_seq_init(&seq, cache_tab);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (!sift_cache_used_in_xact(e->used) && (victim == NULL || e->used < victim->used))
			victim = e;
	return victim;
}

static uint64
cache_oldest(void)
{
	CacheEnt   *e = cache_oldest_ent();

	return e == NULL ? UINT64_MAX : e->used;
}

static bool
cache_evict_one(void)
{
	CacheEnt   *e = cache_oldest_ent();

	if (e == NULL)
		return false;
	MemoryContextDelete(e->st->cxt);
	hash_search(cache_tab, &e->relid, HASH_REMOVE, NULL);
	return true;
}

static void
cache_evict_all(void)
{
	while (cache_evict_one())
		;
}

static const SiftCacheKind cache_kind = {"table_v1_store", cache_oldest, cache_evict_one, cache_evict_all};

/*
 * The backend's caches go before its exit (the scans that the group reads keep between statements are not leaks to the leak check). Before the shared memory exit, not at
 * the process exit: the tombstone runs are inter-transaction temp files, which fd.c closes in its own before_shmem_exit callback since 19, and closing them again fails
 */
static void
cache_exit(int code, Datum arg)
{
	HASH_SEQ_STATUS seq;
	CacheEnt   *e;

	foreach_ptr(MemoryContextData, cxt, retired)
		MemoryContextDelete(cxt);
	retired = NIL;
	hash_seq_init(&seq, cache_tab);
	while ((e = hash_seq_search(&seq)) != NULL)
	{
		MemoryContextDelete(e->st->cxt);
		hash_search(cache_tab, &e->relid, HASH_REMOVE, NULL);
	}
}

SiftTblRel *
sift_tbl_rel(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	bool		found;
	CacheEnt   *e;
	const SiftTableFormatOps *ops;

	sift_tbl_require(rel);
	ops = sift_tbl_ops(rel);

	/* a function of Format 1 asked about a table of another format: its relcache entry must keep that format's state */
	if (ops != &sift_tbl_v1_ops)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("sift_table \"%s\" is a table of Format %u.0, which this function does not read", RelationGetRelationName(rel), ops->format_major),
						errdetail("The function reads the segments of a sift_table of Format 1."),
						errhint("sift.parts, sift.space and sift.check describe a table of any format.")));
	if (cache_tab == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid),.entrysize = sizeof(CacheEnt),.hcxt = CacheMemoryContext};

		cache_tab = hash_create("sift_table relations", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		sift_xact_callback(cache_xact, NULL);
		before_shmem_exit(cache_exit, 0);
		sift_cache_register(&cache_kind);
	}
	e = hash_search(cache_tab, &relid, HASH_ENTER, &found);
	e->used = sift_cache_tick();
	/*
	 * The relcache frees rd_amcache with the entry of the relation when it invalidates it: a table that has a cache entry and no mark was invalidated since.
	 * No relcache callback for it: a backend has ten of those slots for all its libraries.
	 */
	if (rel->rd_amcache == NULL)
	{
		rel->rd_amcache = MemoryContextAlloc(CacheMemoryContext, sizeof(SiftTblRelHeader));
		((SiftTblRelHeader *) rel->rd_amcache)->ops = &sift_tbl_v1_ops;
		if (found)
			e->st->invalid = true;
	}
	if (found && (e->st->invalid || e->st->relnumber != rel->rd_locator.relNumber))
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		retired = lappend(retired, e->st->cxt);
		MemoryContextSwitchTo(old);
		found = false;
	}
	if (!found)
	{
		/* MEM[MEM-TBL-18]: v1 table relcache ctx (cachebudget child; the oldest one no statement of the transaction used is dropped for room) */
		MemoryContext cxt = sift_cache_create_context("sift cache table");

		e->st = MemoryContextAllocZero(cxt, sizeof(SiftTblRel));
		e->st->relid = relid;
		e->st->relnumber = rel->rd_locator.relNumber;
		e->st->cxt = cxt;
		e->st->tgt_blk = InvalidBlockNumber;
		e->st->dir = MemoryContextAllocZero(cxt, offsetof(SiftDirSnapshot, segs));
		sift_tomb_init(&e->st->tomb, cxt, &e->st->dir);
		e->st->tomb.spill = true;
		sift_tbl_opts_load(e->st);
	}
	return e->st;
}

/* The first layout of the relation: meta, directory, the tombstone segment */
typedef struct InitArg
{
	SiftSegEntry tomb;
} InitArg;

static void
init_dir(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	meta->flags |= SIFT_TBL_META_FLAG;
	dir->segs[dir->nsegs++] = ((InitArg *) arg)->tomb;
}

/* The first writer writes the metapage and then the directory, two blocks one after the other: a file of the metapage alone is being laid out */
#define BLOCKS_WRITTEN(rel)	(RelationGetNumberOfBlocks(rel) > SIFT_DIRECTORY_BLKNO)

void
sift_tbl_ensure_init(Relation rel)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	/* a read between the first writer's metapage and its tombstone segment caches a table that is not laid out yet */
	if (st->initialized && sift_tomb_find_seg(st->dir) != NULL)
		return;
	if (!BLOCKS_WRITTEN(rel))
	{
		static const SiftOptions none = {0};

		sift_extension_lock(rel);
		if (RelationGetNumberOfBlocks(rel) == 0)
		{
			SiftWriteTarget *wt = sift_wt_begin(rel, SIFT_WRITE_LAYOUT);

			sift_store_init_meta(wt, &none, 0, "");
			sift_wt_finish(wt);
		}
		sift_extension_unlock(rel);
	}
	sift_tbl_refresh(rel, st);
	if (sift_tomb_find_seg(st->dir) == NULL)
	{
		InitArg		arg;

		sift_tbl_gate(sift_tbl_init_gate);
		LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		sift_dir_reload(rel, &st->dir, st->cxt);
		if (sift_tomb_find_seg(st->dir) == NULL)
		{
			arg.tomb = sift_tomb_new_segment(rel, SIFT_SEG_WBUF_OPEN, 0);
			sift_dir_update(rel, &(SiftDirChange) {.fn = init_dir,.arg = &arg,.nadd = 1,.extra = InvalidBuffer});
		}
		UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		sift_tbl_refresh(rel, st);
	}
}

void
sift_tbl_refresh_full(Relation rel, SiftTblRel *st)
{
	SiftTblSeg *old = st->wbuf;
	int			nold = st->nwbuf,
				n = 0;
	KeyIdx		old_gen = {0};

	if (!BLOCKS_WRITTEN(rel))
	{
		st->initialized = false;
		st->dir->nsegs = 0;
		st->nwbuf = 0;
		return;
	}
	sift_dir_reload(rel, &st->dir, st->cxt);
	if (st->dir->nsegs > 0 && !(st->dir->meta.flags & SIFT_TBL_META_FLAG))	/* flagged with the first segment */
		corrupt(rel, "The metapage does not carry the sift_table flag (0x%x).", st->dir->meta.flags);
	sift_cdict_retire(rel, st->dir);
	st->initialized = true;
	for (int i = 0; i < st->dir->nsegs; i++)
		n += is_wbuf(&st->dir->segs[i]);
	st->wbuf = MemoryContextAllocZero(st->cxt, Max(n, 1) * sizeof(SiftTblSeg));
	st->nwbuf = 0;
	for (int j = 0; j < nold; j++)
		keyidx_put(CurrentMemoryContext, &old_gen, old[j].gen, j);
	for (int i = 0; i < st->dir->nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir->segs[i];
		SiftTblSeg *s;
		int			j;

		if (!is_wbuf(e))
			continue;
		s = &st->wbuf[st->nwbuf++];
		if ((j = keyidx_find(&old_gen, e->gen)) >= 0)
			*s = old[j];
		s->gen = e->gen;
		s->hdr = e->header_blk;
		s->vbase = (uint64) e->sb_lo * SIFT_TBL_VBLOCKS_SB;
		s->vcap = (e->sb_hi - e->sb_lo + 1) * SIFT_TBL_VBLOCKS_SB;
	}
	st->wbuf_version = st->dir->version;
	keyidx_free(&old_gen);
	if (old)
		pfree(old);
}

/* A copy of n write buffers in cxt. A table with no page has none, and then the cache holds no array to copy (memcpy takes no null pointer, whatever the size) */
SiftTblSeg *
sift_tbl_wbuf_copy(const SiftTblSeg *wbuf, int n, MemoryContext cxt)
{
	SiftTblSeg *copy = MemoryContextAlloc(cxt, Max(n, 1) * sizeof(SiftTblSeg));

	if (n > 0)
		memcpy(copy, wbuf, n * sizeof(SiftTblSeg));
	return copy;
}

/* Every directory update bumps its version: a cache that holds the current one has nothing to read (the metapage fields that change without an update, the high-water mark and the generation counter, are not the cache's: sift_tbl_refresh_full) */
void
sift_tbl_refresh(Relation rel, SiftTblRel *st)
{
	if (st->initialized && st->wbuf != NULL && sift_dir_version(rel) == st->wbuf_version)
		return;
	sift_tbl_refresh_full(rel, st);
}

/*
 * The maintenance lock, and the directory as it is once the lock is held: whoever held it before may have promoted or merged the segments the cache lists.
 * The pass then freezes insertions and drops deletions older than oldest, which a standby snapshot that is not past them still reads otherwise: its replay
 * ends those snapshots first, as the freeze horizon of a heap VACUUM does (core/wal_conflict.h)
 */
bool
sift_tbl_maint_lock(Relation rel, SiftTblRel *st, bool wait, TransactionId oldest)
{
	FullTransactionId horizon = FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(), oldest);

	if (!sift_maint_lock(rel, wait))
		return false;
	sift_tbl_refresh(rel, st);
	FullTransactionIdRetreat(&horizon);
	(void) sift_wal_snapshot_conflict(rel, InvalidBlockNumber, horizon);
	return true;
}

uint64
sift_tbl_nvblocks(Relation rel)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	sift_tbl_refresh(rel, st);
	return st->initialized ? SIFT_TBL_NEXT_VBLOCK(&st->dir->meta) : 0;
}
