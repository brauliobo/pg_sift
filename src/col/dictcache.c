/*
 * col/dictcache.c - the parsed dictionaries of immutable column segments, kept by the backend for the next query.
 *
 * A string leaf's dictionary is one compressed blob: a scan that reads the leaf inflates and indexes all of it, however few of its
 * codes the selected rows hold (60,000 entries of a free text path: a tenth of a millisecond per thousand entries, on every query).
 * A segment never changes, so its generation names the dictionary for good: it is kept, under the relation file, generation, header
 * block, rows and leaf, up to CDICT_CACHE_BYTES per backend. A segment pins the entries it uses until its memory context goes (a
 * callback), and eviction only takes unpinned ones, least recently used first. A dictionary that would not fit is parsed into the
 * memory of the scan, as before. The limit of one entry is half of the budget: the dictionary of a free text leaf of 150,000 rows is 18 MB, and a
 * backend that inflates it again for every statement spends most of its time there. Like the docmap directories of bm25/docdir.c.
 */
#include "postgres.h"

#include "lib/ilist.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/am.h"
#include "core/limits.h"
#include "col/seg_int.h"

#define CDICT_CACHE_MAX_KB		65536
#define CDICT_CACHE_BYTES		((Size) sift_limit(dict_cache_kb, CDICT_CACHE_MAX_KB) * 1024)
/* what a lookup adds to a code, built lazily once the dictionary has been searched a few times: the open addressing table (up to 4 slots of 4 bytes) and the sorted order (4) */
#define CDICT_SLOT_BYTES		20

#ifdef SIFT_TESTING
SiftCDictCacheStats sift_cdict_cache_stats;
#define CDICT_STAT(f)		(sift_cdict_cache_stats.f++)
#else
#define CDICT_STAT(f)		((void) 0)
#endif

typedef struct DictKey
{
	RelFileLocator rel;
	uint64		gen;
	uint64		ndocs;			/* read from the segment: a damaged one must not share the entry of an intact one */
	BlockNumber header_blk;
	int32		col;
	uint32		unit;			/* UINT32_MAX: the dictionary of the leaf, else the list lengths of that unit */
} DictKey;

typedef struct DictEnt
{
	DictKey		key;			/* first: the hash key */
	MemoryContext cx;
	void	   *obj;			/* a SiftCDict, or a SiftCLens */
	Size		size;
	int			pins;
	dlist_node	node;			/* in lru: least recently used first */
} DictEnt;

typedef struct Unpin
{
	MemoryContextCallback cb;
	DictKey		key;
} Unpin;

static HTAB *cache;
static MemoryContext cache_cx;
static Size cache_bytes;
static dlist_head lru = DLIST_STATIC_INIT(lru);

#ifdef SIFT_TESTING
Size
sift_cdict_cache_bytes(void)
{
	return cache_bytes;
}
#endif

static void
dict_drop(DictEnt *e)
{
	cache_bytes -= e->size;
	dlist_delete(&e->node);
	MemoryContextDelete(e->cx);
	hash_search(cache, &e->key, HASH_REMOVE, NULL);
}

static void
dict_unpin(void *arg)
{
	Unpin	   *u = arg;
	DictEnt    *e = hash_search(cache, &u->key, HASH_FIND, NULL);

	if (e)
		e->pins--;
}

/* Drops the least recently used entries nobody pins until need more bytes fit; false when they do not */
static bool
dict_room(Size need)
{
	while (cache_bytes + need > CDICT_CACHE_BYTES)
	{
		dlist_iter	it;
		DictEnt    *victim = NULL;

		/* a pinned entry was used by the scan that holds it, so it is at the tail: the walk is as long as the entries dropped */
		dlist_foreach(it, &lru)
		{
			DictEnt    *e = dlist_container(DictEnt, node, it.cur);

			if (e->pins == 0)
			{
				victim = e;
				break;
			}
		}
		if (victim == NULL)
			return false;
		dict_drop(victim);
	}
	return true;
}

static void
dict_pin(DictEnt *e, MemoryContext mcxt)
{
	Unpin	   *u = MemoryContextAlloc(mcxt, sizeof(Unpin));

	e->pins++;
	dlist_move_tail(&lru, &e->node);
	u->cb = (MemoryContextCallback) {dict_unpin, u, NULL};
	u->key = e->key;
	MemoryContextRegisterResetCallback(mcxt, &u->cb);
}

