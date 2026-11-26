/*
 * col/dictshm/dictshm.c - the parsed dictionaries of segments, shared by the backends of a server.
 *
 * A backend that meets a dictionary inflates and indexes all of it (the strings of a million-row ip leaf: 170 million instructions), and col/dictcache.c keeps the
 * result for its own life, so a fresh backend, a pooled connection or a parallel worker does it again. The first backend to parse one keeps its offsets and strings
 * here, under the key of its own cache (the file, generation, rows and header block of the segment, the leaf: a segment never changes, a merge or a rewrite names a
 * new one), and the others read them where they lie, pinned in the ring. What a lookup builds from them (hash slots, sorted order) is written to a new record once a dictionary is asked often.
 *
 * As col/leafshm.c: a ring of bytes in the DSM registry (no shared_preload_libraries), a directory and the copy outside of it; a pin count keeps
 * the ring off a record being read, and a record names its key, which the reader compares. A reader holds the lock of one bucket shared to pin a record and gives the pin
 * back without a lock; the writers, which take their place in the ring and enter their record, hold the ring lock and the lock of the bucket they change. Where the table cannot be had the backend goes without it. The directory
 * is buckets of DSHM_WAYS entries found by the hash of the key. A writer that finds more entries thrown out than found in DSHM_WINDOW writes stops for DSHM_HOLD attempts: a ring too small for the dictionaries
 * asked does not churn.
 *
 * Two registry segments: the directory (small, made by the first backend that looks, and it records the size of the ring) and the ring of records (made by the
 * first backend that keeps a dictionary, at the size the directory records, whatever the setting of the others). sift.shared_cache_kb = 0 makes and attaches
 * neither.
 */
#include "postgres.h"

#include <sys/statvfs.h>

#include "common/hashfn.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "storage/dsm_impl.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "utils/memutils.h"

#include "agg/stats.h"
#include "core/cmp.h"
#include "core/shm/budget.h"
#include "col/dictshm/dictshm.h"
#include "col/dictshm/int.h"
#include "col/dsmreg.h"



#ifdef SIFT_TESTING
int			sift_test_col_dict_shared = 0;
#define TEST_MODE(m)	(sift_test_col_dict_shared == (m))
#else
#define TEST_MODE(m)	false
#endif


static Dshm *dshm;
static char *ring;
static bool dshm_failed,
			ring_failed;

/* The setting, a multiple of the record alignment, and at most a quarter of /dev/shm: the segments of parallel queries live in the same one (a container has 64 MB); *capped says the second */
static Size
ring_bytes(bool *capped)
{
	Size		bytes = TYPEALIGN(DSHM_ALIGN, sift_shm_pool_bytes(SIFT_SHM_COL_DICT));
	struct statvfs sv;

	*capped = false;
	if (dynamic_shared_memory_type == DSM_IMPL_POSIX && statvfs("/dev/shm", &sv) == 0)
	{
		Size		room = Max((Size) 4 << 20, (Size) sv.f_blocks * sv.f_frsize / 4);

		if (bytes > room)
		{
			bytes = room;
			*capped = true;
		}
	}
	return bytes;
}

/* The cap is said once for the postmaster, outside the lock of the registry that makes the table (sift.shared_memory() shows it as the reason of the ring) */
static void
ring_cap_report(void)
{
	bool		capped;
	Size		bytes = ring_bytes(&capped);
	char		note[160];

	if (!capped)
		return;
	snprintf(note, sizeof(note), "capped at %zu kB, a quarter of /dev/shm (the share of sift.shared_cache_kb is %zu kB)", bytes / 1024, sift_shm_pool_bytes(SIFT_SHM_COL_DICT) / 1024);
	if (sift_shm_first(DSHM_RING_NAME, note))
		ereport(LOG,
				(errmsg("sift: the shared dictionaries take %zu kB, not the %zu kB of its share of sift.shared_cache_kb: a quarter of /dev/shm", bytes / 1024, sift_shm_pool_bytes(SIFT_SHM_COL_DICT) / 1024)));
}

