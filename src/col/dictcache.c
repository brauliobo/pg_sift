/*
 * col/dictcache.c - the parsed dictionaries of immutable column segments, kept by the backend for the next query.
 *
 * A string leaf's dictionary is one compressed blob: a scan that reads the leaf inflates and indexes all of it, however few of its
 * codes the selected rows hold (60,000 entries of a free text path: a tenth of a millisecond per thousand entries, on every query).
 * A segment never changes, so its generation names the dictionary for good: it is kept, under the relation file, generation, header
 * block, rows and leaf, up to CDICT_CACHE_BYTES per backend. A segment pins the entries it uses until its memory context goes (a
 * callback), and eviction only takes unpinned ones, least recently used first. A dictionary that would not fit is parsed into the
 * memory of the scan, as before. Like the docmap directories of bm25/docdir.c.
 */
#include "postgres.h"

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
	uint64		used;
} DictEnt;

typedef struct Unpin
{
	MemoryContextCallback cb;
	DictKey		key;
} Unpin;

static HTAB *cache;
static MemoryContext cache_cx;
static Size cache_bytes;
static uint64 cache_clock;

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
		HASH_SEQ_STATUS seq;
		DictEnt    *e,
				   *lru = NULL;

		hash_seq_init(&seq, cache);
		while ((e = hash_seq_search(&seq)) != NULL)
			if (e->pins == 0 && (lru == NULL || e->used < lru->used))
				lru = e;
		if (lru == NULL)
			return false;
		dict_drop(lru);
	}
	return true;
}

static void
dict_pin(DictEnt *e, MemoryContext mcxt)
{
	Unpin	   *u = MemoryContextAlloc(mcxt, sizeof(Unpin));

	e->pins++;
	e->used = ++cache_clock;
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
	if (size > CDICT_CACHE_BYTES / 4 || !dict_room(size))
		return d;
	MemoryContextSetParent(cx, cache_cx);
	e = hash_search(cache, &k, HASH_ENTER, NULL);
	e->cx = cx;
	e->obj = d;
	e->size = size;
	e->pins = 0;
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
sift_cseg_lens_keep(SiftColSegment *seg, uint32 unit, int col, uint32 n, const uint32 *counts, const uint32 *eoff)
{
	DictKey		k;
	DictEnt    *e;
	MemoryContext cx;
	SiftCLens  *l;
	Size		size = sizeof(SiftCLens) + (SIFT_CC_CAP32(n) + 1 + SIFT_CC_CAP32(n + 1)) * sizeof(uint32);

	if (cache == NULL)
		cache_init();
	if (size > CDICT_CACHE_BYTES / 4 || !dict_room(size))
		return;
	dict_key(&k, seg, col, unit);
	cx = AllocSetContextCreate(cache_cx, "sift cache segment lengths", ALLOCSET_SMALL_SIZES);
	l = MemoryContextAllocZero(cx, size);
	l->counts = (uint32 *) (l + 1) + 1;		/* the slot before the counts is the one the running ends are summed from */
	l->eoff = l->counts + SIFT_CC_CAP32(n);
	l->nelems = eoff[n];
	memcpy(l->counts, counts, n * sizeof(uint32));
	memcpy(l->eoff, eoff, (n + 1) * sizeof(uint32));
	CDICT_STAT(loads);
	e = hash_search(cache, &k, HASH_ENTER, NULL);
	e->cx = cx;
	e->obj = l;
	e->size = size;
	e->pins = 0;
	e->used = ++cache_clock;
	cache_bytes += size;
}
