/*
 * agg/lookshm.c - the rows of a joined table, kept in shared memory for the queries of other backends.
 *
 * A backend that has loaded a mapping table writes its rows in the flat form of a parallel scan (agg/lookshare.c) into a dynamic shared memory
 * segment of their own, which is never written again, and lists the segment in a small directory that every backend reaches by name (the DSM
 * registry: nothing is allocated at postmaster start). An entry is the same as one of the cache of a backend (agg/lookcache.c): the key of the
 * load and the LSN of every page of the table, taken before and after the load, with all pages visible to every transaction. A backend that
 * finds the entry with the pages it sees now attaches the segment and reads the rows in place, as a worker does; the segment holds the key and
 * the pages, which are compared in full (the directory only has their hashes). A write moves a page's LSN, so a table that was written to
 * finds no entry, and the entry of the older state goes when its key is kept again. The rows of all entries take up to sift.agg_lookup_shared_kb;
 * the least recently used go first. A segment that is dropped from the directory lives until the backends that read it are done. A crash
 * restart discards the registry and the segments.
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/dsm.h"
#include "storage/dsm_registry.h"
#include "storage/latch.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#if PG_VERSION_NUM >= 180000
#include "utils/wait_classes.h"
#else
#include "utils/wait_event.h"
#endif

#include "agg/agg.h"
#include "agg/agg_int.h"

int			sift_agg_lookup_shared_kb = -1;
#define LOOKSHM_SLOTS		64
/* loads in progress; a backend that finds none free loads without a claim */
#define LOOKSHM_CLAIMS		16
/* a backend that finds its table loaded by another waits this long at most, then loads it itself */
#define LOOKSHM_WAIT_MS		30000
#define LOOKSHM_POLL_MS		5

/* set when the segment of a table could not be created (no room in /dev/shm): the automatic budget is off for the backend */
static bool shm_failed;

/* the budget in bytes: the setting, or when it is -1 the smaller of the cache of a backend and a twentieth of shared_buffers */
Size
lookshm_bytes(void)
{
	if (sift_agg_lookup_shared_kb >= 0)
		return (Size) sift_agg_lookup_shared_kb * 1024;
	if (shm_failed)
		return 0;
	return Min((Size) sift_agg_lookup_cache_kb * 1024, (Size) NBuffers * BLCKSZ / 20);
}

typedef struct LookShmEnt
{
	dsm_handle	h;				/* DSM_HANDLE_INVALID: free */
	uint64		key;			/* hash of the database and the key of the load */
	uint64		pages;			/* hash of what the pages of the table were */
	LookCols	cols;
	Size		size;
	uint64		used;
} LookShmEnt;

/* a backend loading the rows of a key: the others that want them wait for it */
typedef struct LookShmClaim
{
	uint64		key;
	int			pid;			/* 0: free */
} LookShmClaim;

typedef struct LookShm
{
	slock_t		mutex;
	Size		bytes;
	uint64		clock;
	LookShmEnt	ent[LOOKSHM_SLOTS];
	LookShmClaim claim[LOOKSHM_CLAIMS];
} LookShm;

/* the start of a segment; the offsets are from the start of the segment */
typedef struct LookShmHead
{
	LookCols	cols;
	RelFileLocator rel;
	BlockNumber nblocks;
	Size		keylen;
	Size		key;
	Size		lsn;
	Size		map;
} LookShmHead;

/* a segment this backend has attached, for the scans that read it: it cannot be attached twice */
typedef struct LookShmAtt
{
	dsm_handle	h;
	dsm_segment *seg;
	int			refs;
} LookShmAtt;

typedef struct LookShmPin
{
	MemoryContextCallback cb;
	LookShmAtt *att;
} LookShmPin;

static LookShm *shm;
static List *attached;

#if PG_VERSION_NUM >= 190000
#define SHM_INIT_ARGS		void *ptr, void *arg
#define SHM_GET(init, found)	GetNamedDSMSegment("sift_agg_lookup", sizeof(LookShm), init, found, NULL)
#else
#define SHM_INIT_ARGS		void *ptr
#define SHM_GET(init, found)	GetNamedDSMSegment("sift_agg_lookup", sizeof(LookShm), init, found)
#endif

