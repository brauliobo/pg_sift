/*
 * table/f2/chain_cache.c - the logs of f2_chain_cache.h: a few slots, the least recently used goes first, each slot a context of its own under the cache context so that dropping it
 * gives its bytes back.
 *
 * check-memory: allow-allocs a slot holds the log of one partition, whose length the folds of the pending deletes and the compaction of the chain bound, and the budget of core/cachebudget.h bounds the slots
 */
#include "postgres.h"

#include "access/xact.h"
#include "storage/bufmgr.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/cachebudget.h"
#include "core/compat.h"
#include "core/f2/meta.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "table/f2/chain_cache.h"
#include "table/f2/col_delta/batch_enc.h"
#include "table/f2/col_delta/dir.h"

#define CHAIN_SLOTS	8

typedef struct RowIndex
{
	uint32		mask;
	uint64	   *key;
	int32	   *newest;			/* of a key: the record, plus one; 0 for an empty slot */
	int32	   *prev;			/* of a record: the one before it for the same key, -1 for none */
	int			room;			/* the records prev has room for */
} RowIndex;

struct SiftPendDelLog
{
	int			n;
	SiftPendDelRec *recs;
	RowIndex	ix;
};

struct SiftColDeltaLog
{
	SiftColDeltaChain chain;
	int			room;			/* the pages the arena and the blocks of the chain have room for */
	int			n;
	SiftColDeltaEntryHdr *hdr;	/* of a record: its header, with room for ix.room */
	RowIndex	ix;
};

typedef struct PageStamp
{
	BlockNumber blk;
	uint64		gen;
	uint64		lsn;
	uint16		used;
	uint32		crc;
} PageStamp;

typedef struct Slot
{
	MemoryContext cx;			/* NULL: free */
	bool		ready;			/* the log is whole: a statement that errored while it read or extended it leaves it unready, and no lookup finds it */
	bool		pending;		/* else the column delta chain */
	RelFileLocator loc;
	Oid			relid;
	uint32		relhash;		/* sift_f2_rel_hash of the relation, taken when the slot is made */
	uint64		part_id;
	BlockNumber head_blk;
	uint64		manifest_gen;
	PageStamp	stamp;
	uint64		checked_epoch;	/* the snapshot epoch the stamp was last checked in (sift_pend_del_log) */
	uint64		tick;
	union
	{
		SiftPendDelLog pd;
		SiftColDeltaLog cd;
	}			log;
} Slot;

static MemoryContext chain_cx = NULL;
static Slot slots[CHAIN_SLOTS];

#ifdef SIFT_TESTING
uint64		sift_chain_cache_pages = 0;
#endif

/* ---- the index of a log by row ---- */

