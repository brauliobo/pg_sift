/*
 * col/dictshm.c - the parsed dictionaries of segments, shared by the backends of a server.
 *
 * A backend that meets a dictionary inflates and indexes all of it (the strings of a million-row ip leaf: 170 million instructions), and col/dictcache.c keeps the
 * result for its own life, so a fresh backend, a pooled connection or a parallel worker does it again. The first backend to parse one keeps its offsets and strings
 * here, under the key of its own cache (the file, generation, rows and header block of the segment, the leaf: a segment never changes, a merge or a rewrite names a
 * new one), and the others copy them out: a memcpy and a checksum. What a lookup builds from them (hash slots, sorted order) stays lazy in each backend.
 *
 * As col/leafshm.c: a ring of bytes in the DSM registry (no shared_preload_libraries), a directory under a spinlock, the copy outside of it and a checksum that tells a
 * reader the ring wrapped over what it copied; where the table cannot be had the backend goes without it. The directory is buckets of DSHM_WAYS entries found by the
 * hash of the key. A writer that finds more entries thrown out than found in DSHM_WINDOW writes stops for DSHM_HOLD attempts: a ring too small for the dictionaries
 * asked does not churn.
 */
#include "postgres.h"

#include <sys/statvfs.h>

#include "common/hashfn.h"
#include "port/pg_crc32c.h"
#include "storage/bufmgr.h"
#include "storage/dsm_impl.h"
#include "storage/spin.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "agg/stats.h"
#include "core/host.h"
#include "col/dictshm.h"
#include "col/dsmreg.h"

#define DSHM_BUCKETS	64
#define DSHM_WAYS		8
#define DSHM_MAGIC		0x44534831
/* the name carries the layout */
#define DSHM_NAME		"sift_col_dict_2"
/* the smallest record worth a copy: under it the segment is as cheap to read */
#define DSHM_MIN_BYTES	(128 * 1024)
#define DSHM_WINDOW		16
#define DSHM_HOLD		64

#ifdef SIFT_TESTING
int			sift_test_col_dict_shared = 0;
#define TEST_MODE(m)	(sift_test_col_dict_shared == (m))
#else
#define TEST_MODE(m)	false
#endif

typedef struct DshmEnt
{
	bool		used;
	DictKey		key;
	Size		off,
				len;
	uint64		tick;			/* oldest of a bucket goes when it is full */
}			DshmEnt;

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
	DshmEnt		dir[DSHM_BUCKETS * DSHM_WAYS];
	char		data[FLEXIBLE_ARRAY_MEMBER];
}			Dshm;

/* a record: the header, the offsets (n + 1), the strings, then the codes in byte order (n) when the flag says so, 4-aligned */
typedef struct DshmHdr
{
	uint32		magic;
	pg_crc32c	crc;			/* of the arrays that follow, not of the padding */
	DictKey		key;			/* whose record it is: the directory may name a place that another record took */
	uint32		n;
	uint32		nbytes;
	uint32		flags;
	uint32		pad;
}			DshmHdr;

#define F_BLOCKED	1
#define F_SORTED	2

static Dshm *dshm;
static bool dshm_failed;

static Size
ring_bytes(void)
{
	Size		bytes = Max((Size) 64 << 20, Min((Size) 512 << 20, (Size) NBuffers * BLCKSZ / 4));
	struct statvfs sv;

	/* the segments of parallel queries live in the same /dev/shm: the ring never takes more than a quarter of it (a container has 64 MB) */
	if (dynamic_shared_memory_type == DSM_IMPL_POSIX && statvfs("/dev/shm", &sv) == 0)
		bytes = Max((Size) 4 << 20, Min(bytes, (Size) sv.f_blocks * sv.f_frsize / 4));
	return bytes;
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

static Size
sorted_at(uint32 n, uint32 nbytes)
{
	return sizeof(DshmHdr) + ((Size) n + 1) * sizeof(uint32) + TYPEALIGN(sizeof(uint32), nbytes);
}

static Size
record_size(uint32 n, uint32 nbytes, bool sorted)
{
	return sorted_at(n, nbytes) + (sorted ? (Size) n * sizeof(uint32) : 0);
}

static pg_crc32c
arrays_crc(const uint32 *off, const char *bytes, const uint32 *sorted, uint32 n, uint32 nbytes)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, off, ((Size) n + 1) * sizeof(uint32));
	COMP_CRC32C(crc, bytes, nbytes);
	if (sorted)
		COMP_CRC32C(crc, sorted, (Size) n * sizeof(uint32));
	FIN_CRC32C(crc);
	return crc;
}