static void
shm_init(SHM_INIT_ARGS)
{
	memset(ptr, 0, sizeof(LookShm));
	SpinLockInit(&((LookShm *) ptr)->mutex);
}

static LookShm *
shm_get(void)
{
	bool		found;

	if (shm == NULL)
		shm = SHM_GET(shm_init, &found);
	return shm;
}

static uint64
key_hash(const char *key)
{
	return hash_bytes_extended((const unsigned char *) key, strlen(key), MyDatabaseId);
}

static uint64
pages_hash(const LookFinger *f)
{
	return hash_bytes_extended((const unsigned char *) f->lsn, f->nblocks * sizeof(XLogRecPtr), ((uint64) f->rel.relNumber << 32) ^ f->nblocks);
}

static void
shm_unpin(void *arg)
{
	LookShmPin *p = arg;

	if (--p->att->refs == 0)
	{
		attached = list_delete_ptr(attached, p->att);
		dsm_detach(p->att->seg);
		pfree(p->att);
	}
}

/* the segment h, attached once by this backend and detached when the last scan that reads it is done with mcxt */
static dsm_segment *
shm_attach(dsm_handle h, MemoryContext mcxt)
{
	LookShmAtt *att = NULL;
	LookShmPin *p;

	foreach_ptr(LookShmAtt, a, attached)
		if (a->h == h)
			att = a;
	if (att == NULL)
	{
		dsm_segment *seg = dsm_attach(h);
		MemoryContext old;

		if (seg == NULL)
			return NULL;
		dsm_pin_mapping(seg);
		old = MemoryContextSwitchTo(TopMemoryContext);
		att = palloc(sizeof(LookShmAtt));
		att->h = h;
		att->seg = seg;
		att->refs = 0;
		attached = lappend(attached, att);
		MemoryContextSwitchTo(old);
	}
	att->refs++;
	p = MemoryContextAlloc(mcxt, sizeof(LookShmPin));
	p->cb = (MemoryContextCallback) {shm_unpin, p, NULL};
	p->att = att;
	MemoryContextRegisterResetCallback(mcxt, &p->cb);
	return att->seg;
}

static bool
head_is(const LookShmHead *hd, const char *key, const LookFinger *f)
{
	const char *base = (const char *) hd;

	return hd->keylen == strlen(key) && memcmp(base + hd->key, key, hd->keylen) == 0 && RelFileLocatorEquals(hd->rel, f->rel) &&
		hd->nblocks == f->nblocks && memcmp(base + hd->lsn, f->lsn, f->nblocks * sizeof(XLogRecPtr)) == 0;
}

/* The rows kept for the key of a table that has the pages of f and the columns need, read in place and valid until mcxt goes; NULL when there are none. *have as in lookcache_find */
MapTab *
lookshm_find(const char *key, const LookFinger *f, const LookCols *need, LookCols *have, MemoryContext mcxt)
{
	LookShm    *s;
	uint64		kh = key_hash(key),
				ph = pages_hash(f);
	dsm_handle	h = DSM_HANDLE_INVALID;
	dsm_segment *seg;
	const LookShmHead *hd;

	have->n = 0;
	if (lookshm_bytes() == 0)
		return NULL;
	s = shm_get();
	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < LOOKSHM_SLOTS; i++)
	{
		LookShmEnt *e = &s->ent[i];

		if (e->h == DSM_HANDLE_INVALID || e->key != kh || e->pages != ph)
			continue;
		*have = e->cols;
		if (cols_cover(&e->cols, need))
		{
			h = e->h;
			e->used = ++s->clock;
		}
		break;
	}
	SpinLockRelease(&s->mutex);
	if (h == DSM_HANDLE_INVALID || (seg = shm_attach(h, mcxt)) == NULL)
		return NULL;
	hd = dsm_segment_address(seg);
	/* a hash is not proof: the entry is used only if it is the key and the pages */
	if (!head_is(hd, key, f))
	{
		have->n = 0;
		return NULL;
	}
	return map_share_attach((const char *) hd + hd->map, NULL, mcxt);
}

