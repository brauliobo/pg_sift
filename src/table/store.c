/*
 * table/store.c - the layout of a sift_table relation (docs/COL-TAM.md section 2): the per-relation cache, write
 * buffers (row format) and the tombstone segment. Pages and the directory are those of core/store.h.
 *
 * The page chains (a write buffer's data pages, the tombstone log) are the lane 0 of the lane segments of core/lane.h, which also
 * seals them and takes the segment directory swaps; what differs from the record segments of the BM25 and column indexes is the
 * page format (slots of rows, tombstone entries), the single lane (a row must keep its vblock = position in the chain) and that
 * rows go to any unsealed page a backend holds, not just the tail. Lock order as there: the chain tail, the newly allocated page,
 * then the segment header page. Appenders of rows hold their target page only; a reader never holds two buffers.
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/lane.h"
#include "table/table.h"
#include "table/store_int.h"

int			sift_tbl_write_buffer_limit = 4096;	/* kB */
int			sift_tbl_vblock_floor = 0;	/* testing: the first virtual block of a new write buffer at least */
int			sift_tbl_max_segments = SIFT_MAX_SEGMENTS;	/* the directory entries a table may use (lowered by a test setting) */

typedef struct CacheEnt
{
	Oid			relid;
	SiftTblRel *st;
} CacheEnt;

static HTAB *cache_tab;
static List *retired;			/* contexts of cache entries replaced while a scan may still use them: freed at transaction end */

static void
cache_inval(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS seq;
	CacheEnt   *e;

	if (cache_tab == NULL)
		return;
	hash_seq_init(&seq, cache_tab);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (relid == InvalidOid || e->relid == relid)
			e->st->invalid = true;
}

static void
cache_xact(XactEvent event, void *arg)
{
	HASH_SEQ_STATUS seq;
	CacheEnt   *e;

	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT)
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
}

/* The backend's caches go before its exit (the scans that the group reads keep between statements are not leaks to the leak check) */
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
		MemoryContextDelete(e->st->cxt);
}

SiftTblRel *
sift_tbl_rel(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	bool		found;
	CacheEnt   *e;

	if (cache_tab == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid),.entrysize = sizeof(CacheEnt),.hcxt = CacheMemoryContext};

		cache_tab = hash_create("sift_table relations", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		CacheRegisterRelcacheCallback(cache_inval, (Datum) 0);
		RegisterXactCallback(cache_xact, NULL);
		on_proc_exit(cache_exit, 0);
	}
	e = hash_search(cache_tab, &relid, HASH_ENTER, &found);
	if (found && (e->st->invalid || e->st->relnumber != rel->rd_locator.relNumber))
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		retired = lappend(retired, e->st->cxt);
		MemoryContextSwitchTo(old);
		found = false;
	}
	if (!found)
	{
		MemoryContext cxt = AllocSetContextCreate(CacheMemoryContext, "sift cache table", ALLOCSET_DEFAULT_SIZES);

		e->st = MemoryContextAllocZero(cxt, sizeof(SiftTblRel));
		e->st->relid = relid;
		e->st->relnumber = rel->rd_locator.relNumber;
		e->st->cxt = cxt;
		e->st->tgt_blk = InvalidBlockNumber;
		e->st->tomb_last = InvalidBlockNumber;
	}
	return e->st;
}