static DshmEnt *
bucket(Dshm *s, const DictKey *k)
{
	return &s->dir[(hash_bytes((const unsigned char *) k, sizeof(*k)) % DSHM_BUCKETS) * DSHM_WAYS];
}

/* Where the record of k lies in the ring (len 0: none), as its place in the directory is read under the spinlock */
static Size
dir_find(Dshm *s, const DictKey *k, Size *off)
{
	DshmEnt    *b = bucket(s, k);
	Size		len = 0;

	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < DSHM_WAYS; i++)
		if (b[i].used && memcmp(&b[i].key, k, sizeof(*k)) == 0)
		{
			*off = b[i].off;
			len = b[i].len;
			b[i].tick = ++s->tick;
			s->found++;
			break;
		}
	SpinLockRelease(&s->mutex);
	return len;
}

bool
sift_dictshm_has(const DictKey *k)
{
	Dshm	   *s = dshm_get();
	Size		off;

	return s != NULL && dir_find(s, k, &off) > 0;
}

SiftCDict *
sift_dictshm_read(const DictKey *k, MemoryContext mcxt)
{
	Dshm	   *s = dshm_get();
	Size		off = 0,
				len;
	DshmHdr		h;
	SiftCDict  *d;
	const char *rec;
	uint32	   *sorted = NULL;

	if (s == NULL)
		return NULL;
	len = dir_find(s, k, &off);
	if (len < sizeof(h))
		return NULL;
	rec = s->data + off;
	memcpy(&h, rec, sizeof(h));
	if (h.magic != DSHM_MAGIC || memcmp(&h.key, k, sizeof(*k)) != 0 || h.n == 0 || len != record_size(h.n, h.nbytes, (h.flags & F_SORTED) != 0))
		return NULL;
	d = sift_cdict_plain_begin(mcxt, h.n);
	memcpy(d->off, rec + sizeof(h), ((Size) h.n + 1) * sizeof(uint32));
	/* the offsets may be torn by a writer: nothing is allocated from them before they say what the header does */
	if (d->off[0] == 0 && d->off[h.n] == h.nbytes && sift_cdict_plain_bytes(d, mcxt, h.nbytes))
	{
		memcpy(d->bytes, rec + sizeof(h) + ((Size) h.n + 1) * sizeof(uint32), h.nbytes);
		if (h.flags & F_SORTED)
		{
			sorted = MemoryContextAlloc(mcxt, (Size) h.n * sizeof(uint32));
			memcpy(sorted, rec + sorted_at(h.n, h.nbytes), (Size) h.n * sizeof(uint32));
		}
		if (h.crc == arrays_crc(d->off, d->bytes, sorted, h.n, h.nbytes))
		{
			d->blocked = (h.flags & F_BLOCKED) != 0;
			d->stored_order = sorted != NULL;
			d->sorted = sorted;
			sift_stat_add(SIFT_STAT_DICT_SHARED_HITS, 1);
			return d;
		}
	}
	/* the ring wrapped over the record while it was copied: parsed from the segment as if there were none */
	if (sorted)
		pfree(sorted);
	pfree(d->off);
	pfree(d->bytes);
	pfree(d);
	return NULL;
}

/* A place in the ring for a record of len bytes, the entries it overlaps thrown out; false when the writes are on hold */
static bool
ring_reserve(Dshm *s, Size len, Size *off)
{
	SpinLockAcquire(&s->mutex);
	if (s->hold > 0)
	{
		s->hold--;
		SpinLockRelease(&s->mutex);
		sift_stat_add(SIFT_STAT_DICT_SHARED_HELD, 1);
		return false;
	}
	*off = s->head + len > s->size ? 0 : s->head;
	s->head = *off + len;
	for (int i = 0; i < lengthof(s->dir); i++)
		if (s->dir[i].used && s->dir[i].off < *off + len && *off < s->dir[i].off + s->dir[i].len)
		{
			s->dir[i].used = false;
			s->thrown++;
		}
	SpinLockRelease(&s->mutex);
	return true;
}