/* removes the least recently used entry from the directory, for the caller to unpin; false when it is empty */
static bool
shm_evict(LookShm *s, dsm_handle *drop, int *ndrop)
{
	LookShmEnt *lru = NULL;

	for (int i = 0; i < LOOKSHM_SLOTS; i++)
		if (s->ent[i].h != DSM_HANDLE_INVALID && (lru == NULL || s->ent[i].used < lru->used))
			lru = &s->ent[i];
	if (lru == NULL)
		return false;
	drop[(*ndrop)++] = lru->h;
	s->bytes -= lru->size;
	lru->h = DSM_HANDLE_INVALID;
	return true;
}

/*
 * A segment of `size` bytes, NULL when there is no room for it. With the budget on its own (-1) a segment that cannot be made, for the
 * file system of the segments is full or small (a container's /dev/shm), does not fail the query: the backend loads its tables as it did and
 * does not try again.
 */
static dsm_segment *
segment_create(Size size)
{
	dsm_segment *seg = NULL;
	MemoryContext old = CurrentMemoryContext;

	if (sift_agg_lookup_shared_kb >= 0)
		return dsm_create(size, DSM_CREATE_NULL_IF_MAXSEGMENTS);
	PG_TRY();
	{
		seg = dsm_create(size, DSM_CREATE_NULL_IF_MAXSEGMENTS);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(old);
		FlushErrorState();
		shm_failed = true;
		ereport(NOTICE,
				(errmsg("sift: the rows of a joined table are not shared with other backends: no dynamic shared memory for %zu bytes", size),
				 errhint("Make room for dynamic shared memory (dynamic_shared_memory_type, the size of /dev/shm), or set sift.agg_lookup_shared_kb to 0.")));
	}
	PG_END_TRY();
	return seg;
}

/*
 * Keeps the rows m of a table with the pages of f for the other backends, in place of the entry of the key. False when they are not kept; *large
 * is then whether they take more than half of the budget (or the budget is 0), the rows no state of the table would fit.
 */
bool
lookshm_keep(const char *key, const LookFinger *f, MapTab *m, const LookCols *cols, bool *large)
{
	LookShm    *s;
	MapShared	l;
	LookShmHead *hd;
	LookShmEnt *at = NULL;
	dsm_segment *seg;
	dsm_handle	drop[LOOKSHM_SLOTS + 1];
	int			ndrop = 0;
	Size		keylen = strlen(key),
				lsn = MAXALIGN(sizeof(LookShmHead)) + MAXALIGN(keylen),
				map = lsn + MAXALIGN(f->nblocks * sizeof(XLogRecPtr)),
				size;
	uint64		kh = key_hash(key);
	Size		budget = lookshm_bytes();

	*large = true;
	if (budget == 0)
		return false;
	size = map + map_share_size(m, NULL, &l);
	if (size > budget / 2)
		return false;
	*large = false;
	if ((seg = segment_create(size)) == NULL)
		return false;
	s = shm_get();
	hd = dsm_segment_address(seg);
	memset(hd, 0, sizeof(LookShmHead));
	hd->cols = *cols;
	hd->rel = f->rel;
	hd->nblocks = f->nblocks;
	hd->keylen = keylen;
	hd->key = MAXALIGN(sizeof(LookShmHead));
	hd->lsn = lsn;
	hd->map = map;
	memcpy((char *) hd + hd->key, key, keylen);
	memcpy((char *) hd + lsn, f->lsn, f->nblocks * sizeof(XLogRecPtr));
	map_share_write(m, NULL, &l, (char *) hd + map);
	/* the segment stays when this backend detaches, until the directory drops it and the last reader is done */
	dsm_pin_segment(seg);

	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < LOOKSHM_SLOTS; i++)
		if (s->ent[i].h != DSM_HANDLE_INVALID && s->ent[i].key == kh)
		{
			drop[ndrop++] = s->ent[i].h;
			s->bytes -= s->ent[i].size;
			s->ent[i].h = DSM_HANDLE_INVALID;
		}
	while (s->bytes + size > budget && shm_evict(s, drop, &ndrop))
		;
	for (;;)
	{
		for (int i = 0; i < LOOKSHM_SLOTS && at == NULL; i++)
			if (s->ent[i].h == DSM_HANDLE_INVALID)
				at = &s->ent[i];
		if (at != NULL || !shm_evict(s, drop, &ndrop))
			break;
	}
	if (at != NULL)
	{
		at->h = dsm_segment_handle(seg);
		at->key = kh;
		at->pages = pages_hash(f);
		at->cols = *cols;
		at->size = size;
		at->used = ++s->clock;
		s->bytes += size;
	}
	SpinLockRelease(&s->mutex);

	for (int i = 0; i < ndrop; i++)
		dsm_unpin_segment(drop[i]);
	if (at == NULL)
		dsm_unpin_segment(dsm_segment_handle(seg));
	dsm_detach(seg);
	return at != NULL;
}

