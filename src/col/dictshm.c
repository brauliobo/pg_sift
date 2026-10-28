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
#include "utils/memutils.h"

#include "agg/stats.h"
#include "col/dictshm.h"
#include "col/dsmreg.h"

#define DSHM_BUCKETS	64
#define DSHM_WAYS		8
#define DSHM_MAGIC		0x44534831
/* the name carries the layout */
#define DSHM_NAME		"sift_col_dict_1"
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
	if (h.magic != DSHM_MAGIC || h.n == 0 || len != record_size(h.n, h.nbytes, (h.flags & F_SORTED) != 0))
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

void
sift_dictshm_write(const DictKey *k, const SiftCDict *d)
{
	Dshm	   *s;
	DshmEnt    *b;
	bool		sorted = d->stored_order && d->sorted != NULL;
	Size		len = record_size(d->n, d->nbytes, sorted),
				off,
				at;
	DshmHdr		h = {.magic = DSHM_MAGIC, .n = d->n, .nbytes = d->nbytes, .flags = (d->blocked ? F_BLOCKED : 0) | (sorted ? F_SORTED : 0)};
	char	   *rec;
	int			slot = -1;

	if (len < (TEST_MODE(2) ? 0 : DSHM_MIN_BYTES) || d->n == 0 || (s = dshm_get()) == NULL || len > s->size / 2)
		return;
	sift_stat_add(SIFT_STAT_DICT_SHARED_MISSES, 1);
	SpinLockAcquire(&s->mutex);
	if (s->hold > 0)
	{
		s->hold--;
		SpinLockRelease(&s->mutex);
		sift_stat_add(SIFT_STAT_DICT_SHARED_HELD, 1);
		return;
	}
	off = s->head + len > s->size ? 0 : s->head;
	s->head = off + len;
	for (int i = 0; i < lengthof(s->dir); i++)
		if (s->dir[i].used && s->dir[i].off < off + len && off < s->dir[i].off + s->dir[i].len)
		{
			s->dir[i].used = false;
			s->thrown++;
		}
	SpinLockRelease(&s->mutex);

	rec = s->data + off;
	at = sizeof(h);
	memcpy(rec + at, d->off, ((Size) d->n + 1) * sizeof(uint32));
	at += ((Size) d->n + 1) * sizeof(uint32);
	memcpy(rec + at, d->bytes, d->nbytes);
	if (sorted)
		memcpy(rec + sorted_at(d->n, d->nbytes), d->sorted, (Size) d->n * sizeof(uint32));
	h.crc = arrays_crc(d->off, d->bytes, sorted ? d->sorted : NULL, d->n, d->nbytes);
	memcpy(rec, &h, sizeof(h));

	b = bucket(s, k);
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
