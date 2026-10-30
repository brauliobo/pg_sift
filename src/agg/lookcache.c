/*
 * agg/lookcache.c - the rows of a joined table, kept by the backend for the next query.
 *
 * A lookup join loads the rows it can match into a hash table before the scan (agg/lookup/): a mapping table of half a million rows is
 * a second of every query that joins it. The rows are kept under the conditions that narrowed the load, up to sift.agg_lookup_cache_kb
 * per backend, and serve the next query when the table is what it was. A page that every transaction sees (PD_ALL_VISIBLE) holds the
 * same rows for every snapshot, and a write to it moves its LSN: so an entry keeps the LSN of every page of the table, taken before the
 * load and again after it, and serves a query that finds the same file of the same size with the same LSNs, all pages visible. A table
 * with a page that is not (rows not vacuumed yet) is not kept. A scan pins the entries it uses until its memory context goes (a
 * callback), and eviction only takes unpinned ones, least recently used first. Like the dictionaries of col/dictcache.c. The rows go to the
 * shared memory of all backends too (sift.agg_lookup_shared_kb, agg/lookshm.c), under the same key and pages, and a query that finds none of its
 * own reads them there.
 */
#include "postgres.h"

#include "access/parallel.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/agg_int.h"

int			sift_agg_lookup_cache_kb = 256 * 1024;
#define LOOKCACHE_BYTES		((Size) sift_agg_lookup_cache_kb * 1024)
/* what the pages of a table are weighed against: the larger of the caches that are on */
#define LOOKCACHE_BUDGET	Max(LOOKCACHE_BYTES, lookshm_bytes())
/* the pages of a table are fingerprinted at about 0.4 microseconds each, a twentieth of what loading their rows costs: what bounds a table is the memory of its rows, and the fingerprint itself may take half the budget */
#define LOOKCACHE_MAX_BLOCKS	(Min(LOOKCACHE_BUDGET / 2, MaxAllocSize) / sizeof(XLogRecPtr))
/* tables whose rows did not fit, by key: they are not fingerprinted again while the file is no smaller and the budget no larger */
#define LOOKCACHE_REFUSED		16

typedef struct LookRefused
{
	char	   *key;
	RelFileLocator rel;
	BlockNumber nblocks;
	Size		budget;
} LookRefused;

typedef struct LookEnt
{
	char	   *key;
	LookFinger *finger;
	MemoryContext cx;
	MapTab	   *map;
	LookCols	cols;			/* the columns of the rows: a query that reads others loads the union and the entry is replaced */
	Size		size;
	int			pins;
	uint64		used;
} LookEnt;

typedef struct LookPin
{
	MemoryContextCallback cb;
	LookEnt    *ent;
} LookPin;

static LookRefused refused[LOOKCACHE_REFUSED];
static int	nrefused;
static List *entries;
static MemoryContext cache_cx;
static Size cache_bytes;
static uint64 cache_clock;

/* does the heap page hold rows every snapshot sees? (a page nobody wrote to has none) */
static bool
page_seen(Page page)
{
	return PageIsNew(page) || PageIsAllVisible(page);
}

/* the LSNs of the pages of a table whose pages all show their rows to every transaction; NULL for a table the cache cannot vouch for */
LookFinger *
lookcache_finger(Relation rel)
{
	LookFinger *f;
	BufferAccessStrategy bas;
	bool		seen = true;

	if (LOOKCACHE_BUDGET == 0 || IsParallelWorker() || rel->rd_tableam != GetHeapamTableAmRoutine() || !RelationNeedsWAL(rel) ||
		rel->rd_createSubid != InvalidSubTransactionId || rel->rd_firstRelfilelocatorSubid != InvalidSubTransactionId)
		return NULL;
	f = palloc(sizeof(LookFinger));
	f->rel = rel->rd_locator;
	f->nblocks = RelationGetNumberOfBlocks(rel);
	if (f->nblocks > LOOKCACHE_MAX_BLOCKS)
		return NULL;
	f->lsn = palloc(Max(f->nblocks, 1) * sizeof(XLogRecPtr));
	bas = GetAccessStrategy(BAS_BULKREAD);
	for (BlockNumber b = 0; b < f->nblocks && seen; b++)
	{
		Buffer		buf = ReadBufferExtended(rel, MAIN_FORKNUM, b, RBM_NORMAL, bas);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		seen = page_seen(BufferGetPage(buf));
		f->lsn[b] = PageGetLSN(BufferGetPage(buf));
		UnlockReleaseBuffer(buf);
	}
	FreeAccessStrategy(bas);
	return seen ? f : NULL;
}

