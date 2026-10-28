/*
 * col/dictshm.c - the parsed dictionaries of segments, shared by the backends of a server.
 *
 * A backend that meets a dictionary inflates and indexes all of it (the strings of a million-row ip leaf: 170 million instructions), and col/dictcache.c keeps the
 * result for its own life, so a fresh backend, a pooled connection or a parallel worker does it again. The first backend to parse one keeps its offsets and strings
 * here, under the key of its own cache (the file, generation, rows and header block of the segment, the leaf: a segment never changes, a merge or a rewrite names a
 * new one), and the others read them where they lie, pinned in the ring. What a lookup builds from them (hash slots, sorted order) is written to a new record once a dictionary is asked often.
 *
 * As col/leafshm.c: a ring of bytes in the DSM registry (no shared_preload_libraries), a directory under a spinlock and the copy outside of it; a pin count keeps
 * the ring off a record being read, and a record names its key, which the reader compares. Where the table cannot be had the backend goes without it. The directory
 * is buckets of DSHM_WAYS entries found by the hash of the key. A writer that finds more entries thrown out than found in DSHM_WINDOW writes stops for DSHM_HOLD attempts: a ring too small for the dictionaries
 * asked does not churn.
 */
#include "postgres.h"

#include <sys/statvfs.h>

#include "common/hashfn.h"
#include "lib/ilist.h"
#include "storage/bufmgr.h"
#include "storage/dsm_impl.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "utils/memutils.h"

#include "agg/stats.h"
#include "col/dictshm.h"
#include "col/dsmreg.h"

#define DSHM_BUCKETS	64
#define DSHM_WAYS		8
#define DSHM_FLIGHT		8
/* places tried for a record before the write is given up: the ring is held by readers */
#define DSHM_TRIES		16
#define DSHM_MAGIC		0x44534833
/* the name carries the layout */
#define DSHM_NAME		"sift_col_dict_4"
/* the smallest record worth sharing: under it the segment is as cheap to read */
#define DSHM_MIN_BYTES	(128 * 1024)
#define DSHM_WINDOW		16
#define DSHM_HOLD		64
#define DSHM_ALIGN		64

#ifdef SIFT_TESTING
int			sift_test_col_dict_shared = 0;
#define TEST_MODE(m)	(sift_test_col_dict_shared == (m))
#else
#define TEST_MODE(m)	false
#endif

typedef enum
{
	E_FREE,
	E_LIVE,
	E_STALE						/* replaced by a newer record of its key, still read by the backends that pin it */
} EntState;

typedef struct DshmEnt
{
	uint8		state;
	uint32		asks;			/* times it was read, by all backends: a dictionary asked often is worth its hash table and sorted order (dict.c) */
	int32		pins;			/* backends reading the record in place: its bytes are not overwritten while there are any */
	DictKey		key;
	Size		off,
				len;
	uint64		tick;			/* oldest of a bucket goes when it is full */
}			DshmEnt;

typedef struct DshmFlight
{
	Size		off,
				len;			/* a record being copied in: len 0 when the place is free */
}			DshmFlight;

typedef struct Dshm
{
	slock_t		mutex;
	uint64		tick;
	Size		size;
	Size		head;
	uint32		hold;			/* writes still skipped */
	uint32		writes,
				thrown,
				found;			/* of the window */
	DshmFlight	flight[DSHM_FLIGHT];
	DshmEnt		dir[DSHM_BUCKETS * DSHM_WAYS];
	char		data[FLEXIBLE_ARRAY_MEMBER];
}			Dshm;

/*
 * A record, the arrays 8-aligned after the header: off[n + 1], the strings, the hash slots when the flag says so (nslots, as cdict_ready makes them), the
 * codes in byte order (n). Readers use them where they lie.
 */
typedef struct DshmHdr
{
	uint32		magic;
	uint32		n;
	uint32		nbytes;
	uint32		flags;
	uint32		nslots;
	DictKey		key;			/* whose record it is: a directory entry may name a place that another record took */
}			DshmHdr;