static void
dshm_init(SIFT_DSM_INIT_ARGS)
{
	Dshm	   *s = ptr;
	bool		capped;

	memset(s, 0, sizeof(Dshm));
	s->tranche = sift_tranche_new(DSHM_TRANCHE);
	LWLockInitialize(&s->ring_lock.lock, s->tranche);
	for (int i = 0; i < DSHM_BUCKETS; i++)
		LWLockInitialize(&s->bucket_lock[i].lock, s->tranche);
	pg_atomic_init_u64(&s->tick, 0);
	for (int i = 0; i < lengthof(s->dir); i++)
	{
		pg_atomic_init_u64(&s->dir[i].orows, 0);
		pg_atomic_init_u64(&s->dir[i].tick, 0);
		pg_atomic_init_u32(&s->dir[i].asks, 0);
		pg_atomic_init_u32(&s->dir[i].pins, 0);
	}
	for (int i = 0; i < DSHM_COUNTS; i++)
	{
		pg_atomic_init_u64(&s->count[i].hits, 0);
		pg_atomic_init_u64(&s->count[i].misses, 0);
	}
	s->size = ring_bytes(&capped);
}

/* The ring is made on the first record kept, not on the first read: a server that shares no dictionary takes only the directory */
static void
ring_init(SIFT_DSM_INIT_ARGS)
{
	pg_write_barrier();
	dshm->ring_ready = true;
}

/* The directory of a server that shares dictionaries (nothing at all with the setting at 0) */
Dshm *
dshm_get(void)
{
	if (sift_shm_pool_bytes(SIFT_SHM_COL_DICT) == 0 || dshm_failed)
		return NULL;
	if (dshm == NULL)
	{
		dshm = sift_dsm_attach(DSHM_NAME, sizeof(Dshm), dshm_init, &dshm_failed,
							   "the parsed dictionaries of the column indexes", "sift.shared_cache_kb", TEST_MODE(2));
		if (dshm != NULL)
		{
			sift_tranche_register(dshm->tranche, DSHM_TRANCHE);
			ring_cap_report();
		}
	}
	return dshm;
}

/*
 * The ring of the directory s, at the size the directory records. A parallel worker, or the leader inside the parallel operation, takes a ring that is there and does not make one
 * (it is made on the first record kept, by a backend that is not in a parallel operation).
 */
bool
ring_get(Dshm *s)
{
	if (ring != NULL)
		return true;
	if (ring_failed || (IsInParallelMode() && !s->ring_ready))
		return false;
	ring = sift_dsm_attach(DSHM_RING_NAME, s->size, ring_init, &ring_failed, "the parsed dictionaries of the column indexes", "sift.shared_cache_kb", false);
	return ring != NULL;
}

/* The directory is there for this backend, and the ring too when a dictionary is already kept: a parallel leader asks before it enters parallel mode */
bool
sift_dictshm_prepare(void)
{
	Dshm	   *s = dshm_get();

	if (s != NULL && s->ring_ready)
		ring_get(s);
	return s != NULL;
}

void
sift_dictshm_join(void)
{
	if ((dshm = sift_dsm_attach(DSHM_NAME, sizeof(Dshm), dshm_init, &dshm_failed, "the parsed dictionaries of the column indexes", "sift.shared_cache_kb", false)) == NULL)
		return;
	sift_tranche_register(dshm->tranche, DSHM_TRANCHE);
	if (dshm->ring_ready)
		ring_get(dshm);
}

/* The arrays of a record: where each lies, and the size of all (a multiple of DSHM_ALIGN) */
Layout
layout(uint32 n, uint32 nbytes, uint32 flags, uint32 nslots)
{
	Layout		l;

	l.bytes_at = sizeof(DshmHdr) + TYPEALIGN(8, ((Size) n + 1) * sizeof(uint32));
	l.slots_at = l.bytes_at + TYPEALIGN(8, nbytes);
	l.order_at = l.slots_at + ((flags & F_SLOTS) ? TYPEALIGN(8, (Size) nslots * sizeof(uint64)) : 0);
	l.total = TYPEALIGN(DSHM_ALIGN, l.order_at + ((flags & F_ORDER) ? TYPEALIGN(8, (Size) n * sizeof(uint32)) : 0));
	return l;
}

