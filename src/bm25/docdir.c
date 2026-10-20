/*
 * bm25/docdir.c - the SB directory of a segment's docmap, kept by the backend for the next query.
 *
 * The directory is 24 bytes per superblock, and a query that reads docs of a segment reads all of it
 * and sums its ranks (more than 300 kB per segment of a billion-row index, per query). A segment never
 * changes, so its generation names the directory for good: it is kept, under the relation file,
 * generation and header block, up to DOCDIR_CACHE_BYTES per backend. A segment object pins the entry it
 * uses until its memory context goes (a callback), and eviction only takes unpinned, complete ones.
 * Directories under sift_docdir_min_sb superblocks are read each time: that costs less than the lookup.
 * The SQLite port keeps none (sqstubs.c).
 */
#include "postgres.h"

#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "bm25/segment.h"

#define DOCDIR_CACHE_BYTES		(16 * 1024 * 1024)

int			sift_docdir_min_sb = 128;

#ifdef SIFT_TESTING
SiftDocDirStats sift_docdir_stats;
#define DOCDIR_STAT(f)		(sift_docdir_stats.f++)
#else
#define DOCDIR_STAT(f)		((void) 0)
#endif

typedef struct DirKey
{
	RelFileLocator rel;
	uint64		gen;
	BlockNumber header_blk;
	uint32		nsb;			/* read from the segment: a damaged one must not share the entry of an intact size */
} DirKey;

typedef struct DirEnt
{
	DirKey		key;			/* first: the hash key */
	SiftDocSBRef *dir;
	uint64	   *rank_base;
	Size		size;
	int			pins;
	bool		ready;			/* filled by its first user */
	uint64		used;
} DirEnt;

typedef struct Unpin
{
	MemoryContextCallback cb;
	DirKey		key;
} Unpin;

static HTAB *cache;
static MemoryContext cache_cx;
static Size cache_bytes;
static uint64 cache_clock;

static void
dir_drop(DirEnt *e)
{
	cache_bytes -= e->size;
	pfree(e->dir);
	pfree(e->rank_base);
	hash_search(cache, &e->key, HASH_REMOVE, NULL);
}

static void
dir_unpin(void *arg)
{
	Unpin	   *u = arg;
	DirEnt	   *e = hash_search(cache, &u->key, HASH_FIND, NULL);

	if (e && --e->pins == 0 && !e->ready)
		dir_drop(e);
}

/* Makes room for need bytes by dropping the least recently used entries nobody pins */
static void
dir_evict(Size need)
{
	while (cache_bytes + need > DOCDIR_CACHE_BYTES)
	{
		HASH_SEQ_STATUS seq;
		DirEnt	   *e,
				   *lru = NULL;

		hash_seq_init(&seq, cache);
		while ((e = hash_seq_search(&seq)) != NULL)
			if (e->pins == 0 && e->ready && (lru == NULL || e->used < lru->used))
				lru = e;
		if (lru == NULL)
			return;
		dir_drop(lru);
	}
}

bool
sift_docdir_get(SiftSegment *seg, uint32 nsb, SiftDocSBRef **dir, uint64 **rank_base, void **handle)
{
	Size		size = (Size) nsb * (sizeof(SiftDocSBRef) + sizeof(uint64)) + sizeof(uint64);
	DirKey		k;
	DirEnt	   *e;
	bool		found;
	Unpin	   *u;

	memset(&k, 0, sizeof(k));
	k.rel = seg->index->rd_locator;
	k.gen = seg->entry.gen;
	k.header_blk = seg->entry.header_blk;
	k.nsb = nsb;
	*handle = NULL;
	if (nsb < sift_docdir_min_sb || size > DOCDIR_CACHE_BYTES / 4)
	{
		*dir = MemoryContextAllocHuge(seg->mcxt, nsb * sizeof(SiftDocSBRef));
		*rank_base = MemoryContextAllocHuge(seg->mcxt, (nsb + 1) * sizeof(uint64));
		return false;
	}
	if (cache == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(DirKey),.entrysize = sizeof(DirEnt)};

		cache_cx = AllocSetContextCreate(TopMemoryContext, "sift cache docmap", ALLOCSET_DEFAULT_SIZES);
		ctl.hcxt = cache_cx;
		cache = hash_create("sift docmap directories", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	e = hash_search(cache, &k, HASH_FIND, NULL);
	if (e == NULL)
	{
		dir_evict(size);
		e = hash_search(cache, &k, HASH_ENTER, &found);
		e->dir = MemoryContextAllocHuge(cache_cx, nsb * sizeof(SiftDocSBRef));
		e->rank_base = MemoryContextAllocHuge(cache_cx, (nsb + 1) * sizeof(uint64));
		e->size = size;
		e->pins = 0;
		e->ready = false;
		cache_bytes += size;
	}
	e->pins++;
	e->used = ++cache_clock;
	u = MemoryContextAlloc(seg->mcxt, sizeof(Unpin));
	u->cb = (MemoryContextCallback) {dir_unpin, u, NULL};
	u->key = k;
	MemoryContextRegisterResetCallback(seg->mcxt, &u->cb);
	*dir = e->dir;
	*rank_base = e->rank_base;
	*handle = e;
	if (e->ready)
		DOCDIR_STAT(hits);
	else
		DOCDIR_STAT(loads);
	return e->ready;
}

void
sift_docdir_ready(void *handle)
{
	if (handle)
		((DirEnt *) handle)->ready = true;
}