#define F_BLOCKED	1
#define F_SORTED	2			/* sorted is the stored order of the blocks form */
#define F_SLOTS		4
#define F_ORDER		8			/* sorted is there, stored or built */

static Dshm *dshm;
static bool dshm_failed;

static Size
ring_bytes(void)
{
	Size		bytes = Max((Size) 64 << 20, Min((Size) 512 << 20, (Size) NBuffers * BLCKSZ / 4));
	struct statvfs sv;

	if (TEST_MODE(4))
		return (Size) 8 << 20;

	/* the segments of parallel queries live in the same /dev/shm: the ring never takes more than a quarter of it (a container has 64 MB) */
	if (dynamic_shared_memory_type == DSM_IMPL_POSIX && statvfs("/dev/shm", &sv) == 0)
		bytes = Max((Size) 4 << 20, Min(bytes, (Size) sv.f_blocks * sv.f_frsize / 4));
	return TYPEALIGN(DSHM_ALIGN, bytes);
}

static void
dshm_init(SIFT_DSM_INIT_ARGS)
{
	Dshm	   *s = ptr;

	memset(s, 0, offsetof(Dshm, data));
	SpinLockInit(&s->mutex);
	s->size = ring_bytes();
}

static Dshm *
dshm_get(void)
{
	if (dshm_failed || TEST_MODE(1))
		return NULL;
	if (dshm == NULL)
		dshm = sift_dsm_attach(DSHM_NAME, offsetof(Dshm, data) + ring_bytes(), dshm_init, &dshm_failed,
							   "the parsed dictionaries of the column indexes", NULL, TEST_MODE(3));
	return dshm;
}

bool
sift_dictshm_prepare(void)
{
	return dshm_get() != NULL;
}

void
sift_dictshm_join(void)
{
	dshm = sift_dsm_join(DSHM_NAME, offsetof(Dshm, data) + ring_bytes(), dshm_init);
}

/* The arrays of a record: where each lies, and the size of all (a multiple of DSHM_ALIGN) */
typedef struct Layout
{
	Size		bytes_at,
				slots_at,
				order_at,
				total;
}			Layout;

static Layout
layout(uint32 n, uint32 nbytes, uint32 flags, uint32 nslots)
{
	Layout		l;

	l.bytes_at = sizeof(DshmHdr) + TYPEALIGN(8, ((Size) n + 1) * sizeof(uint32));
	l.slots_at = l.bytes_at + TYPEALIGN(8, nbytes);
	l.order_at = l.slots_at + ((flags & F_SLOTS) ? TYPEALIGN(8, (Size) nslots * sizeof(uint64)) : 0);
	l.total = TYPEALIGN(DSHM_ALIGN, l.order_at + ((flags & F_ORDER) ? TYPEALIGN(8, (Size) n * sizeof(uint32)) : 0));
	return l;
}

static DshmEnt *
bucket(Dshm *s, const DictKey *k)
{
	return &s->dir[(hash_bytes((const unsigned char *) k, sizeof(*k)) % DSHM_BUCKETS) * DSHM_WAYS];
}

/* A backend's reads in place: the pin on the record of the dictionary goes with the memory context of the scan, or with the backend */
typedef struct ShmPin
{
	MemoryContextCallback cb;
	dlist_node	node;
	int			slot;			/* -1: given back */
}			ShmPin;

static dlist_head pins = DLIST_STATIC_INIT(pins);
static bool exit_hooked;

static void
unpin(void *arg)
{
	ShmPin	   *p = arg;
	DshmEnt    *e;

	if (p->slot < 0)
		return;
	e = &dshm->dir[p->slot];
	dlist_delete(&p->node);
	SpinLockAcquire(&dshm->mutex);
	if (--e->pins == 0 && e->state == E_STALE)
		e->state = E_FREE;
	SpinLockRelease(&dshm->mutex);
	p->slot = -1;
}