DshmEnt *
bucket(Dshm *s, const DictKey *k)
{
	return &s->dir[(hash_bytes((const unsigned char *) k, sizeof(*k)) % DSHM_BUCKETS) * DSHM_WAYS];
}

/* the lock of the bucket that holds the entry at slot of the directory */
static LWLock *
bucket_lock_of(Dshm *s, int slot)
{
	return &s->bucket_lock[slot / DSHM_WAYS].lock;
}

static DshmCount *
my_count(Dshm *s)
{
	return &s->count[MyProcNumber & (DSHM_COUNTS - 1)];
}

/* the reads that found a record, of all backends, since the table was made */
uint64
dshm_hits(Dshm *s)
{
	uint64		n = 0;

	for (int i = 0; i < DSHM_COUNTS; i++)
		n += pg_atomic_read_u64(&s->count[i].hits);
	return n;
}

/* the entry holds a live record: the other fields are the caller's */
void
ent_set(DshmEnt *e, const DictKey *k, Size off, Size len, uint32 hdr_flags, uint32 asks, uint64 orows, uint64 tick)
{
	e->key = *k;
	e->off = off;
	e->len = len;
	e->hdr_flags = hdr_flags;
	pg_atomic_write_u32(&e->asks, asks);
	pg_atomic_write_u32(&e->pins, 0);
	pg_atomic_write_u64(&e->orows, orows);
	pg_atomic_write_u64(&e->tick, tick);
	e->state = E_LIVE;
}

/* the entry was read: when its tick is old it is made new, so that the oldest of a full bucket is one that nobody reads */
static void
ent_touch(Dshm *s, DshmEnt *e)
{
	uint64		now = pg_atomic_read_u64(&s->tick);

	if (now - pg_atomic_read_u64(&e->tick) >= DSHM_TICK_LAG)
		pg_atomic_write_u64(&e->tick, now);
}

/* A backend's reads in place: the pin on the record of the dictionary goes with the memory context of the scan, or with the backend */
typedef struct ShmPin
{
	MemoryContextCallback cb;
	dlist_node	node;
	int			slot;			/* -1: given back */
	SiftCDict  *d;				/* the dictionary read in place: the rows its top-N scans compared go back to the entry */
	uint64		orows;			/* those the entry had when it was pinned */
}			ShmPin;

static dlist_head pins = DLIST_STATIC_INIT(pins);
static bool exit_hooked;

/* the last reader of a record that a newer one replaced gives its place back; a writer that marked it stale and saw a pin left it to this */
static void
stale_free(Dshm *s, int slot)
{
	DshmEnt    *e = &s->dir[slot];

	LWLockAcquire(bucket_lock_of(s, slot), LW_EXCLUSIVE);
	if (e->state == E_STALE && pg_atomic_read_u32(&e->pins) == 0)
		e->state = E_FREE;
	LWLockRelease(bucket_lock_of(s, slot));
}

static void
slot_unpin(Dshm *s, int slot)
{
	if (pg_atomic_sub_fetch_u32(&s->dir[slot].pins, 1) == 0 && s->dir[slot].state == E_STALE)
		stale_free(s, slot);
}

static void
unpin(void *arg)
{
	ShmPin	   *p = arg;
	DshmEnt    *e;

	if (p->slot < 0)
		return;
	e = &dshm->dir[p->slot];
	dlist_delete(&p->node);
	if (p->d != NULL && p->d->orows > p->orows)
		pg_atomic_fetch_add_u64(&e->orows, p->d->orows - p->orows);
	slot_unpin(dshm, p->slot);
	p->slot = -1;
}

/* before the segment is detached: a backend that leaves holding records (FATAL, exit) gives them back */
static void
unpin_all(int code, Datum arg)
{
	while (!dlist_is_empty(&pins))
		unpin(dlist_container(ShmPin, node, dlist_head_node(&pins)));
}