static void
cache_init(void)
{
	HASHCTL		ctl = {.keysize = sizeof(DictKey),.entrysize = sizeof(DictEnt)};

	cache_cx = AllocSetContextCreate(TopMemoryContext, "sift cache dictionaries", ALLOCSET_DEFAULT_SIZES);
	ctl.hcxt = cache_cx;
	cache = hash_create("sift column dictionaries", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static void
dict_key(DictKey *k, SiftColSegment *seg, int col, uint32 unit)
{
	/* the table hashes the bytes of the key, padding included */
	memset(k, 0, sizeof(*k));
	k->rel = seg->index->rd_locator;
	k->gen = seg->entry.gen;
	k->ndocs = seg->entry.ndocs;
	k->header_blk = seg->entry.header_blk;
	k->col = col;
	k->unit = unit;
}

/*
 * How many tests the backend made of a dictionary it searched in the stream, over all its queries: a dictionary asked often is parsed and kept (the
 * parse costs about as much as DICT_PARSE_AFTER searches of the stream, and the cache then answers an equality in a hash probe). The counters of
 * 1,024 dictionaries are kept; the table is emptied when it is full.
 */
#define DICT_ASKS_KEPT	1024

typedef struct AskEnt
{
	DictKey		key;
	uint32		asks;
} AskEnt;

static HTAB *asks;

uint32
sift_cseg_dict_asked(SiftColSegment *seg, int col)
{
	DictKey		k;
	AskEnt	   *e;
	bool		found;

	if (asks == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(DictKey),.entrysize = sizeof(AskEnt)};

		if (cache == NULL)
			cache_init();
		ctl.hcxt = cache_cx;
		asks = hash_create("sift column dictionary asks", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	else if (hash_get_num_entries(asks) >= DICT_ASKS_KEPT)
	{
		hash_destroy(asks);
		asks = NULL;
		return sift_cseg_dict_asked(seg, col);
	}
	dict_key(&k, seg, col, UINT32_MAX);
	e = hash_search(asks, &k, HASH_ENTER, &found);
	if (!found)
		e->asks = 0;
	return ++e->asks;
}

SiftCDict *
sift_cseg_dict_peek(SiftColSegment *seg, int col)
{
	DictKey		k;
	DictEnt    *e;

	if (cache == NULL)
		return NULL;
	dict_key(&k, seg, col, UINT32_MAX);
	if ((e = hash_search(cache, &k, HASH_FIND, NULL)) == NULL)
		return NULL;
	CDICT_STAT(hits);
	dict_pin(e, seg->mcxt);
	return e->obj;
}

SiftCDict *
sift_cseg_dict_cached(SiftColSegment *seg, int col, SiftCDictLoad load)
{
	DictKey		k;
	DictEnt    *e;
	MemoryContext cx;
	SiftCDict  *d;
	Size		size;

	dict_key(&k, seg, col, UINT32_MAX);
	if (cache == NULL)
		cache_init();
	if ((e = hash_search(cache, &k, HASH_FIND, NULL)) != NULL)
	{
		CDICT_STAT(hits);
		dict_pin(e, seg->mcxt);
		return e->obj;
	}
	/* under the scan until it is kept: a dictionary that does not parse leaves nothing behind */
	cx = AllocSetContextCreate(seg->mcxt, "sift cache segment dictionary", ALLOCSET_DEFAULT_SIZES);
	d = load(seg, col, cx);
	size = MemoryContextMemAllocated(cx, true) + (Size) d->n * CDICT_SLOT_BYTES;
	CDICT_STAT(loads);
	if (size > CDICT_CACHE_BYTES / 2 || !dict_room(size))
		return d;
	MemoryContextSetParent(cx, cache_cx);
	e = hash_search(cache, &k, HASH_ENTER, NULL);
	e->cx = cx;
	e->obj = d;
	e->size = size;
	e->pins = 0;
	dlist_push_tail(&lru, &e->node);
	cache_bytes += size;
	dict_pin(e, seg->mcxt);
	return d;
}

const SiftCLens *
sift_cseg_lens_find(SiftColSegment *seg, uint32 unit, int col, MemoryContext arena)
{
	DictKey		k;
	DictEnt    *e;

	if (cache == NULL)
		return NULL;
	dict_key(&k, seg, col, unit);
	if ((e = hash_search(cache, &k, HASH_FIND, NULL)) == NULL)
		return NULL;
	CDICT_STAT(hits);
	dict_pin(e, arena);
	return e->obj;
}

void
sift_cseg_lens_keep(SiftColSegment *seg, uint32 unit, int col, uint32 n, const uint32 *counts, uint32 nelems)
{
	DictKey		k;
	DictEnt    *e;
	MemoryContext cx;
	SiftCLens  *l;
	uint16	   *kept;
	Size		size = sizeof(SiftCLens) + (Size) n * sizeof(uint16);

	if (cache == NULL)
		cache_init();
	if (size > CDICT_CACHE_BYTES / 2 || !dict_room(size))
		return;
	for (uint32 i = 0; i < n; i++)
		if (counts[i] > PG_UINT16_MAX)
			return;
	dict_key(&k, seg, col, unit);
	cx = AllocSetContextCreate(cache_cx, "sift cache segment lengths", ALLOCSET_SMALL_SIZES);
	l = MemoryContextAlloc(cx, size);
	kept = (uint16 *) (l + 1);
	for (uint32 i = 0; i < n; i++)
		kept[i] = (uint16) counts[i];
	l->counts = kept;
	l->nelems = nelems;
	CDICT_STAT(loads);
	e = hash_search(cache, &k, HASH_ENTER, NULL);
	e->cx = cx;
	e->obj = l;
	e->size = size;
	e->pins = 0;
	dlist_push_tail(&lru, &e->node);
	cache_bytes += size;
}