/* before the segment is detached: a backend that leaves holding records (FATAL, exit) gives them back */
static void
unpin_all(int code, Datum arg)
{
	while (!dlist_is_empty(&pins))
		unpin(dlist_container(ShmPin, node, dlist_head_node(&pins)));
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
	DshmHdr		h;
	Layout		l;
	const char *rec;
	MemoryContext cx;
	SiftCDict  *d;
	ShmPin	   *p;

	if (s == NULL)
		return NULL;
	b = bucket(s, k);
	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < DSHM_WAYS; i++)
		if (b[i].state == E_LIVE && memcmp(&b[i].key, k, sizeof(*k)) == 0)
		{
			b[i].pins++;
			asks = ++b[i].asks;
			b[i].tick = ++s->tick;
			s->found++;
			at = (int) (b - s->dir) + i;
			off = b[i].off;
			len = b[i].len;
			break;
		}
	SpinLockRelease(&s->mutex);
	if (at < 0)
		return NULL;
	rec = s->data + off;
	memcpy(&h, rec, sizeof(h));
	l = layout(h.n, h.nbytes, h.flags, h.nslots);
	/* the record cannot change while it is pinned: what is checked here is that it is one, and the one asked for (a stale entry is a miss) */
	if (h.magic != DSHM_MAGIC || memcmp(&h.key, k, sizeof(*k)) != 0 || h.n == 0 || l.total != len ||
		((h.flags & F_SLOTS) && ((h.nslots & (h.nslots - 1)) != 0 || ((Size) h.n + 1) * 2 > h.nslots)))
	{
		ShmPin		tmp = {.slot = at};

		dlist_push_head(&pins, &tmp.node);
		unpin(&tmp);
		return NULL;
	}
	cx = AllocSetContextCreate(parent, "sift shared dictionary", ALLOCSET_SMALL_SIZES);
	d = sift_cdict_create(cx);
	pfree(d->off);
	pfree(d->bytes);
	d->n = h.n;
	d->cap = h.n;
	d->off = (uint32 *) (rec + sizeof(h));
	d->nbytes = h.nbytes;
	d->bytescap = h.nbytes;
	d->bytes = (char *) rec + l.bytes_at;
	d->finds = d->tests = asks;
	d->blocked = (h.flags & F_BLOCKED) != 0;
	if (h.flags & F_SLOTS)
	{
		d->slots = (uint64 *) (rec + l.slots_at);
		d->nslots = h.nslots;
	}
	if (h.flags & F_ORDER)
	{
		d->sorted = (uint32 *) (rec + l.order_at);
		d->stored_order = (h.flags & F_SORTED) != 0;
	}
	p = MemoryContextAlloc(cx, sizeof(ShmPin));
	p->slot = at;
	p->cb = (MemoryContextCallback) {unpin, p, NULL};
	if (!exit_hooked)
	{
		before_shmem_exit(unpin_all, 0);
		exit_hooked = true;
	}
	dlist_push_head(&pins, &p->node);
	MemoryContextRegisterResetCallback(cx, &p->cb);
	return d;
}

/*
 * Takes len bytes of the ring for a record, not over the records that are pinned or being copied (under the spinlock): the next place after the head that
 * is free; the unpinned records it covers go. False when there is none.
 */