bool
sift_tbl_is_table(Relation rel)
{
	return rel->rd_tableam == sift_tbl_routine();
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

static SiftSegEntry
new_tomb_segment(Relation rel)
{
	uint64		gen = sift_store_new_gen(rel);
	Buffer		hbuf = sift_alloc_page(rel, SIFT_PT_WBUFHEADER, gen);
	Buffer		dbuf = sift_alloc_page(rel, SIFT_PT_WBUFDATA, gen);
	GenericXLogState *state = GenericXLogStart(rel);
	Page		hp = GenericXLogRegisterBuffer(state, hbuf, 0);
	Page		dp = GenericXLogRegisterBuffer(state, dbuf, 0);
	SiftSegEntry e = {.gen = gen,.kind = SIFT_SEG_WBUF_OPEN,.flags = SIFT_TBL_SEG_TOMB,.header_blk = BufferGetBlockNumber(hbuf)};

	sift_lane_init_header(hp, gen, BufferGetBlockNumber(dbuf), 1);
	TBL_TOMB_HDR(hp)->magic = SIFT_TBL_TOMB_MAGIC;
	SiftPageSetUsed(hp, MAXALIGN(sizeof(SiftWbufHeaderData)) + sizeof(SiftTblTombHdr));
	sift_lane_init_page(dp, TBL_TOMB_PAGE_INIT);
	GenericXLogFinish(state);
	UnlockReleaseBuffer(hbuf);
	UnlockReleaseBuffer(dbuf);
	return e;
}

void
sift_tbl_ensure_init(Relation rel)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	if (st->initialized)
		return;
	if (RelationGetNumberOfBlocks(rel) == 0)
	{
		static const SiftOptions none = {0};

		LockRelationForExtension(rel, ExclusiveLock);
		if (RelationGetNumberOfBlocks(rel) == 0)
		{
			SiftWriteTarget *wt = sift_wt_begin(rel, SIFT_WRITE_BULK);

			sift_store_init_meta(wt, &none, 0);
			sift_wt_finish(wt);
		}
		UnlockRelationForExtension(rel, ExclusiveLock);
	}
	sift_tbl_refresh(rel, st);
	if (find_seg(&st->dir, is_tomb, 0) == NULL)
	{
		InitArg		arg;

		LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		sift_dir_read(rel, &st->dir);
		if (find_seg(&st->dir, is_tomb, 0) == NULL)
		{
			arg.tomb = new_tomb_segment(rel);
			sift_dir_update(rel, &(SiftDirChange) {.fn = init_dir,.arg = &arg,.extra = InvalidBuffer});
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

	if (RelationGetNumberOfBlocks(rel) == 0)
	{
		st->initialized = false;
		st->dir.nsegs = 0;
		st->nwbuf = 0;
		return;
	}
	sift_dir_read(rel, &st->dir);
	if (st->dir.nsegs > 0 && !(st->dir.meta.flags & SIFT_TBL_META_FLAG))	/* flagged with the first segment */
		corrupt(rel, "The metapage does not carry the sift_table flag (0x%x).", st->dir.meta.flags);
	st->initialized = true;
	for (int i = 0; i < st->dir.nsegs; i++)
		n += is_wbuf(&st->dir.segs[i]);
	st->wbuf = MemoryContextAllocZero(st->cxt, Max(n, 1) * sizeof(SiftTblSeg));
	st->nwbuf = 0;
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];
		SiftTblSeg *s;

		if (!is_wbuf(e))
			continue;
		s = &st->wbuf[st->nwbuf++];
		for (int j = 0; j < nold; j++)
			if (old[j].gen == e->gen)
				*s = old[j];
		s->gen = e->gen;
		s->hdr = e->header_blk;
		s->vbase = (uint64) e->sb_lo * SIFT_TBL_VBLOCKS_SB;
		s->vcap = (e->sb_hi - e->sb_lo + 1) * SIFT_TBL_VBLOCKS_SB;
	}
	st->wbuf_version = st->dir.version;
	if (old)
		pfree(old);
}

/* Every directory update bumps its version: a cache that holds the current one has nothing to read (the metapage fields that change without an update, the high-water mark and the generation counter, are not the cache's: sift_tbl_refresh_full) */
void
sift_tbl_refresh(Relation rel, SiftTblRel *st)
{
	if (st->initialized && st->wbuf != NULL && sift_dir_version(rel) == st->wbuf_version)
		return;
	sift_tbl_refresh_full(rel, st);
}

uint64
sift_tbl_nvblocks(Relation rel)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	sift_tbl_refresh(rel, st);
	return st->initialized ? SIFT_TBL_NEXT_VBLOCK(&st->dir.meta) : 0;
}
