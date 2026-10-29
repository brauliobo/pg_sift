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

#include "agg/stats.h"
#include "col/am.h"
#include "col/dictshm.h"
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
entry_key(DictKey *k, Relation index, const SiftSegEntry *entry, int col, uint32 unit)
{
	/* the table hashes the bytes of the key, padding included */
	memset(k, 0, sizeof(*k));
	k->rel = index->rd_locator;
	k->gen = entry->gen;
	k->ndocs = entry->ndocs;
	k->header_blk = entry->header_blk;
	k->col = col;
	k->unit = unit;
}

static void
dict_key(DictKey *k, SiftColSegment *seg, int col, uint32 unit)
{
	entry_key(k, seg->index, &seg->entry, col, unit);
}

/* The head of a segment is the entry of leaf -1 */
#define HEAD_COL	(-1)

/*
 * The schema and unit directory of a segment that open checked, found when the header page read again is the one they were made under (the
 * extents, rows and sections are in it, so a segment rewritten in place or a damaged header misses). The pages of the schema and of the
 * directory are not read again.
 */
const SegHead *
sift_cseg_head_find(Relation index, const SiftSegEntry *entry, const SiftSegHeaderData *h, MemoryContext mcxt)
{
	DictKey		k;
	DictEnt    *e;
	const SegHead *hd;

	if (cache == NULL)
		return NULL;
	entry_key(&k, index, entry, HEAD_COL, UINT32_MAX);
	if ((e = hash_search(cache, &k, HASH_FIND, NULL)) == NULL)
		return NULL;
	hd = e->obj;
	if (hd->hdrlen != SEG_HDRLEN(h) || memcmp(hd->hdr, h, hd->hdrlen) != 0)
		return NULL;
	CDICT_STAT(head_hits);
	dict_pin(e, mcxt);
	return hd;
}

/* Keeps obj, made in cx (a child of mcxt: it stays there when the cache has no room), under k, and pins it for mcxt; an entry of k that nobody pins is replaced */
static void
obj_keep(const DictKey *k, MemoryContext cx, void *obj, MemoryContext mcxt)
{
	DictEnt    *e;
	Size		size = MemoryContextMemAllocated(cx, true);

	if (cache == NULL)
		cache_init();
	if ((e = hash_search(cache, k, HASH_FIND, NULL)) != NULL)
	{
		/* the entry of a header that has changed since */
		if (e->pins > 0)
			return;
		dict_drop(e);
	}
	if (size > CDICT_CACHE_BYTES / 2 || !dict_room(size))
		return;
	MemoryContextSetParent(cx, cache_cx);
	e = hash_search(cache, k, HASH_ENTER, NULL);
	e->cx = cx;
	e->obj = obj;
	e->size = size;
	e->pins = 0;
	dlist_push_tail(&lru, &e->node);
	cache_bytes += size;
	dict_pin(e, mcxt);
}

void
sift_cseg_head_keep(Relation index, const SiftSegEntry *entry, MemoryContext cx, SegHead *hd, MemoryContext mcxt)
{
	DictKey		k;

	CDICT_STAT(head_loads);
	entry_key(&k, index, entry, HEAD_COL, UINT32_MAX);
	obj_keep(&k, cx, hd, mcxt);
}

/*
 * The zone arrays of a leaf of a segment (min, max, sums, counts, flags, filters of every unit: the bytes a scan tests whatever few units it reads) are
 * the entry of the leaf at unit UINT32_MAX - 1. They are found without the header: a scan has opened the segment, whose head was checked against it.
 */
#define ZONES_UNIT	(UINT32_MAX - 1)

ZoneArrays *
sift_cseg_zones_find(SiftColSegment *seg, int col)
{
	DictKey		k;
	DictEnt    *e;

	if (cache == NULL)
		return NULL;
	dict_key(&k, seg, col, ZONES_UNIT);
	if ((e = hash_search(cache, &k, HASH_FIND, NULL)) == NULL)
		return NULL;
	CDICT_STAT(zone_hits);
	dict_pin(e, seg->mcxt);
	return e->obj;
}

void
sift_cseg_zones_keep(SiftColSegment *seg, int col, MemoryContext cx, ZoneArrays *z)
{
	DictKey		k;

	CDICT_STAT(zone_loads);
	dict_key(&k, seg, col, ZONES_UNIT);
	obj_keep(&k, cx, z, seg->mcxt);
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
	uint64		spent;			/* instructions the checks of strings found by the hash of a dictionary in the stream cost (sift_cseg_dict_spend) */
	bool		nokeep;			/* the parsed dictionary did not fit the cache: parsing it again for every query would cost more than all the searches of the stream */
} AskEnt;