static bool
ring_take(Dshm *s, Size len, Size *off, int *flight)
{
	Size		at = s->head + len > s->size ? 0 : s->head;
	int			slot = -1;

	for (int tries = 0; tries < DSHM_TRIES; tries++)
	{
		Size		next = 0;

		for (int i = 0; i < lengthof(s->dir); i++)
			if (s->dir[i].state != E_FREE && s->dir[i].pins > 0 && s->dir[i].off < at + len && at < s->dir[i].off + s->dir[i].len)
				next = Max(next, s->dir[i].off + s->dir[i].len);
		for (int i = 0; i < DSHM_FLIGHT; i++)
			if (s->flight[i].len > 0 && s->flight[i].off < at + len && at < s->flight[i].off + s->flight[i].len)
				next = Max(next, s->flight[i].off + s->flight[i].len);
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
	for (int i = 0; i < lengthof(s->dir); i++)
		if (s->dir[i].state != E_FREE && s->dir[i].off < at + len && at < s->dir[i].off + s->dir[i].len)
		{
			s->dir[i].state = E_FREE;
			s->thrown++;
		}
	s->flight[slot] = (DshmFlight) {.off = at, .len = len};
	s->head = at + len;
	*off = at;
	*flight = slot;
	return true;
}

bool
sift_dictshm_write(const DictKey *k, const SiftCDict *d)
{
	Dshm	   *s;
	DshmEnt    *b;
	uint32		flags = (d->blocked ? F_BLOCKED : 0) | (d->sorted != NULL ? F_ORDER : 0) | (d->stored_order && d->sorted != NULL ? F_SORTED : 0) |
		(d->slots != NULL && ((Size) d->n + 1) * 2 <= d->nslots ? F_SLOTS : 0);
	Layout		l = layout(d->n, d->nbytes, flags, d->nslots);
	Size		off;
	DshmHdr		h = {.magic = DSHM_MAGIC, .key = *k, .n = d->n, .nbytes = d->nbytes, .flags = flags, .nslots = (flags & F_SLOTS) ? d->nslots : 0};
	char	   *rec;
	uint32		asks = 0;
	int			slot = -1,
				flight;

	if (l.total < (TEST_MODE(2) ? 0 : DSHM_MIN_BYTES) || d->n == 0 || (s = dshm_get()) == NULL || l.total > s->size / 2)
		return false;
	sift_stat_add(SIFT_STAT_DICT_SHARED_MISSES, 1);
	SpinLockAcquire(&s->mutex);
	b = bucket(s, k);
	for (int i = 0; i < DSHM_WAYS; i++)
		if (b[i].state == E_LIVE && memcmp(&b[i].key, k, sizeof(*k)) == 0 && (((const DshmHdr *) (s->data + b[i].off))->flags & flags) == flags)
		{
			SpinLockRelease(&s->mutex);
			return true;
		}
	if (s->hold > 0 || !ring_take(s, l.total, &off, &flight))
	{
		if (s->hold > 0)
			s->hold--;
		SpinLockRelease(&s->mutex);
		sift_stat_add(SIFT_STAT_DICT_SHARED_HELD, 1);
		return false;
	}
	SpinLockRelease(&s->mutex);

	rec = s->data + off;
	memcpy(rec, &h, sizeof(h));
	memcpy(rec + sizeof(h), d->off, ((Size) d->n + 1) * sizeof(uint32));
	memcpy(rec + l.bytes_at, d->bytes, d->nbytes);
	if (flags & F_SLOTS)
		memcpy(rec + l.slots_at, d->slots, (Size) d->nslots * sizeof(uint64));
	if (flags & F_ORDER)
		memcpy(rec + l.order_at, d->sorted, (Size) d->n * sizeof(uint32));

	b = bucket(s, k);
	SpinLockAcquire(&s->mutex);
	s->flight[flight].len = 0;
	/* a record of the same key is read no more (it goes when nobody reads it); the oldest unpinned goes if all places are taken */
	for (int i = 0; i < DSHM_WAYS; i++)
		if (b[i].state == E_LIVE && memcmp(&b[i].key, k, sizeof(*k)) == 0)
		{
			asks = b[i].asks;
			b[i].state = b[i].pins > 0 ? E_STALE : E_FREE;
		}
	for (int i = 0; i < DSHM_WAYS && slot < 0; i++)
		if (b[i].state == E_FREE)
			slot = i;
	if (slot < 0)
	{
		for (int i = 0; i < DSHM_WAYS; i++)
			if (b[i].pins == 0 && (slot < 0 || b[i].tick < b[slot].tick))
				slot = i;
		s->thrown++;
	}
	if (slot >= 0)
		b[slot] = (DshmEnt) {.state = E_LIVE, .key = *k, .off = off, .len = l.total, .asks = asks, .tick = ++s->tick};
	if (++s->writes >= DSHM_WINDOW)
	{
		if (s->thrown > s->found)
			s->hold = DSHM_HOLD;
		s->writes = s->thrown = s->found = 0;
	}
	SpinLockRelease(&s->mutex);
	return slot >= 0;
}

#ifdef SIFT_TESTING
/* The records some backend reads in place, and the pins on them (all backends) */
void
sift_dictshm_pinned(int64 *records, int64 *pins)
{
	Dshm	   *s = dshm_get();

	*records = *pins = 0;
	if (s == NULL)
		return;
	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < lengthof(s->dir); i++)
		if (s->dir[i].state != E_FREE && s->dir[i].pins > 0)
		{
			(*records)++;
			*pins += s->dir[i].pins;
		}
	SpinLockRelease(&s->mutex);
}