/* The dictionary of the record rec (its header h, its layout l), read in place: its arrays are those of the ring */
static SiftCDict *
dict_in_place(MemoryContext parent, const char *rec, const DshmHdr *h, const Layout *l, uint32 asks, uint64 orows)
{
	SiftCDict  *d = sift_cdict_create(AllocSetContextCreate(parent, "sift shared dictionary", ALLOCSET_SMALL_SIZES));

	pfree(d->off);
	pfree(d->bytes);
	d->n = h->n;
	d->cap = h->n;
	d->off = (uint32 *) (rec + sizeof(*h));
	d->nbytes = h->nbytes;
	d->bytescap = h->nbytes;
	d->bytes = (char *) rec + l->bytes_at;
	d->finds = d->tests = asks;
	d->orows = orows;
	d->blocked = (h->flags & F_BLOCKED) != 0;
	if (h->flags & F_SLOTS)
	{
		d->slots = (uint64 *) (rec + l->slots_at);
		d->nslots = h->nslots;
	}
	if (h->flags & F_ORDER)
	{
		d->sorted = (uint32 *) (rec + l->order_at);
		d->stored_order = (h->flags & F_SORTED) != 0;
	}
	return d;
}

SiftCDict *
sift_dictshm_read(const DictKey *k, MemoryContext parent)
{
	Dshm	   *s = dshm_get();
	DshmEnt    *b;
	int			at = -1;
	Size		off = 0,
				len = 0;
	uint32		asks = 0;
	uint64		orows = 0;
	bool		bad = true;
	DshmHdr		h;
	Layout		l;
	const char *rec;
	SiftCDict  *d;
	ShmPin	   *p;

	if (s == NULL)
		return NULL;
	b = bucket(s, k);
	LWLockAcquire(bucket_lock_of(s, (int) (b - s->dir)), LW_SHARED);
	for (int i = 0; i < DSHM_WAYS; i++)
		if (b[i].state == E_LIVE && memcmp(&b[i].key, k, sizeof(*k)) == 0)
		{
			pg_atomic_fetch_add_u32(&b[i].pins, 1);
			asks = pg_atomic_add_fetch_u32(&b[i].asks, 1);
			orows = pg_atomic_read_u64(&b[i].orows);
			ent_touch(s, &b[i]);
			at = (int) (b - s->dir) + i;
			off = b[i].off;
			len = b[i].len;
			break;
		}
	LWLockRelease(bucket_lock_of(s, (int) (b - s->dir)));
	pg_atomic_fetch_add_u64(at < 0 ? &my_count(s)->misses : &my_count(s)->hits, 1);
	if (at < 0)
		return NULL;
	/* the record cannot change while it is pinned: what is checked here is that it is inside the ring, that it is one, and the one asked for (a stale entry is a miss) */
	if (ring_get(s) && off + len <= s->size && len >= sizeof(h))
	{
		rec = ring + off;
		memcpy(&h, rec, sizeof(h));
		l = layout(h.n, h.nbytes, h.flags, h.nslots);
		bad = h.magic != DSHM_MAGIC || memcmp(&h.key, k, sizeof(*k)) != 0 || h.n == 0 || l.total != len ||
			((h.flags & F_SLOTS) && ((h.nslots & (h.nslots - 1)) != 0 || ((Size) h.n + 1) * 2 > h.nslots));
	}
	if (bad)
	{
		slot_unpin(s, at);
		return NULL;
	}
	PG_TRY();
	{
#ifdef SIFT_TESTING
		if (sift_test_dsm_fail != NULL && strstr(sift_test_dsm_fail, "dictshm_read") != NULL)
			ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("sift test: the shared dictionary cannot be read in place (dictshm_read)")));	/* check-errors: allow */
#endif
		d = dict_in_place(parent, rec, &h, &l, asks, orows);
		p = MemoryContextAlloc(d->mcxt, sizeof(ShmPin));
		if (!exit_hooked)
		{
			before_shmem_exit(unpin_all, 0);
			exit_hooked = true;
		}
	}
	PG_CATCH();
	{
		slot_unpin(s, at);
		PG_RE_THROW();
	}
	PG_END_TRY();
	p->slot = at;
	p->d = d;
	p->orows = orows;
	p->cb = (MemoryContextCallback) {unpin, p, NULL};
	dlist_push_head(&pins, &p->node);
	MemoryContextRegisterResetCallback(d->mcxt, &p->cb);
	return d;
}

