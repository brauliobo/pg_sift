/*
 * bm25/deadkeys.c - the docs of immutable segments that a heap check found dead to every snapshot (bm25/deadkeys.h).
 */
#include "core/host.h"

#include "bm25/deadkeys.h"
#include "core/cachebudget.h"

typedef struct DeadKey
{
	RelFileLocator rel;
	uint32		unused;			/* the key is hashed as bytes: no padding */
	uint64		gen;
	uint64		blk;			/* the block of SIFT_BLK_PAGES pages of the segment */
} DeadKey;

typedef struct DeadEnt
{
	DeadKey		key;			/* first: the hash key */
	uint64		off[SIFT_BLK_PAGES][SIFT_OFFMASK_WORDS];
} DeadEnt;

static HTAB *tab;
static MemoryContext tab_cx;	/* holds tab: hash_destroy leaves a HASH_CONTEXT behind */
static bool registered;
static uint64 used;				/* sift_cache_tick() of the last doc kept */

static uint64
deadkeys_oldest(void)
{
	return tab && hash_get_num_entries(tab) > 0 ? used : UINT64_MAX;
}

static bool
deadkeys_evict_one(void)
{
	bool		had = tab != NULL;

	if (had)
		MemoryContextDelete(tab_cx);
	tab = NULL;
	tab_cx = NULL;
	return had;
}

static void
deadkeys_evict_all(void)
{
	deadkeys_evict_one();
}

static const SiftCacheKind deadkeys_kind = {"bm25_dead_docs", deadkeys_oldest, deadkeys_evict_one, deadkeys_evict_all};

static void
deadkey(DeadKey *k, const SiftSegment *seg, uint64 blk)
{
	*k = (DeadKey) {.rel = seg->index->rd_locator,.gen = seg->entry.gen,.blk = blk};
}

void
sift_deadkeys_note(const SiftSegment *seg, SiftKey key)
{
	BlockNumber page = sift_key_block(key);
	int			off = (key & 0xFFFF) - 1;
	DeadKey		k;
	DeadEnt    *e;
	bool		found;

	if (sift_backend_cache_kb <= 0)
		return;
	deadkey(&k, seg, page / SIFT_BLK_PAGES);
	sift_host_cache_lock();
	if (sift_cache_room(sizeof(DeadEnt)))
	{
		if (tab == NULL)
		{
			HASHCTL		ctl = {.keysize = sizeof(DeadKey),.entrysize = sizeof(DeadEnt)};

			ctl.hcxt = tab_cx = sift_cache_create_context("sift cache dead docs");
			tab = hash_create("sift dead docs", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
			if (!registered)
				sift_cache_register(&deadkeys_kind);
			registered = true;
		}
		e = hash_search(tab, &k, HASH_ENTER, &found);
		if (!found)
			memset(e->off, 0, sizeof(e->off));
		e->off[page % SIFT_BLK_PAGES][off / 64] |= UINT64CONST(1) << (off % 64);
		used = sift_cache_tick();
	}
	sift_host_cache_unlock();
}

void
sift_deadkeys_clear(const SiftSegment *seg, uint32 sb, int b, uint64 (*res)[SIFT_OFFMASK_WORDS])
{
	DeadKey		k;
	DeadEnt    *e;

	if (tab == NULL || hash_get_num_entries(tab) == 0)
		return;
	deadkey(&k, seg, ((uint64) sb * SIFT_SB_PAGES) / SIFT_BLK_PAGES + b);
	sift_host_cache_lock();
	if ((e = hash_search(tab, &k, HASH_FIND, NULL)) != NULL)
		for (int pg = 0; pg < SIFT_BLK_PAGES; pg++)
			for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
				res[pg][w] &= ~e->off[pg][w];
	sift_host_cache_unlock();
}

void
sift_deadkeys_drop_file(const RelFileLocator *loc)
{
	HASH_SEQ_STATUS seq;
	DeadEnt    *e;

	if (tab == NULL)
		return;
	sift_host_cache_lock();
	hash_seq_init(&seq, tab);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (memcmp(&e->key.rel, loc, sizeof(*loc)) == 0)
			hash_search(tab, &e->key, HASH_REMOVE, NULL);
	sift_host_cache_unlock();
}