typedef struct LookShmRelease
{
	MemoryContextCallback cb;
	uint64		key;
	bool		taken;
} LookShmRelease;

static void
claim_drop(uint64 kh)
{
	LookShm    *s = shm_get();

	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < LOOKSHM_CLAIMS; i++)
		if (s->claim[i].pid == MyProcPid && s->claim[i].key == kh)
			s->claim[i].pid = 0;
	SpinLockRelease(&s->mutex);
}

/* the statement ended, or failed, while the claim was held */
static void
claim_release(void *arg)
{
	claim_drop(((LookShmRelease *) arg)->key);
}

/* the rows of the key are loaded (kept or not): the backends that wait for them look again */
void
lookshm_unclaim(const char *key)
{
	if (lookshm_bytes() != 0)
		claim_drop(key_hash(key));
}

/* the pid of another backend that is loading the rows of the key, 0 when none is; with take, the claim is the caller's then (r keeps its release) */
static int
claim_try(LookShm *s, uint64 kh, LookShmRelease *r)
{
	int			pid = 0;

	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < LOOKSHM_CLAIMS; i++)
		if (s->claim[i].pid != 0 && s->claim[i].pid != MyProcPid && s->claim[i].key == kh)
			pid = s->claim[i].pid;
	for (int i = 0; pid == 0 && r != NULL && i < LOOKSHM_CLAIMS; i++)
		if (s->claim[i].pid == 0)
		{
			s->claim[i].pid = MyProcPid;
			s->claim[i].key = kh;
			r->taken = true;
			break;
		}
	SpinLockRelease(&s->mutex);
	return pid;
}

/* the claims of a backend that is gone are dropped */
static void
claim_clear(LookShm *s, int pid)
{
	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < LOOKSHM_CLAIMS; i++)
		if (s->claim[i].pid == pid)
			s->claim[i].pid = 0;
	SpinLockRelease(&s->mutex);
}

/*
 * The backend is about to load the rows of a key that nobody has kept. Many sessions that run the same query at once would each load a copy;
 * the first to ask claims the load, until mcxt goes, and the others wait for it (at most LOOKSHM_WAIT_MS) to find its rows in the shared
 * memory. True when this backend waited and should look for the rows again; false when it loads them (it has the claim, or there is no
 * claim to take, or the other backend took too long).
 */
bool
lookshm_claim(const char *key, MemoryContext mcxt)
{
	LookShm    *s;
	uint64		kh = key_hash(key);
	TimestampTz start = GetCurrentTimestamp();
	LookShmRelease *r;
	bool		waited = false;

	if (lookshm_bytes() == 0)
		return false;
	s = shm_get();
	r = MemoryContextAllocZero(mcxt, sizeof(LookShmRelease));
	r->key = kh;
	for (;;)
	{
		int			pid = claim_try(s, kh, waited ? NULL : r);

		if (pid == 0)
			break;
		if (BackendPidGetProc(pid) == NULL)
		{
			claim_clear(s, pid);
			continue;
		}
		if (TimestampDifferenceExceeds(start, GetCurrentTimestamp(), LOOKSHM_WAIT_MS))
			return false;
		waited = true;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, LOOKSHM_POLL_MS, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
	if (r->taken)
	{
		r->cb = (MemoryContextCallback) {claim_release, r, NULL};
		MemoryContextRegisterResetCallback(mcxt, &r->cb);
	}
	return waited;
}