/* The first place of the index that ends after at: the places that a record at [at, at + len) covers follow it while they begin before at + len */
static int
ord_first(const Dshm *s, Size at)
{
	int			lo = 0,
				hi = s->nord;

	SIFT_BISECT(int, m, lo, hi, s->dir[s->ord[m]].off + s->dir[s->ord[m]].len <= at);
	return lo;
}

void
ord_drop(Dshm *s, int slot)
{
	int			i = ord_first(s, s->dir[slot].off);

	if (i < s->nord && s->ord[i] == slot)
		memmove(s->ord + i, s->ord + i + 1, (s->nord-- - i - 1) * sizeof(int16));
}

static void
ord_add(Dshm *s, int slot)
{
	int			i = ord_first(s, s->dir[slot].off);

	memmove(s->ord + i + 1, s->ord + i, (s->nord++ - i) * sizeof(int16));
	s->ord[i] = (int16) slot;
}

/* Where the pinned records and the flights that [at, at + len) covers end, the last of them; 0: none */
static Size
ring_busy_until(Dshm *s, Size at, Size len)
{
	Size		next = 0;

	for (int i = ord_first(s, at); i < s->nord && s->dir[s->ord[i]].off < at + len; i++)
		if (s->dir[s->ord[i]].state != E_FREE && pg_atomic_read_u32(&s->dir[s->ord[i]].pins) > 0)
			next = Max(next, s->dir[s->ord[i]].off + s->dir[s->ord[i]].len);
	for (int i = 0; i < DSHM_FLIGHT; i++)
		if (s->flight[i].len > 0 && s->flight[i].off < at + len && at < s->flight[i].off + s->flight[i].len)
			next = Max(next, s->flight[i].off + s->flight[i].len);
	return next;
}

/*
 * The records that [at, at + len) covers go, each under the lock of its bucket, which no reader that pins holds meanwhile. Where one was pinned after ring_busy_until looked, where
 * that record ends; 0: all went.
 */
static Size
ring_evict(Dshm *s, Size at, Size len, int *evicted)
{
	for (int i = ord_first(s, at); i < s->nord && s->dir[s->ord[i]].off < at + len; i++)
	{
		DshmEnt    *e = &s->dir[s->ord[i]];
		LWLock	   *lock = bucket_lock_of(s, s->ord[i]);
		Size		pinned_until = 0;

		if (e->state == E_FREE)
			continue;
		LWLockAcquire(lock, LW_EXCLUSIVE);
		if (pg_atomic_read_u32(&e->pins) > 0)
			pinned_until = e->off + e->len;
		else
		{
			*evicted += e->state == E_LIVE;
			e->state = E_FREE;
			s->thrown++;
		}
		LWLockRelease(lock);
		if (pinned_until > 0)
			return pinned_until;
	}
	return 0;
}

/*
 * Takes len bytes of the ring for a record, not over the records that are pinned or being copied (the ring lock is held): the next place after the head that
 * is free; the unpinned records it covers go. False when there is none.
 */
bool
ring_take(Dshm *s, Size len, Size *off, int *flight, int *evicted)
{
	Size		at = s->head + len > s->size ? 0 : s->head;
	int			slot = -1,
				first,
				end;

	for (int tries = 0; tries < DSHM_TRIES; tries++)
	{
		Size		next = ring_busy_until(s, at, len);

		if (next == 0)
			next = ring_evict(s, at, len, evicted);
		if (next == 0)
			break;
		at = TYPEALIGN(DSHM_ALIGN, next);
		if (at + len > s->size)
			at = 0;
		if (tries == DSHM_TRIES - 1)
			return false;
	}
	for (int i = 0; i < DSHM_FLIGHT && slot < 0; i++)
		if (s->flight[i].len == 0)
			slot = i;
	if (slot < 0)
		return false;
	first = ord_first(s, at);
	for (end = first; end < s->nord && s->dir[s->ord[end]].off < at + len; end++)
		;
	memmove(s->ord + first, s->ord + end, (s->nord - end) * sizeof(int16));
	s->nord -= end - first;
	s->flight[slot] = (DshmFlight) {.off = at, .len = len};
	s->head = at + len;
	*off = at;
	*flight = slot;
	return true;
}