static void
ring_fill(Dshm *s, const DictKey *k, const SiftCDict *d, Size off)
{
	bool		sorted = d->stored_order && d->sorted != NULL;
	DshmHdr		h = {.magic = DSHM_MAGIC, .key = *k, .n = d->n, .nbytes = d->nbytes, .flags = (d->blocked ? F_BLOCKED : 0) | (sorted ? F_SORTED : 0)};
	char	   *rec = s->data + off;
	Size		at = sizeof(h);

	memcpy(rec + at, d->off, ((Size) d->n + 1) * sizeof(uint32));
	at += ((Size) d->n + 1) * sizeof(uint32);
	memcpy(rec + at, d->bytes, d->nbytes);
	if (sorted)
		memcpy(rec + sorted_at(d->n, d->nbytes), d->sorted, (Size) d->n * sizeof(uint32));
	h.crc = arrays_crc(d->off, d->bytes, sorted ? d->sorted : NULL, d->n, d->nbytes);
	memcpy(rec, &h, sizeof(h));
}

/*
 * The record is there for the others. Another writer may have been given the same place meanwhile (the ring wrapped while this one copied: the entry it
 * overlapped was not there yet to be thrown out), which is why a record names its key and the reader compares it.
 */
static void
ring_publish(Dshm *s, const DictKey *k, Size off, Size len)
{
	DshmEnt    *b = bucket(s, k);
	int			slot = -1;

	SpinLockAcquire(&s->mutex);
	/* the same key goes (a record that did not check); the oldest goes if all places are taken */
	for (int i = 0; i < DSHM_WAYS; i++)
		if (b[i].used && memcmp(&b[i].key, k, sizeof(*k)) == 0)
			b[i].used = false;
	for (int i = 0; i < DSHM_WAYS && slot < 0; i++)
		if (!b[i].used)
			slot = i;
	if (slot < 0)
	{
		slot = 0;
		for (int i = 1; i < DSHM_WAYS; i++)
			if (b[i].tick < b[slot].tick)
				slot = i;
		s->thrown++;
	}
	b[slot] = (DshmEnt) {.used = true, .key = *k, .off = off, .len = len, .tick = ++s->tick};
	if (++s->writes >= DSHM_WINDOW)
	{
		if (s->thrown > s->found)
			s->hold = DSHM_HOLD;
		s->writes = s->thrown = s->found = 0;
	}
	SpinLockRelease(&s->mutex);
}

void
sift_dictshm_write(const DictKey *k, const SiftCDict *d)
{
	Dshm	   *s;
	Size		len = record_size(d->n, d->nbytes, d->stored_order && d->sorted != NULL),
				off;

	if (len < (TEST_MODE(2) ? 0 : DSHM_MIN_BYTES) || d->n == 0 || (s = dshm_get()) == NULL || len > s->size / 2)
		return;
	sift_stat_add(SIFT_STAT_DICT_SHARED_MISSES, 1);
	if (!ring_reserve(s, len, &off))
		return;
	ring_fill(s, k, d, off);
	ring_publish(s, k, off, len);
}

#ifdef SIFT_TESTING
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
 * sift._test_col_dict_shared_stale(): a writer that copied its record and was not scheduled again before it published, while the ring wrapped over the
 * place it had been given and another writer, whose dictionary has the size of its own, filled that place and published. The first one is published now,
 * for a place that holds the record of the other: the table must not hand the dictionary of the other to a reader of the first one. Returns the first
 * letter of the dictionary the reader of the first key gets, NULL when it gets none. The ring is made small for the test and put back, empty.
 */
Datum
sift_test_col_dict_shared_stale(PG_FUNCTION_ARGS)
{
	Dshm	   *s = dshm_get();
	DictKey		ka,
				kc;
	SiftCDict  *da = stale_dict('a'),
			   *dc = stale_dict('c'),
			   *got;
	Size		len = record_size(da->n, da->nbytes, false),
				size,
				offa,
				offb,
				offc;
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
		s->dir[i].used = false;
	SpinLockRelease(&s->mutex);

	ring_reserve(s, len, &offa);
	ring_fill(s, &ka, da, offa);
	ring_reserve(s, len, &offb);
	ring_reserve(s, len, &offc);
	ring_fill(s, &kc, dc, offc);
	ring_publish(s, &kc, offc, len);
	ring_publish(s, &ka, offa, len);
	SIFT_INVARIANT(offa == offc && offb != offa, "the test ring did not wrap: %zu, %zu, %zu", offa, offb, offc);
	got = sift_dictshm_read(&ka, CurrentMemoryContext);
	if (got != NULL)
		res[0] = got->bytes[got->off[0]];

	SpinLockAcquire(&s->mutex);
	s->size = size;
	s->head = 0;
	for (int i = 0; i < lengthof(s->dir); i++)
		s->dir[i].used = false;
	SpinLockRelease(&s->mutex);
	if (got == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(res));
}
#endif
