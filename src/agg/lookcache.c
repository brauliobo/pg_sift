/*
 * agg/lookcache.c - the rows of a joined table, kept by the backend for the next query.
 *
 * A lookup join loads the rows it can match into a hash table before the scan (agg/lookup.c): a mapping table of half a million rows is
 * a second of every query that joins it. The rows are kept under the conditions that narrowed the load, up to sift.agg_lookup_cache_kb
 * per backend, and serve the next query when the table is what it was. A page that every transaction sees (PD_ALL_VISIBLE) holds the
 * same rows for every snapshot, and a write to it moves its LSN: so an entry keeps the LSN of every page of the table, taken before the
 * load and again after it, and serves a query that finds the same file of the same size with the same LSNs, all pages visible. A table
 * with a page that is not (rows not vacuumed yet) is not kept. A scan pins the entries it uses until its memory context goes (a
 * callback), and eviction only takes unpinned ones, least recently used first. Like the dictionaries of col/dictcache.c.
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
/* a table of more pages is not fingerprinted: its rows would not fit the cache anyway */
#define LOOKCACHE_MAX_BLOCKS	(1 << 15)

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

	if (sift_agg_lookup_cache_kb <= 0 || IsParallelWorker() || rel->rd_tableam != GetHeapamTableAmRoutine() || !RelationNeedsWAL(rel) ||
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

static bool
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
	return NULL;
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

	if (!finger_same(before, after))
		return false;
	/* the rows of an older state of the table go; dropping changes the list, so it is searched again */
	while ((e = look_unpinned(key)) != NULL)
		look_drop(e);
	size = MemoryContextMemAllocated(cx, true) + (Size) before->nblocks * sizeof(XLogRecPtr);
	if (size > LOOKCACHE_BYTES / 2 || !look_room(size))
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
	/* nothing below fails: the entry owns cx from here */
	MemoryContextSetParent(cx, cache_cx);
	cache_bytes += size;
	look_pin(e, mcxt);
	return true;
}