/* the entry of the key that holds a live record with the arrays of flags (a record that lacks one is made again), NULL: none */
static DshmEnt *
ent_with(Dshm *s, const DictKey *k, uint32 flags)
{
	DshmEnt    *b = bucket(s, k);

	for (int i = 0; i < DSHM_WAYS; i++)
		if (b[i].state == E_LIVE && memcmp(&b[i].key, k, sizeof(*k)) == 0 && (b[i].hdr_flags & flags) == flags)
			return &b[i];
	return NULL;
}

/* whether a backend is copying in a record of the key with the arrays of flags (the ring lock is held) */
static bool
in_flight(const Dshm *s, const DictKey *k, uint32 flags)
{
	for (int i = 0; i < DSHM_FLIGHT; i++)
		if (s->flight[i].len > 0 && memcmp(&s->flight[i].key, k, sizeof(*k)) == 0 && (s->flight[i].flags & flags) == flags)
			return true;
	return false;
}

/* the live record of the key is read no more: it goes when nobody reads it. The pin is looked at after the mark: a reader that gave its last pin back before it saw the mark frees nothing */
static void
ent_retire(DshmEnt *e)
{
	e->state = E_STALE;
	pg_memory_barrier();
	if (pg_atomic_read_u32(&e->pins) == 0)
		e->state = E_FREE;
}

/* the writes of a window that threw out more records than the reads found in the ring in it stop for DSHM_HOLD attempts */
static void
window_note_write(Dshm *s)
{
	if (++s->writes < DSHM_WINDOW)
		return;
	if (s->thrown > dshm_hits(s) - s->window_hits)
		s->hold = DSHM_HOLD;
	s->writes = s->thrown = 0;
	s->window_hits = dshm_hits(s);
}

/* the record, whose arrays the caller has copied into [off, off + l.total), is entered into the directory in the place of those of its key; the entry's slot of the bucket, -1: none free */
static int
ent_enter(Dshm *s, const DictKey *k, uint32 flags, Size off, Size total, int *evicted)
{
	DshmEnt    *b = bucket(s, k);
	uint32		asks = 0;
	uint64		orows = 0;
	int			slot = -1;

	for (int i = 0; i < DSHM_WAYS; i++)
		if (b[i].state == E_LIVE && memcmp(&b[i].key, k, sizeof(*k)) == 0)
		{
			asks = pg_atomic_read_u32(&b[i].asks);
			orows = pg_atomic_read_u64(&b[i].orows);
			ent_retire(&b[i]);
		}
	for (int i = 0; i < DSHM_WAYS && slot < 0; i++)
		if (b[i].state == E_FREE)
			slot = i;
	if (slot < 0)
	{
		for (int i = 0; i < DSHM_WAYS; i++)
			if (pg_atomic_read_u32(&b[i].pins) == 0 && (slot < 0 || pg_atomic_read_u64(&b[i].tick) < pg_atomic_read_u64(&b[slot].tick)))
				slot = i;
		s->thrown++;
		*evicted += slot >= 0;
	}
	if (slot >= 0)
	{
		ord_drop(s, (int) (b - s->dir) + slot);
		ent_set(&b[slot], k, off, total, flags, asks, orows, pg_atomic_read_u64(&s->tick));
		ord_add(s, (int) (b - s->dir) + slot);
	}
	return slot;
}