/* did the rows of this table under key not fit the cache the last time they were loaded, and is the table no smaller? */
bool
lookcache_refused(const char *key, Relation rel)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);

	for (int i = 0; i < nrefused; i++)
		if (strcmp(refused[i].key, key) == 0 && RelFileLocatorEquals(refused[i].rel, rel->rd_locator))
			return nblocks >= refused[i].nblocks && LOOKCACHE_BUDGET <= refused[i].budget;
	return false;
}

static void
look_refuse(const char *key, const LookFinger *f)
{
	LookRefused *r = NULL;
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

	for (int i = 0; i < nrefused; i++)
		if (strcmp(refused[i].key, key) == 0)
			r = &refused[i];
	if (r == NULL && nrefused < LOOKCACHE_REFUSED)
		r = &refused[nrefused++];
	if (r == NULL)
	{
		/* the oldest goes: the slots are used in turn */
		static int	next;

		r = &refused[next++ % LOOKCACHE_REFUSED];
	}
	if (r->key)
		pfree(r->key);
	r->key = pstrdup(key);
	r->rel = f->rel;
	r->nblocks = f->nblocks;
	r->budget = LOOKCACHE_BUDGET;
	MemoryContextSwitchTo(old);
}

static bool
finger_same(const LookFinger *a, const LookFinger *b)
{
	return RelFileLocatorEquals(a->rel, b->rel) && a->nblocks == b->nblocks && memcmp(a->lsn, b->lsn, a->nblocks * sizeof(XLogRecPtr)) == 0;
}

static void
look_unpin(void *arg)
{
	((LookPin *) arg)->ent->pins--;
}

static void
look_pin(LookEnt *e, MemoryContext mcxt)
{
	LookPin    *p = MemoryContextAlloc(mcxt, sizeof(LookPin));

	e->pins++;
	e->used = ++cache_clock;
	p->cb = (MemoryContextCallback) {look_unpin, p, NULL};
	p->ent = e;
	MemoryContextRegisterResetCallback(mcxt, &p->cb);
}

static void
look_drop(LookEnt *e)
{
	cache_bytes -= e->size;
	entries = list_delete_ptr(entries, e);
	MemoryContextDelete(e->cx);
}

static LookEnt *
look_unpinned(const char *key)
{
	foreach_ptr(LookEnt, e, entries)
		if (e->pins == 0 && strcmp(e->key, key) == 0)
			return e;
	return NULL;
}

/* Drops the least recently used entries nobody pins until need more bytes fit; false when they do not */
static bool
look_room(Size need)
{
	while (cache_bytes + need > LOOKCACHE_BYTES)
	{
		LookEnt    *lru = NULL;

		foreach_ptr(LookEnt, e, entries)
			if (e->pins == 0 && (lru == NULL || e->used < lru->used))
				lru = e;
		if (lru == NULL)
			return false;
		look_drop(lru);
	}
	return true;
}

bool
cols_cover(const LookCols *have, const LookCols *need)
{
	for (int i = 0; i < need->n; i++)
	{
		int			k;

		for (k = 0; k < have->n; k++)
			if (have->att[k] == need->att[i] && have->type[k] == need->type[i])
				break;
		if (k == have->n)
			return false;
	}
	return true;
}

static LookEnt *
look_unpinned_other(const char *key, const LookEnt *keep)
{
	foreach_ptr(LookEnt, e, entries)
		if (e != keep && e->pins == 0 && strcmp(e->key, key) == 0)
			return e;
	return NULL;
}