static HTAB *asks;

static AskEnt *
ask_ent(SiftColSegment *seg, int col)
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
		return ask_ent(seg, col);
	}
	dict_key(&k, seg, col, UINT32_MAX);
	e = hash_search(asks, &k, HASH_ENTER, &found);
	if (!found)
	{
		e->asks = 0;
		e->spent = 0;
		e->nokeep = false;
	}
	return e;
}

uint32
sift_cseg_dict_asked(SiftColSegment *seg, int col)
{
	return ++ask_ent(seg, col)->asks;
}

uint64
sift_cseg_dict_spent(SiftColSegment *seg, int col, uint64 add)
{
	AskEnt	   *e = ask_ent(seg, col);

	e->spent += add;
	return e->spent;
}

bool
sift_cseg_dict_nokeep(SiftColSegment *seg, int col)
{
	return ask_ent(seg, col)->nokeep;
}

Size
sift_cdict_cache_budget(void)
{
	return CDICT_CACHE_BYTES;
}

Size
sift_cdict_parsed_bytes(Size nbytes, uint32 n)
{
	return nbytes + (Size) n * (sizeof(uint32) + CDICT_SLOT_BYTES);
}

void
sift_cseg_dict_heavy(SiftColSegment *seg, int col, uint64 spent)
{
	AskEnt	   *e = ask_ent(seg, col);

	e->asks = Max(e->asks, DICT_PARSE_AFTER);
	e->spent = Max(e->spent, spent);
}

/* Keeps d, made in cx (a child of the scan's context), under k as the cache has room for it, and pins it for the scan */
static SiftCDict *
dict_keep(SiftColSegment *seg, int col, const DictKey *k, MemoryContext cx, SiftCDict *d)
{
	DictEnt    *e;
	Size		size = MemoryContextMemAllocated(cx, true) + (Size) d->n * CDICT_SLOT_BYTES;

	if (size > CDICT_CACHE_BYTES / 2 || !dict_room(size))
	{
		ask_ent(seg, col)->nokeep = true;
		return d;
	}
	MemoryContextSetParent(cx, cache_cx);
	e = hash_search(cache, k, HASH_ENTER, NULL);
	e->cx = cx;
	e->obj = d;
	e->size = size;
	e->pins = 0;
	dlist_push_tail(&lru, &e->node);
	cache_bytes += size;
	dict_pin(e, seg->mcxt);
	return d;
}

/* The hash table or the sorted order of a dictionary read in place has just been made: the backends that read it next find them in the ring (a record that has them already is left alone) */
static void
dict_built(SiftCDict *d, void *arg)
{
	sift_dictshm_write(arg, d);
}

/* d, read in place from a record that lacks its lookup structures, shares them when it makes them */
static SiftCDict *
dict_hook(SiftCDict *d, const DictKey *k)
{
	if (d != NULL && (d->slots == NULL || d->sorted == NULL))
	{
		d->built_arg = MemoryContextAlloc(d->mcxt, sizeof(*k));
		memcpy(d->built_arg, k, sizeof(*k));
		d->built = dict_built;
	}
	return d;
}

static SiftCDict *
dict_shared(SiftCDict *d, const DictKey *k)
{
	if (d != NULL)
		sift_stat_add(SIFT_STAT_DICT_SHARED_HITS, 1);
	return dict_hook(d, k);
}

static MemoryContext
dict_cx(SiftColSegment *seg)
{
	return AllocSetContextCreate(seg->mcxt, "sift cache segment dictionary", ALLOCSET_DEFAULT_SIZES);
}

/* The dictionary in the cache, else the one another backend parsed, read in the shared memory it lies in: it was found worth its parse, and reading it costs a lookup */
SiftCDict *
sift_cseg_dict_peek(SiftColSegment *seg, int col)
{
	DictKey		k;
	DictEnt    *e;

	dict_key(&k, seg, col, UINT32_MAX);
	if (cache != NULL && (e = hash_search(cache, &k, HASH_FIND, NULL)) != NULL)
	{
		CDICT_STAT(hits);
		dict_pin(e, seg->mcxt);
		return e->obj;
	}
	return dict_shared(sift_dictshm_read(&k, seg->mcxt), &k);
}