bool
sift_dictshm_write(const DictKey *k, const SiftCDict *d)
{
	Dshm	   *s;
	uint32		flags = (d->blocked ? F_BLOCKED : 0) | (d->sorted != NULL ? F_ORDER : 0) | (d->stored_order && d->sorted != NULL ? F_SORTED : 0) |
		(d->slots != NULL && ((Size) d->n + 1) * 2 <= d->nslots ? F_SLOTS : 0);
	Layout		l = layout(d->n, d->nbytes, flags, d->nslots);
	Size		off;
	DshmHdr		h = {.magic = DSHM_MAGIC, .key = *k, .n = d->n, .nbytes = d->nbytes, .flags = flags, .nslots = (flags & F_SLOTS) ? d->nslots : 0};
	char	   *rec;
	int			slot,
				flight,
				evicted = 0;

	if (l.total < (TEST_MODE(1) ? 0 : DSHM_MIN_BYTES) || d->n == 0 || (s = dshm_get()) == NULL || l.total > s->size / 2 || !ring_get(s))
		return false;
	sift_stat_add(SIFT_STAT_DICT_SHARED_MISSES, 1);
	LWLockAcquire(&s->ring_lock.lock, LW_EXCLUSIVE);
	if (ent_with(s, k, flags) != NULL)
	{
		LWLockRelease(&s->ring_lock.lock);
		return true;
	}
	if (in_flight(s, k, flags))
	{
		LWLockRelease(&s->ring_lock.lock);
		return false;
	}
	if (s->hold > 0 || !ring_take(s, l.total, &off, &flight, &evicted))
	{
		if (s->hold > 0)
			s->hold--;
		LWLockRelease(&s->ring_lock.lock);
		sift_stat_add(SIFT_STAT_DICT_SHARED_HELD, 1);
		return false;
	}
	s->flight[flight].key = *k;
	s->flight[flight].flags = flags;
	LWLockRelease(&s->ring_lock.lock);

	rec = ring + off;
	memcpy(rec, &h, sizeof(h));
	memcpy(rec + sizeof(h), d->off, ((Size) d->n + 1) * sizeof(uint32));
	memcpy(rec + l.bytes_at, d->bytes, d->nbytes);
	if (flags & F_SLOTS)
		memcpy(rec + l.slots_at, d->slots, (Size) d->nslots * sizeof(uint64));
	if (flags & F_ORDER)
		memcpy(rec + l.order_at, d->sorted, (Size) d->n * sizeof(uint32));

	LWLockAcquire(&s->ring_lock.lock, LW_EXCLUSIVE);
	LWLockAcquire(bucket_lock_of(s, (int) (bucket(s, k) - s->dir)), LW_EXCLUSIVE);
	s->flight[flight].len = 0;
	pg_atomic_write_u64(&s->tick, pg_atomic_read_u64(&s->tick) + 1);
	slot = ent_enter(s, k, flags, off, l.total, &evicted);
	LWLockRelease(bucket_lock_of(s, (int) (bucket(s, k) - s->dir)));
	window_note_write(s);
	LWLockRelease(&s->ring_lock.lock);
	sift_shm_note_eviction(SIFT_SHM_COL_DICT, evicted);
	return slot >= 0;
}


/* sift.shared_memory(): the bytes of the live records of the ring and the reads of the directory that found one and that did not */
bool
sift_dictshm_stat(SiftShmStat *st)
{
	Dshm	   *s = sift_dsm_find(DSHM_NAME, NULL);

	if (s == NULL)
		return false;
	sift_tranche_register(s->tranche, DSHM_TRANCHE);
	for (int b = 0; b < DSHM_BUCKETS; b++)
	{
		LWLockAcquire(&s->bucket_lock[b].lock, LW_SHARED);
		for (int i = b * DSHM_WAYS; i < (b + 1) * DSHM_WAYS; i++)
			if (s->dir[i].state == E_LIVE)
				st->used += s->dir[i].len;
		LWLockRelease(&s->bucket_lock[b].lock);
	}
	st->hits = dshm_hits(s);
	st->misses = 0;
	for (int i = 0; i < DSHM_COUNTS; i++)
		st->misses += pg_atomic_read_u64(&s->count[i].misses);
	st->has_used = st->has_counts = true;
	return true;
}