PG_FUNCTION_INFO_V1(sift_test_col_dict_shared_stale);

/* 6,000 strings of 64 bytes that start with c: two such dictionaries have the same record size, and the strings tell them apart */
static SiftCDict *
stale_dict(char c)
{
	SiftCDict  *d = sift_cdict_create(CurrentMemoryContext);

	for (int i = 0; i < 6000; i++)
	{
		char		str[65];

		snprintf(str, sizeof(str), "%c%063d", c, i);
		sift_cdict_intern(d, str, 64);
	}
	return d;
}

/*
 * sift._test_col_dict_shared_stale(): the two ways a reader could be handed the record of another key. A place that a writer is copying into is not given to a
 * second writer (a ring of 2.5 records refuses the third while two are being copied in), and a directory entry that names a place that holds the record of
 * another key (forged here) is a miss for the reader, which gives its pin back. Returns the first letter of the dictionary the reader of the first key gets,
 * NULL when it gets none. The ring is made small for the test and put back, empty.
 */
Datum
sift_test_col_dict_shared_stale(PG_FUNCTION_ARGS)
{
	Dshm	   *s = dshm_get();
	DictKey		ka,
				kc;
	SiftCDict  *dc = stale_dict('c'),
			   *got;
	Size		len = layout(dc->n, dc->nbytes, dc->slots != NULL && ((Size) dc->n + 1) * 2 <= dc->nslots ? F_SLOTS : 0, dc->nslots).total,
				size,
				off[3];
	int			flight[3];
	int64		records,
				npins;
	bool		third,
				wrote;
	char		res[2] = {0, 0};

	if (s == NULL)
		PG_RETURN_NULL();
	memset(&ka, 0, sizeof(ka));
	kc = ka;
	ka.gen = 1;
	kc.gen = 2;
	SpinLockAcquire(&s->mutex);
	size = s->size;
	s->size = len * 5 / 2;
	s->head = 0;
	s->hold = 0;
	for (int i = 0; i < lengthof(s->dir); i++)
		s->dir[i].state = E_FREE;
	ring_take(s, len, &off[0], &flight[0]);
	ring_take(s, len, &off[1], &flight[1]);
	third = ring_take(s, len, &off[2], &flight[2]);
	s->flight[flight[0]].len = s->flight[flight[1]].len = 0;
	s->head = 0;
	SpinLockRelease(&s->mutex);
	SIFT_INVARIANT(!third && off[0] != off[1], "a place being copied into was given to another writer: %zu, %zu", off[0], off[1]);

	wrote = sift_dictshm_write(&kc, dc);
	SIFT_INVARIANT(wrote, "the record of the test was not written");
	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < lengthof(s->dir); i++)
		if (s->dir[i].state == E_LIVE)
		{
			DshmEnt    *b = bucket(s, &ka);

			b[0] = s->dir[i];
			b[0].key = ka;
			break;
		}
	SpinLockRelease(&s->mutex);
	got = sift_dictshm_read(&ka, CurrentMemoryContext);
	if (got != NULL)
		res[0] = got->bytes[got->off[0]];
	else
	{
		sift_dictshm_pinned(&records, &npins);
		SIFT_INVARIANT(npins == 0, "the reader of a stale entry kept its pin: %ld", (long) npins);
	}

	SpinLockAcquire(&s->mutex);
	s->size = size;
	s->head = 0;
	for (int i = 0; i < lengthof(s->dir); i++)
		s->dir[i].state = E_FREE;
	SpinLockRelease(&s->mutex);
	if (got == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(res));
}
#endif
