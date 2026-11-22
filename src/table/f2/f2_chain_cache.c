/*
 * table/f2/f2_chain_cache.c - the logs of f2_chain_cache.h: a few slots, the least recently used goes first, each slot a context of its own under the cache context so that dropping it
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
#include "core/memtrack.h"
#include "core/page_io.h"
#include "table/f2/f2_chain_cache.h"

#define CHAIN_SLOTS	8

typedef struct RowIndex
{
	uint32		mask;
	uint64	   *key;
	int32	   *newest;			/* of a key: the record, plus one; 0 for an empty slot */
	int32	   *prev;			/* of a record: the one before it for the same key, -1 for none */
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
	int			n;
	uint32	   *off;			/* of a record: its place in the arena of the chain */
	RowIndex	ix;
};

typedef struct PageStamp
{
	BlockNumber blk;
	uint64		gen;
	uint64		lsn;
} PageStamp;

typedef struct Slot
{
	MemoryContext cx;			/* NULL: free */
	bool		pending;		/* else the column delta chain */
	RelFileLocator loc;
	Oid			relid;
	uint64		part_id;
	BlockNumber head_blk;
	uint64		manifest_gen;
	PageStamp	stamp;
	uint64		tick;
	union
	{
		SiftPendDelLog pd;
		SiftColDeltaLog cd;
	}			log;
} Slot;

static MemoryContext chain_cx = NULL;
static Slot slots[CHAIN_SLOTS];

/* ---- the index of a log by row ---- */

static void
index_init(RowIndex *ix, int n)
{
	uint32		cap = pg_nextpower2_32(Max(n * 2, 16));

	ix->mask = cap - 1;
	ix->key = palloc(sizeof(uint64) * cap);
	ix->newest = palloc0(sizeof(int32) * cap);
	ix->prev = palloc(sizeof(int32) * Max(n, 1));
}