SiftCDict *
sift_cseg_dict_cached(SiftColSegment *seg, int col, SiftCDictLoad load)
{
	DictKey		k;
	DictEnt    *e;
	MemoryContext cx;
	SiftCDict  *d,
			   *shared;

	dict_key(&k, seg, col, UINT32_MAX);
	if (cache == NULL)
		cache_init();
	if ((e = hash_search(cache, &k, HASH_FIND, NULL)) != NULL)
	{
		CDICT_STAT(hits);
		dict_pin(e, seg->mcxt);
		return e->obj;
	}
	if ((d = sift_dictshm_read(&k, seg->mcxt)) != NULL)
		return dict_shared(d, &k);
	/* under the scan until it is kept: a dictionary that does not parse leaves nothing behind */
	cx = dict_cx(seg);
	d = load(seg, col, cx);
	/* counted here, not where it is kept: a dictionary that goes to the ring is read in place and never kept */
	CDICT_STAT(loads);
	/* the copy in the ring is the one the backends read, this one included */
	if (sift_dictshm_write(&k, d) && (shared = sift_dictshm_read(&k, seg->mcxt)) != NULL)
	{
		MemoryContextDelete(cx);
		return dict_hook(shared, &k);
	}
	return dict_keep(seg, col, &k, cx, d);
}

#define BISECT_UNIT		(UINT32_MAX - 2)

SiftCBisect *
sift_cseg_bisect_find(SiftColSegment *seg, int col)
{
	DictKey		k;
	DictEnt    *e;

	if (cache == NULL)
		return NULL;
	dict_key(&k, seg, col, BISECT_UNIT);
	if ((e = hash_search(cache, &k, HASH_FIND, NULL)) == NULL)
		return NULL;
	dict_pin(e, seg->mcxt);
	return e->obj;
}

SiftCBisect *
sift_cseg_bisect(SiftColSegment *seg, int col, uint32 n)
{
	DictKey		k;
	DictEnt    *e;
	MemoryContext cx;
	SiftCBisect *b;
	uint32		slots = 256;

	dict_key(&k, seg, col, BISECT_UNIT);
	if (cache == NULL)
		cache_init();
	if ((e = hash_search(cache, &k, HASH_FIND, NULL)) != NULL)
	{
		dict_pin(e, seg->mcxt);
		return e->obj;
	}
	while (slots < BISECT_SLOTS_MAX && slots < n / 4)
		slots *= 2;
	cx = AllocSetContextCreate(seg->mcxt, "sift cache segment bisection", ALLOCSET_SMALL_SIZES);
	b = MemoryContextAllocZero(cx, offsetof(SiftCBisect, slot) + (Size) slots * sizeof(BisectSlot));
	b->mask = slots - 1;
	obj_keep(&k, cx, b, seg->mcxt);
	return b;
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

/* a unit's page mask and run head are kept under the unit number with this bit: the lengths of the unit are under the number alone */
#define SBHEAD_UNIT		0x80000000u

bool
sift_cseg_sbhead(SiftColSegment *seg, uint32 u, int col, SiftCSbHead *out)
{
	DictKey		k;
	DictEnt    *e;
	MemoryContext cx;
	SiftCRun	r;
	uint64		at;

	dict_key(&k, seg, col, u | SBHEAD_UNIT);
	if (cache != NULL && (e = hash_search(cache, &k, HASH_FIND, NULL)) != NULL)
	{
		dlist_move_tail(&lru, &e->node);
		*out = *(SiftCSbHead *) e->obj;
		return true;
	}
	memset(out, 0, sizeof(*out));
	if (!sift_cseg_unit_pagemask(seg, u, out->pm))
		return false;
	if (sift_cseg_run(seg, col, u, &r, &at) && r.heads > 0)
	{
		out->has = true;
		sift_crun_head_counts(r.heads, r.n, &out->nlo, &out->nhi);
		sift_cseg_run_heads(seg, at, &r, out->ent);
	}
	if (cache == NULL)
		cache_init();
	if (!dict_room(sizeof(*out)))
		return true;
	cx = AllocSetContextCreate(cache_cx, "sift cache segment run head", ALLOCSET_SMALL_SIZES);
	e = hash_search(cache, &k, HASH_ENTER, NULL);
	e->cx = cx;
	e->obj = MemoryContextAlloc(cx, sizeof(*out));
	memcpy(e->obj, out, sizeof(*out));
	e->size = sizeof(*out);
	e->pins = 0;
	dlist_push_tail(&lru, &e->node);
	cache_bytes += sizeof(*out);
	return true;
}