static uint32
index_slot(const RowIndex *ix, uint64 key)
{
	uint32		p = (uint32) ((key * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & ix->mask;

	while (ix->newest[p] != 0 && ix->key[p] != key)
		p = (p + 1) & ix->mask;
	return p;
}

/* A log of a chain is one allocation of each of its arrays, past 1 GB for a chain of pending deletes that decodes to tens of millions of records */
static void *
log_resize(void *p, Size bytes)
{
	/* check-memory: allow a slot of the chain cache, which the budget of core/cachebudget.h bounds (sift_cache_commit) */
	return p == NULL ? MemoryContextAllocHuge(CurrentMemoryContext, bytes) : repalloc_huge(p, bytes);
}

/* Room for n records: the vector of the links grows by doubling and the table is made again, from its slots, when it would be more than half full */
static void
index_reserve(RowIndex *ix, int n)
{
	Size		cap = pg_nextpower2_size_t(Max((Size) n * 2, 16));

	if (n > ix->room)
	{
		ix->room = (int) Max(n, Min((int64) ix->room * 2, INT_MAX));
		ix->prev = log_resize(ix->prev, sizeof(int32) * (Size) ix->room);
	}
	if (ix->key == NULL || cap > (Size) ix->mask + 1)
	{
		uint64	   *key = ix->key;
		int32	   *newest = ix->newest;
		Size		old = key == NULL ? 0 : (Size) ix->mask + 1;

		ix->mask = (uint32) (cap - 1);
		ix->key = log_resize(NULL, sizeof(uint64) * cap);
		ix->newest = log_resize(NULL, sizeof(int32) * cap);
		memset(ix->newest, 0, sizeof(int32) * cap);
		for (Size i = 0; i < old; i++)
			if (newest[i] != 0)
			{
				uint32		p = index_slot(ix, key[i]);

				ix->key[p] = key[i];
				ix->newest[p] = newest[i];
			}
		if (key != NULL)
		{
			pfree(key);
			pfree(newest);
		}
	}
}

static void
index_add(RowIndex *ix, int rec, uint64 key)
{
	uint32		p = index_slot(ix, key);

	ix->key[p] = key;
	ix->prev[rec] = ix->newest[p] - 1;
	ix->newest[p] = rec + 1;
}

static int
index_newest(const RowIndex *ix, uint64 key)
{
	return ix->newest[index_slot(ix, key)] - 1;
}

/* ---- the slots ---- */

static void
slot_drop(Slot *s)
{
	if (s->cx != NULL)
		MemoryContextDelete(s->cx);
	memset(s, 0, sizeof(*s));
}

static uint64
chain_oldest(void)
{
	uint64		oldest = UINT64_MAX;

	for (int i = 0; i < CHAIN_SLOTS; i++)
		if (slots[i].cx != NULL && !sift_cache_used_in_xact(slots[i].tick))
			oldest = Min(oldest, slots[i].tick);
	return oldest;
}

static bool
chain_evict_one(void)
{
	Slot	   *lru = NULL;

	for (int i = 0; i < CHAIN_SLOTS; i++)
		if (slots[i].cx != NULL && !sift_cache_used_in_xact(slots[i].tick) && (lru == NULL || slots[i].tick < lru->tick))
			lru = &slots[i];
	if (lru == NULL)
		return false;
	slot_drop(lru);
	return true;
}

static void
chain_evict_all(void)
{
	for (int i = 0; i < CHAIN_SLOTS; i++)
		slot_drop(&slots[i]);
}

static const SiftCacheKind chain_kind = {"table_f2_chain_cache", chain_oldest, chain_evict_one, chain_evict_all};

static void
chain_invalidate(uint32 hashvalue)
{
	for (int i = 0; i < CHAIN_SLOTS; i++)
		if (slots[i].cx != NULL && sift_f2_inval_hits(slots[i].relhash, hashvalue))
			slot_drop(&slots[i]);
}

void
sift_chain_cache_forget_rel(Oid relid)
{
	for (int i = 0; i < CHAIN_SLOTS; i++)
		if (slots[i].cx != NULL && slots[i].relid == relid)
			slot_drop(&slots[i]);
}

static void
chain_init(void)
{
	chain_cx = sift_cache_create_context("sift cache f2 table chains");
	sift_cache_register(&chain_kind);
	sift_f2_invalidator_register(chain_invalidate);
}

static Slot *
slot_find(Relation rel, bool pending, uint64 part_id)
{
	for (int i = 0; i < CHAIN_SLOTS; i++)
		if (slots[i].ready && slots[i].pending == pending && slots[i].part_id == part_id && RelFileLocatorEquals(slots[i].loc, rel->rd_locator))
			return &slots[i];
	return NULL;
}

/* The pending deletes of a partition are asked for by the entries of different readers, which may name the chain by different heads (a manifest read before and after the chain grew): each head has its log */
static Slot *
slot_find_head(Relation rel, uint64 part_id, BlockNumber head)
{
	for (int i = 0; i < CHAIN_SLOTS; i++)
		if (slots[i].ready && slots[i].pending && slots[i].part_id == part_id && slots[i].head_blk == head && RelFileLocatorEquals(slots[i].loc, rel->rd_locator))
			return &slots[i];
	return NULL;
}

/* A free or unready slot, else the least recently used one, emptied */
static Slot *
slot_take(void)
{
	Slot	   *lru = &slots[0];

	for (int i = 0; i < CHAIN_SLOTS; i++)
	{
		if (!slots[i].ready)
		{
			slot_drop(&slots[i]);
			return &slots[i];
		}
		if (slots[i].tick < lru->tick)
			lru = &slots[i];
	}
	slot_drop(lru);
	return lru;
}

static Slot *
slot_open(Relation rel, bool pending, uint64 part_id)
{
	Slot	   *s = slot_take();

	s->cx = sift_mem_allocset_create(chain_cx, "sift cache f2 chain log");
	s->pending = pending;
	s->loc = rel->rd_locator;
	s->relid = RelationGetRelid(rel);
	s->relhash = sift_f2_rel_hash(rel);
	s->part_id = part_id;
	return s;
}

static PageStamp
stamp_of_page(const char *page, BlockNumber blk)
{
	return (PageStamp) {blk, SiftPageGetOpaque((Page) page)->gen, PageGetLSN((Page) page), (uint16) SiftPageGetUsed((Page) page),
		*(const uint32 *) (SiftPageGetData((Page) page) + SIFT_PAGE_CRC_OFF)};
}

static PageStamp
stamp_read(Relation rel, BlockNumber blk)
{
	Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);
	PageStamp	st = stamp_of_page(BufferGetPage(buf), blk);

	UnlockReleaseBuffer(buf);
	return st;
}

static bool
stamp_same(const PageStamp *a, const PageStamp *b)
{
	return a->blk == b->blk && a->gen == b->gen && a->lsn == b->lsn && a->used == b->used && a->crc == b->crc;
}

/* ---- the pending deletes ---- */

static uint64
pd_key(const SiftPendDelRec *r)
{
	return r->tid & SIFT_PD_TID_MASK;
}

#define PD_LOAD_TRIES	4

/*
 * The records, then the stamp of the last page, then the count of the records the pages hold: when it is the count read nothing was appended since the records were, so the stamp is the one of
 * the records. Writers that append at every try leave the records of the last one, whole for this read but not the ones of its stamp: false, the slot is not found again.
 */
static bool
pd_load(Relation rel, const SiftPartitionEntry *entry, Slot *s)
{
	SiftPendDelLog *l = &s->log.pd;
	BlockNumber *pages;
	int			npages;
	bool		settled = false;

	for (int tries = 1;; tries++)
	{
		CHECK_FOR_INTERRUPTS();
		l->recs = sift_pend_del_read(rel, entry->part_id, entry->pend_del_blk, &l->n, &pages, &npages);
		s->stamp = stamp_read(rel, pages[npages - 1]);
		pfree(pages);
		SIFT_POINT("sift-pend-del-loaded");
		settled = sift_pend_del_stamp(rel, entry->part_id, entry->pend_del_blk).count == (uint32) l->n;
		if (settled || tries == PD_LOAD_TRIES)
			break;
		pfree(l->recs);
	}
	index_reserve(&l->ix, l->n);
	for (int i = 0; i < l->n; i++)
		if ((l->recs[i].tid & SIFT_PD_TID_SUCC) == 0)
			index_add(&l->ix, i, pd_key(&l->recs[i]));
		else
			l->ix.prev[i] = -1;
	return settled;
}

const SiftPendDelLog *
sift_pend_del_log(Relation rel, const SiftPartitionEntry *entry, uint64 epoch)
{
	Slot	   *s;
	MemoryContext old;

	if (entry->pend_del_blk == InvalidBlockNumber)
		return NULL;
	if (chain_cx == NULL)
		chain_init();
	s = slot_find_head(rel, entry->part_id, entry->pend_del_blk);
	if (s != NULL && epoch != 0 && s->checked_epoch == epoch)
	{
		s->tick = sift_cache_tick();
		return &s->log.pd;
	}
	if (s != NULL)
	{
		PageStamp now = stamp_read(rel, s->stamp.blk);

		if (stamp_same(&now, &s->stamp))
		{
			s->checked_epoch = epoch;
			s->tick = sift_cache_tick();
			return &s->log.pd;
		}
		slot_drop(s);
	}
	s = slot_open(rel, true, entry->part_id);
	old = MemoryContextSwitchTo(s->cx);
	s->head_blk = pd_load(rel, entry, s) ? entry->pend_del_blk : InvalidBlockNumber;
	MemoryContextSwitchTo(old);
	s->checked_epoch = epoch;
	s->tick = sift_cache_tick();
	s->ready = true;
	(void) sift_cache_commit();
	return &s->log.pd;
}

int
sift_pend_del_log_newest(const SiftPendDelLog *l, uint64 tid)
{
	return index_newest(&l->ix, tid & SIFT_PD_TID_MASK);
}

int
sift_pend_del_log_prev(const SiftPendDelLog *l, int i)
{
	return l->ix.prev[i];
}

int
sift_pend_del_log_count(const SiftPendDelLog *l)
{
	return l->n;
}

const SiftPendDelRec *
sift_pend_del_log_rec(const SiftPendDelLog *l, int i)
{
	return &l->recs[i];
}

/* ---- the column delta chain ---- */

/* The records of a page from p (NULL: its first) */
static int
cd_page_records(const char *page, const uint8 *p)
{
	const uint8 *end = sift_col_delta_page_end(page);
	SiftColDeltaEntryHdr h;
	int			n = 0;

	for (p = p == NULL ? sift_col_delta_page_first(page) : p; p < end; p += h.rec_bytes)
	{
		memcpy(&h, p, sizeof(h));
		n += (int) sift_col_delta_rec_rows(p);
	}
	return n;
}

/* Room for n records in the log, taken before the records are indexed so that nothing that can fail comes between the index and its records */
static void
cd_reserve(SiftColDeltaLog *l, int n)
{
	int			room = l->ix.room;

	index_reserve(&l->ix, n);
	if (l->ix.room != room)
		l->hdr = log_resize(l->hdr, sizeof(SiftColDeltaEntryHdr) * (Size) l->ix.room);
}

/* The records of page i of the arena from p (NULL: its first), into the log, which has room for them */
static void
cd_index_page(SiftColDeltaLog *l, int i, const uint8 *p)
{
	const char *page = sift_col_delta_chain_page(&l->chain, i);
	SiftColDeltaRecs recs;
	const uint8 *rec;

	sift_col_delta_recs_init_hdrs(&recs, p == NULL ? sift_col_delta_page_first(page) : p, sift_col_delta_page_end(page));
	for (; sift_col_delta_recs_next(&recs, &rec, &l->hdr[l->n]); l->n++)
		index_add(&l->ix, l->n, sift_col_delta_hdr_tid(&l->hdr[l->n]));
}

static void
cd_index(SiftColDeltaLog *l)
{
	int			nrecs = 0;

	for (int i = 0; i < l->chain.npages; i++)
		nrecs += cd_page_records(sift_col_delta_chain_page(&l->chain, i), NULL);
	cd_reserve(l, nrecs);
	for (int i = 0; i < l->chain.npages; i++)
		cd_index_page(l, i, NULL);
}

static void
cd_room_pages(SiftColDeltaLog *l, int npages)
{
	if (npages <= l->room)
		return;
	l->room = Max(npages, l->room + l->room / 2);
	l->chain.arena = repalloc(l->chain.arena, (Size) l->room * BLCKSZ);
	l->chain.blks = repalloc(l->chain.blks, sizeof(BlockNumber) * l->room);
}

/* The page of the chain the log holds as its newest is still that page with records after its own: the same page (its generation, depth and link), and its records the ones it held */
static bool
cd_same_head(const char *held, const char *now)
{
	const SiftColDeltaPageData *a = sift_col_delta_page_hdr(held),
			   *b = sift_col_delta_page_hdr(now);

	return SiftPageGetOpaque((Page) held)->gen == SiftPageGetOpaque((Page) now)->gen && a->chain_pages == b->chain_pages && a->prev_blk == b->prev_blk &&
		SiftPageGetUsed(now) >= SiftPageGetUsed(held) &&
		memcmp(sift_col_delta_page_first(held), sift_col_delta_page_first(now), sift_col_delta_page_end(held) - sift_col_delta_page_first(held)) == 0;
}

/* The newest page of the log as the chain has it now and the pages after it, read and checked; false when the chain is not the log's with pages added */
static bool
cd_read_newer(SiftColDeltaLog *l, const SiftColDeltaPageReader *reader, const SiftColDeltaDirEntry *ent, int npages, char *now)
{
	SiftColDeltaChain *c = &l->chain;
	int			held = c->npages;
	bool		same = true;

	for (int i = 0; i < held && same; i++)
		same = ent[i].blkno == c->blks[i];
	same = same && reader->read(reader->ctx, c->blks[held - 1], now) == NULL && sift_col_delta_page_check(now) == NULL &&
		cd_same_head(sift_col_delta_chain_page(c, held - 1), now);
	cd_room_pages(l, npages);
	for (int i = held; i < npages && same; i++)
	{
		char	   *dst = c->arena + (Size) i * BLCKSZ;

		same = reader->read(reader->ctx, ent[i].blkno, dst) == NULL && sift_col_delta_page_check(dst) == NULL &&
			sift_col_delta_check_link(dst, i + 1, ent[i - 1].blkno) == NULL;
		c->blks[i] = ent[i].blkno;
	}
	return same;
}

/*
 * The chain of the entry is the one the log holds with its newest page grown and pages after it: of an indexed chain the directory names the pages, the ones the log holds are the
 * first of them and the newest of those is the page it was, so the records of the log stay and only what the chain gained is read and indexed. Pages are only ever added to the
 * newest end of a chain, and a chain that was folded or rewritten is made of pages of other generations. The slot is the current memory context.
 */
static bool
cd_extend(Relation rel, const SiftPartitionEntry *entry, Slot *s)
{
	SiftColDeltaLog *l = &s->log.cd;
	SiftColDeltaChain *c = &l->chain;
	SiftColDeltaAnchor a = sift_manifest_read_col_delta(rel, entry);
	SiftColDeltaDiskReader d = {.rel = rel,.part_id = (uint32) entry->part_id};
	SiftColDeltaPageReader reader = sift_col_delta_disk_reader(&d);
	SiftColDeltaDirEntry *ent;
	BlockNumber *dir_blks;
	char	   *now;
	char	   *head;
	int			ndir,
				held = c->npages,
				had = SiftPageGetUsed(sift_col_delta_chain_page(c, held - 1)),
				nnew;

	if (!sift_col_delta_anchor_sane(&a) || (a.flags & SIFT_CDLA_INDEXED_DIR) == 0 || a.npages < held ||
		sift_cddir_chain_entries(&a, &reader, &ent, &dir_blks, &ndir) != NULL)
		return false;
	if (!sift_col_delta_log_fits(&a))
		return false;
	now = palloc(BLCKSZ);
	if (!cd_read_newer(l, &reader, ent, a.npages, now))
		return false;
	head = c->arena + (Size) (held - 1) * BLCKSZ;
	nnew = cd_page_records(now, (const uint8 *) SiftPageGetData(now) + had);
	for (int i = held; i < a.npages; i++)
		nnew += cd_page_records(sift_col_delta_chain_page(c, i), NULL);
	cd_reserve(l, l->n + nnew);
	memcpy(head, now, BLCKSZ);
	cd_index_page(l, held - 1, (const uint8 *) SiftPageGetData(head) + had);
	for (int i = held; i < a.npages; i++)
		cd_index_page(l, i, NULL);
	c->npages = c->nchain = a.npages;
#ifdef SIFT_TESTING
	sift_chain_cache_pages += a.npages - held + 1;
#endif
	if (c->dir_blks != NULL)
		pfree(c->dir_blks);
	c->dir_blks = dir_blks;
	c->ndir = ndir;
	s->stamp = stamp_of_page(sift_col_delta_chain_page(c, a.npages - 1), c->blks[a.npages - 1]);
	s->head_blk = entry->manifest_blk;
	s->manifest_gen = entry->manifest_gen;
	pfree(now);
	pfree(ent);
	return true;
}

static bool
cd_hit(Relation rel, const SiftPartitionEntry *entry, const Slot *s)
{
	PageStamp	now;

	if (s->head_blk != entry->manifest_blk || s->manifest_gen != entry->manifest_gen)
		return false;
	now = stamp_read(rel, s->stamp.blk);
	return stamp_same(&now, &s->stamp);
}

#define LOG_ROWS_PER_PAGE		(BLCKSZ / 3)	/* a row of a batch takes 3 bytes at the least */
#define LOG_BYTES_PER_ROW		100				/* its place, its links, its slot of the index and the record it was */
#define LOG_BYTES_PER_PAGE		(BLCKSZ + LOG_ROWS_PER_PAGE * LOG_BYTES_PER_ROW)

bool
sift_col_delta_log_fits(const SiftColDeltaAnchor *anchor)
{
	return anchor->npages <= Max(SIFT_CDDIR_FLAT_PAGES, (uint64) sift_backend_cache_kb * 1024 / (4 * LOG_BYTES_PER_PAGE));
}

const SiftColDeltaLog *
sift_col_delta_log(Relation rel, const SiftPartitionEntry *entry)
{
	Slot	   *s;
	MemoryContext old;
	bool		has;

	if ((entry->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		return NULL;
	if (chain_cx == NULL)
		chain_init();
	s = slot_find(rel, false, entry->part_id);
	if (s != NULL && cd_hit(rel, entry, s))
	{
		s->tick = sift_cache_tick();
		return &s->log.cd;
	}
	if (s != NULL)
	{
		s->ready = false;
		old = MemoryContextSwitchTo(s->cx);
		has = cd_extend(rel, entry, s);
		MemoryContextSwitchTo(old);
		if (has)
		{
			s->ready = true;
			s->tick = sift_cache_tick();
			(void) sift_cache_commit();
			return &s->log.cd;
		}
		slot_drop(s);
	}
	s = slot_open(rel, false, entry->part_id);
	SIFT_POINT("sift-col-delta-log-load");
	old = MemoryContextSwitchTo(s->cx);
	has = sift_col_delta_load_chain(rel, entry, 0, &s->log.cd.chain);
	if (has)
	{
		SiftColDeltaChain *c = &s->log.cd.chain;

		s->log.cd.room = c->npages;
#ifdef SIFT_TESTING
		sift_chain_cache_pages += c->npages;
#endif
		s->stamp = stamp_of_page(sift_col_delta_chain_page(c, c->npages - 1), c->blks[c->npages - 1]);
		cd_index(&s->log.cd);
	}
	MemoryContextSwitchTo(old);
	if (!has)
	{
		slot_drop(s);
		return NULL;
	}
	s->head_blk = entry->manifest_blk;
	s->manifest_gen = entry->manifest_gen;
	s->tick = sift_cache_tick();
	s->ready = true;
	(void) sift_cache_commit();
	return &s->log.cd;
}

int
sift_col_delta_log_newest(const SiftColDeltaLog *l, uint64 tid)
{
	return index_newest(&l->ix, tid);
}

int
sift_col_delta_log_prev(const SiftColDeltaLog *l, int i)
{
	return l->ix.prev[i];
}

const SiftColDeltaEntryHdr *
sift_col_delta_log_hdr(const SiftColDeltaLog *l, int i)
{
	return &l->hdr[i];
}