static void
index_add(RowIndex *ix, int rec, uint64 key)
{
	uint32		p = (uint32) ((key * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & ix->mask;

	while (ix->newest[p] != 0 && ix->key[p] != key)
		p = (p + 1) & ix->mask;
	ix->key[p] = key;
	ix->prev[rec] = ix->newest[p] - 1;
	ix->newest[p] = rec + 1;
}

static int
index_newest(const RowIndex *ix, uint64 key)
{
	uint32		p = (uint32) ((key * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & ix->mask;

	while (ix->newest[p] != 0 && ix->key[p] != key)
		p = (p + 1) & ix->mask;
	return ix->newest[p] - 1;
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
chain_invalidate(Datum arg pg_attribute_unused(), Oid relid)
{
	for (int i = 0; i < CHAIN_SLOTS; i++)
		if (slots[i].cx != NULL && (!OidIsValid(relid) || slots[i].relid == relid))
			slot_drop(&slots[i]);
}

void
sift_chain_cache_forget_rel(Oid relid)
{
	chain_invalidate((Datum) 0, relid);
}

static void
chain_init(void)
{
	chain_cx = sift_cache_create_context("sift cache f2 table chains");
	sift_cache_register(&chain_kind);
	CacheRegisterRelcacheCallback(chain_invalidate, (Datum) 0);
}

static Slot *
slot_find(Relation rel, bool pending, uint64 part_id)
{
	for (int i = 0; i < CHAIN_SLOTS; i++)
		if (slots[i].cx != NULL && slots[i].pending == pending && slots[i].part_id == part_id && RelFileLocatorEquals(slots[i].loc, rel->rd_locator))
			return &slots[i];
	return NULL;
}

/* A free slot, else the least recently used one, emptied */
static Slot *
slot_take(void)
{
	Slot	   *lru = &slots[0];

	for (int i = 0; i < CHAIN_SLOTS; i++)
	{
		if (slots[i].cx == NULL)
			return &slots[i];
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
	s->part_id = part_id;
	return s;
}

static PageStamp
stamp_of_page(const char *page, BlockNumber blk)
{
	return (PageStamp) {blk, SiftPageGetOpaque((Page) page)->gen, PageGetLSN((Page) page)};
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
	return a->blk == b->blk && a->gen == b->gen && a->lsn == b->lsn;
}

/* ---- the pending deletes ---- */

static uint64
pd_key(const SiftPendDelRec *r)
{
	return r->tid & SIFT_PD_TID_MASK;
}

/* The records, read twice around the stamp of the last page: when the two reads agree nothing was appended between them, so the stamp is the one of the records */
static void
pd_load(Relation rel, const SiftPartitionEntry *entry, Slot *s)
{
	SiftPendDelLog *l = &s->log.pd;
	BlockNumber *pages;
	int			npages,
				n2;
	SiftPendDelRec *again;

	for (;;)
	{
		l->recs = sift_pend_del_read(rel, entry->part_id, entry->pend_del_blk, &l->n, &pages, &npages);
		s->stamp = stamp_read(rel, pages[npages - 1]);
		again = sift_pend_del_read(rel, entry->part_id, entry->pend_del_blk, &n2, NULL, NULL);
		pfree(pages);
		pfree(again);
		if (n2 == l->n)
			break;
		pfree(l->recs);
	}
	index_init(&l->ix, l->n);
	for (int i = 0; i < l->n; i++)
		if ((l->recs[i].tid & SIFT_PD_TID_SUCC) == 0)
			index_add(&l->ix, i, pd_key(&l->recs[i]));
		else
			l->ix.prev[i] = -1;
}

const SiftPendDelLog *
sift_pend_del_log(Relation rel, const SiftPartitionEntry *entry)
{
	Slot	   *s;
	MemoryContext old;

	if (entry->pend_del_blk == InvalidBlockNumber)
		return NULL;
	if (chain_cx == NULL)
		chain_init();
	s = slot_find(rel, true, entry->part_id);
	if (s != NULL && s->head_blk == entry->pend_del_blk)
	{
		PageStamp now = stamp_read(rel, s->stamp.blk);

		if (stamp_same(&now, &s->stamp))
		{
			s->tick = sift_cache_tick();
			return &s->log.pd;
		}
	}
	if (s != NULL)
		slot_drop(s);
	s = slot_open(rel, true, entry->part_id);
	old = MemoryContextSwitchTo(s->cx);
	pd_load(rel, entry, s);
	MemoryContextSwitchTo(old);
	s->head_blk = entry->pend_del_blk;
	s->tick = sift_cache_tick();
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

static void
cd_index(SiftColDeltaLog *l)
{
	int			cap = 0;

	l->n = 0;
	l->off = NULL;
	for (int pass = 0; pass < 2; pass++)
	{
		int			n = 0;

		for (int i = 0; i < l->chain.npages; i++)
		{
			const char *page = sift_col_delta_chain_page(&l->chain, i);
			const uint8 *p = sift_col_delta_page_first(page),
					   *end = sift_col_delta_page_end(page);

			while (p < end)
			{
				SiftColDeltaEntryHdr h;

				memcpy(&h, p, sizeof(h));
				if (pass == 1)
				{
					l->off[n] = (uint32) (p - (const uint8 *) l->chain.arena);
					index_add(&l->ix, n, sift_col_delta_hdr_tid(&h));
				}
				n++;
				p += h.rec_bytes;
			}
		}
		if (pass == 0)
		{
			cap = n;
			l->off = palloc(sizeof(uint32) * Max(cap, 1));
			index_init(&l->ix, cap);
		}
	}
	l->n = cap;
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
	if (s != NULL && s->head_blk == entry->manifest_blk && s->manifest_gen == entry->manifest_gen)
	{
		PageStamp now = stamp_read(rel, s->stamp.blk);

		if (stamp_same(&now, &s->stamp))
		{
			s->tick = sift_cache_tick();
			return &s->log.cd;
		}
	}
	if (s != NULL)
		slot_drop(s);
	s = slot_open(rel, false, entry->part_id);
	old = MemoryContextSwitchTo(s->cx);
	has = sift_col_delta_load_chain(rel, entry, 0, &s->log.cd.chain);
	if (has)
	{
		const char *head = sift_col_delta_chain_page(&s->log.cd.chain, s->log.cd.chain.npages - 1);

		s->stamp = stamp_of_page(head, s->log.cd.chain.blks[s->log.cd.chain.npages - 1]);
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

const uint8 *
sift_col_delta_log_rec(const SiftColDeltaLog *l, int i)
{
	return (const uint8 *) l->chain.arena + l->off[i];
}