/*
 * The rows kept under key for a table that still has the pages of f and hold the columns need, pinned by mcxt; NULL when there are none. *have
 * is then the columns the rows were loaded with, or of the entry of the table that lacks some of need (the columns to load are those and need).
 */
MapTab *
lookcache_find(const char *key, const LookFinger *f, const LookCols *need, LookCols *have, MemoryContext mcxt)
{
	have->n = 0;
	foreach_ptr(LookEnt, e, entries)
		if (strcmp(e->key, key) == 0 && finger_same(e->finger, f))
		{
			LookEnt    *other;

			if (!cols_cover(&e->cols, need))
			{
				if (e->cols.n > have->n)
					*have = e->cols;
				continue;
			}
			look_pin(e, mcxt);
			*have = e->cols;
			/* the entries of the same rows with fewer columns are not needed any more */
			while ((other = look_unpinned_other(key, e)) != NULL)
				look_drop(other);
			return e->map;
		}
	if (lookshm_bytes() > 0)
	{
		LookCols	shared;
		MapTab	   *m = lookshm_find(key, f, need, &shared, mcxt);

		if (m != NULL || shared.n > have->n)
			*have = shared;
		return m;
	}
	return NULL;
}

/* is there an entry for key, in this backend or in the shared memory? (the question that costs no read of the table's pages, which the finger does) */
bool
lookcache_known(const char *key)
{
	foreach_ptr(LookEnt, e, entries)
		if (strcmp(e->key, key) == 0)
			return true;
	return lookshm_bytes() > 0 && lookshm_known(key);
}

/*
 * Keeps the rows m, loaded into cx between the fingers before and after, for the next query, which owns cx from here. False when they
 * are not kept (the table was written to during the load, or the rows do not fit): cx stays with the caller.
 */
bool
lookcache_keep(const char *key, const LookFinger *before, const LookFinger *after, MapTab *m, const LookCols *cols, MemoryContext cx, MemoryContext mcxt)
{
	LookEnt    *e;
	MemoryContext old;
	Size		size;
	bool		shared_large;

	if (!finger_same(before, after))
		return false;
	lookshm_keep(key, before, m, cols, &shared_large);
	/* the rows of an older state of the table go; dropping changes the list, so it is searched again */
	while ((e = look_unpinned(key)) != NULL)
		look_drop(e);
	size = MemoryContextMemAllocated(cx, true) + (Size) before->nblocks * sizeof(XLogRecPtr);
	if (size > LOOKCACHE_BYTES / 2)
	{
		/* the table is refused when no cache that is on could take its rows */
		if (shared_large)
			look_refuse(key, before);
		return false;
	}
	if (!look_room(size))
		return false;
	if (cache_cx == NULL)
		cache_cx = AllocSetContextCreate(TopMemoryContext, "sift cache lookup tables", ALLOCSET_DEFAULT_SIZES);
	e = MemoryContextAllocZero(cx, sizeof(LookEnt));
	e->key = MemoryContextStrdup(cx, key);
	e->finger = MemoryContextAlloc(cx, sizeof(LookFinger));
	*e->finger = *before;
	e->finger->lsn = MemoryContextAlloc(cx, before->nblocks * sizeof(XLogRecPtr));
	memcpy(e->finger->lsn, before->lsn, before->nblocks * sizeof(XLogRecPtr));
	e->cx = cx;
	e->map = m;
	e->cols = *cols;
	e->size = size;
	old = MemoryContextSwitchTo(TopMemoryContext);
	entries = lappend(entries, e);
	MemoryContextSwitchTo(old);
	/* the rows fit now: the table is not refused any more */
	for (int i = 0; i < nrefused; i++)
		if (strcmp(refused[i].key, key) == 0)
		{
			pfree(refused[i].key);
			refused[i] = refused[--nrefused];
			refused[nrefused].key = NULL;
			break;
		}
	/* nothing below fails: the entry owns cx from here */
	MemoryContextSetParent(cx, cache_cx);
	cache_bytes += size;
	look_pin(e, mcxt);
	return true;
}
