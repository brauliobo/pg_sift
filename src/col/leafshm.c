/*
 * col/leafshm.c - the statistics of an index, shared by the backends of a server.
 *
 * A backend that plans a statement over a sift_col index for the first time reads the zones of all its units, leaf by leaf (about 36 bytes a unit and
 * leaf, 10,000 units of six leaves: 9 ms and 2,500 buffers; the planning of an index of 63,000 units and some forty leaves is the 100 to 800 ms of a new
 * connection), to fold them into one record for each leaf and a sample of the unit zones (col/am/am.c leaf_cache_entry). The records depend on the segments
 * and the options of the index only, so the first backend that has them keeps them here, under the file of the index and a stamp of its segments (their
 * generations and formats) and options, and the others read them: 0.2 ms of copying. A merge, a rewrite or a REINDEX moves the stamp, and the next backend
 * to plan makes the records again.
 *
 * The table is a ring of bytes (sift.shared_cache_kb, the DSM registry: nothing is allocated at postmaster start) and a directory of LSHM_DIR entries. A
 * writer takes its place in the ring and clears the entries it covers under the write lock, copies outside of it and enters the directory when done; a record
 * carries a checksum, so a reader that copied bytes another writer was overwriting (a ring that wrapped) finds out and plans as if there were no record.
 * Readers take no lock and write nothing shared but their own counters: they copy the bucket of the index between two reads of the sequence number of its stripe,
 * which a writer makes odd while it changes an entry of the stripe, and read again when it moved (a reader that finds it odd waits for the write lock).
 */
#include "postgres.h"

#include "miscadmin.h"
#include "port/atomics.h"
#include "port/pg_crc32c.h"
#include "storage/lwlock.h"

#include "core/cmp.h"
#include "core/shm/budget.h"
#include "col/dsmreg.h"
#include "col/leafshm.h"

/* a place for each index of a partitioned table of a few thousand partitions to keep its record, in buckets of LSHM_WAYS: a lookup scans the bucket of the index */
#define LSHM_DIR		4096
#define LSHM_WAYS		8
#define LSHM_STRIPES	64
#define LSHM_COUNTS		32
/* a record read this many writes ago is read again as new: the oldest entry of a bucket goes when it is full, so a hit writes the tick of its entry once in a while */
#define LSHM_TICK_LAG	64
#define LSHM_MAGIC		0x4C534831
/* the name carries the layout, as for the shared verdicts */
#define LSHM_NAME		"sift_col_leaf_stats_6"
#define LSHM_TRANCHE	"SiftLeafShm"


typedef struct LshmEnt
{
	/* MEM[MEM-SHM-15]: DSM leaf stats dir entry (48 B, was 56; 4096 entries: -32 KB) */
	uint64		stamp;
	Size		off,
				len;
	pg_atomic_uint64 tick;		/* oldest goes when the directory is full */
	RelFileNumber rel;
	Oid			db;
	LshmKind	kind;
	bool		used;
	bool		scope;
}			LshmEnt;
StaticAssertDecl(sizeof(LshmEnt) == 48, "LshmEnt is 48 bytes");

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) LshmStripe
{
	pg_atomic_uint32 seq;		/* odd while a writer changes an entry of the buckets of the stripe */
} LshmStripe;

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) LshmCount
{
	pg_atomic_uint64 hits,		/* reads of a record that found it, and that did not: since the table was made (sift.shared_memory) */
				misses;
} LshmCount;

typedef struct Lshm
{
	LWLockPadded write_lock;	/* the writers: the ring, the order of its records, the entries of the directory */
	int			tranche;
	Size		size;
	Size		head;
	Size		used_bytes;		/* of the records of the directory */
	pg_atomic_uint64 tick;		/* the writes so far */
	int			nord;
	int16		ord[LSHM_DIR];	/* the used entries by the place of their record: no two overlap */
	LshmStripe	stripe[LSHM_STRIPES] pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	LshmCount	count[LSHM_COUNTS];	/* by the process number of the reader: no counter is a line of another reader */
	LshmEnt		dir[LSHM_DIR] pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	char		data[FLEXIBLE_ARRAY_MEMBER];
}			Lshm;

/*
 * A record of the leaves: the header, ncols leaf records, then for each leaf with a sample its column number and the lo and hi arrays. A record of the
 * pre-aggregate counts: the header and the counts. An index has one of each: the counts cost reads of the sections of the units and only the planner of an
 * aggregate asks for them
 */
typedef struct LshmHdr
{
	uint32		magic;
	pg_crc32c	crc;			/* of everything after the header */
	uint32		ncols;
	uint32		nsamples;		/* leaves with arrays */
	uint32		zn;				/* units sampled */
	uint32		kind;
	uint64		zunits;
	uint64		zstride;
}			LshmHdr;

static Lshm *lshm;

static int
lshm_bucket(RelFileNumber rel)
{
	return (int) ((rel * UINT32_C(2654435761)) % (LSHM_DIR / LSHM_WAYS)) * LSHM_WAYS;
}

static pg_atomic_uint32 *
lshm_seq(Lshm *s, int slot)
{
	return &s->stripe[(slot / LSHM_WAYS) % LSHM_STRIPES].seq;
}

static LshmCount *
lshm_count(Lshm *s)
{
	return &s->count[MyProcNumber & (LSHM_COUNTS - 1)];
}

static bool lshm_failed;

static void
lshm_init(SIFT_DSM_INIT_ARGS)
{
	Lshm	   *s = ptr;

	memset(s, 0, offsetof(Lshm, data));
	s->tranche = sift_tranche_new(LSHM_TRANCHE);
	LWLockInitialize(&s->write_lock.lock, s->tranche);
	s->size = sift_shm_pool_bytes(SIFT_SHM_COL_LEAF);
	pg_atomic_init_u64(&s->tick, 0);
	for (int i = 0; i < LSHM_STRIPES; i++)
		pg_atomic_init_u32(&s->stripe[i].seq, 0);
	for (int i = 0; i < LSHM_COUNTS; i++)
	{
		pg_atomic_init_u64(&s->count[i].hits, 0);
		pg_atomic_init_u64(&s->count[i].misses, 0);
	}
	for (int i = 0; i < LSHM_DIR; i++)
		pg_atomic_init_u64(&s->dir[i].tick, 0);
}

static Lshm *
lshm_get(void)
{
	if (sift_shm_pool_bytes(SIFT_SHM_COL_LEAF) == 0 || lshm_failed)
		return NULL;
	if (lshm == NULL)
	{
		lshm = sift_dsm_attach(LSHM_NAME, offsetof(Lshm, data) + sift_shm_pool_bytes(SIFT_SHM_COL_LEAF), lshm_init, &lshm_failed,
							   "the statistics of the column indexes", "sift.shared_cache_kb", false);
		if (lshm != NULL)
			sift_tranche_register(lshm->tranche, LSHM_TRANCHE);
	}
	return lshm;
}

static Size
record_size(LshmKind kind, int ncols, uint32 zn, int nsamples)
{
	if (kind == LSHM_PRE)
		return sizeof(LshmHdr) + sizeof(SiftColPreStats);
	return sizeof(LshmHdr) + ncols * sizeof(SiftColLeafStats) + nsamples * (sizeof(uint64) + 2 * (Size) zn * sizeof(int64));
}

static pg_crc32c
record_crc(const char *rec, Size len)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, rec + sizeof(LshmHdr), len - sizeof(LshmHdr));
	FIN_CRC32C(crc);
	return crc;
}

typedef struct LshmHit
{
	int			slot;
	uint64		stamp;
	Size		off,
				len;
} LshmHit;

/* the entry of the bucket of the index that holds its record of the kind (under the stamp, when there is one), as no writer was changing it; false: none */
static bool
lshm_find(Lshm *s, RelFileNumber rel, bool scope, LshmKind kind, const uint64 *stamp, LshmHit *hit)
{
	const int	first = lshm_bucket(rel);
	pg_atomic_uint32 *seq = lshm_seq(s, first);

	for (;;)
	{
		uint32		before = pg_atomic_read_u32(seq);
		bool		found = false;

		if (before & 1)
		{
			LWLockAcquire(&s->write_lock.lock, LW_SHARED);
			LWLockRelease(&s->write_lock.lock);
			continue;
		}
		pg_read_barrier();
		for (int i = first; i < first + LSHM_WAYS && !found; i++)
		{
			const LshmEnt *e = &s->dir[i];

			if (e->used && e->rel == rel && e->db == MyDatabaseId && e->scope == scope && e->kind == kind && (stamp == NULL || e->stamp == *stamp))
			{
				*hit = (LshmHit) {.slot = i, .stamp = e->stamp, .off = e->off, .len = e->len};
				found = true;
			}
		}
		pg_read_barrier();
		if (pg_atomic_read_u32(seq) == before)
			return found;
	}
}

/* the entry was read: when its tick is old it is made new, so that the oldest of a full bucket is one that nobody reads */
static void
lshm_touch(Lshm *s, int slot)
{
	uint64		now = pg_atomic_read_u64(&s->tick);

	if (now - pg_atomic_read_u64(&s->dir[slot].tick) >= LSHM_TICK_LAG)
		pg_atomic_write_u64(&s->dir[slot].tick, now);
}

bool
sift_col_leafshm_peek(RelFileNumber rel, bool scope, LshmKind kind, uint64 *stamp)
{
	Lshm	   *s = lshm_get();
	LshmHit		hit;

	if (s == NULL || !lshm_find(s, rel, scope, kind, NULL, &hit))
		return false;
	*stamp = hit.stamp;
	return true;
}

/* a copy of the record the ring holds for the index under the stamp, NULL: none */
static char *
lshm_copy(RelFileNumber rel, bool scope, LshmKind kind, uint64 stamp, Size *len)
{
	Lshm	   *s = lshm_get();
	LshmHit		hit;
	char	   *rec;

	if (s == NULL)
		return NULL;
	if (!lshm_find(s, rel, scope, kind, &stamp, &hit) || hit.len < sizeof(LshmHdr))
	{
		pg_atomic_fetch_add_u64(&lshm_count(s)->misses, 1);
		*len = 0;
		return NULL;
	}
	pg_atomic_fetch_add_u64(&lshm_count(s)->hits, 1);
	lshm_touch(s, hit.slot);
	*len = hit.len;
	rec = palloc(hit.len);
	memcpy(rec, s->data + hit.off, hit.len);
	return rec;
}

/* is rec whole: the magic, the kind, the size the header says and the checksum */
static bool
record_whole(const char *rec, Size len, LshmKind kind, const LshmHdr *h)
{
	return h->magic == LSHM_MAGIC && h->kind == kind && h->zn <= SIFT_COL_ZONE_SAMPLE &&
		len == record_size(kind, h->ncols, h->zn, h->nsamples) && h->crc == record_crc(rec, len);
}

bool
sift_col_leafshm_read(RelFileNumber rel, bool scope, uint64 stamp, int ncols, SiftColLeafStats *st, SiftColZones *zs)
{
	Size		len;
	char	   *rec = lshm_copy(rel, scope, LSHM_LEAF, stamp, &len);
	const char *p;
	LshmHdr		h;
	int			nsamples = 0;
	bool		whole = false;

	if (rec == NULL)
		return false;
	memcpy(&h, rec, sizeof(h));
	for (int c = 0; c < zs->ncols; c++)
		nsamples += zs->lo[c] != NULL;
	if (h.ncols == (uint32) ncols && h.nsamples == (uint32) nsamples && record_whole(rec, len, LSHM_LEAF, &h))
	{
		p = rec + sizeof(LshmHdr);
		memcpy(st, p, ncols * sizeof(SiftColLeafStats));
		p += ncols * sizeof(SiftColLeafStats);
		zs->n = h.zn;
		zs->units = h.zunits;
		zs->stride = h.zstride;
		whole = true;
		for (int k = 0; k < nsamples && whole; k++)
		{
			uint64		c;

			memcpy(&c, p, sizeof(c));
			p += sizeof(c);
			if (c >= (uint64) zs->ncols || zs->lo[c] == NULL)
			{
				whole = false;
				break;
			}
			memcpy(zs->lo[c], p, h.zn * sizeof(int64));
			p += h.zn * sizeof(int64);
			memcpy(zs->hi[c], p, h.zn * sizeof(int64));
			p += h.zn * sizeof(int64);
		}
	}
	pfree(rec);
	return whole;
}

bool
sift_col_leafshm_pre_read(RelFileNumber rel, bool scope, uint64 stamp, SiftColPreStats *pre)
{
	Size		len;
	char	   *rec = lshm_copy(rel, scope, LSHM_PRE, stamp, &len);
	LshmHdr		h;
	bool		whole;

	if (rec == NULL)
		return false;
	memcpy(&h, rec, sizeof(h));
	whole = record_whole(rec, len, LSHM_PRE, &h);
	if (whole)
		memcpy(pre, rec + sizeof(LshmHdr), sizeof(*pre));
	pfree(rec);
	return whole;
}

/* The first entry of the index whose record ends after at: those a record at [at, at + len) covers follow it while they begin before at + len */
static int
ord_first(const Lshm *s, Size at)
{
	int			lo = 0,
				hi = s->nord;

	SIFT_BISECT(int, m, lo, hi, s->dir[s->ord[m]].off + s->dir[s->ord[m]].len <= at);
	return lo;
}

/* a change of an entry of the directory, between the two moves of the sequence number of its stripe: the readers that copy the bucket meanwhile read it again */
static void
lshm_change_begin(Lshm *s, int slot)
{
	pg_atomic_uint32 *seq = lshm_seq(s, slot);

	pg_atomic_write_u32(seq, pg_atomic_read_u32(seq) + 1);
	pg_write_barrier();
}

static void
lshm_change_end(Lshm *s, int slot)
{
	pg_atomic_uint32 *seq = lshm_seq(s, slot);

	pg_write_barrier();
	pg_atomic_write_u32(seq, pg_atomic_read_u32(seq) + 1);
}

/* the entry leaves the directory; the order of the ring is the caller's */
static void
lshm_drop(Lshm *s, int slot)
{
	LshmEnt    *e = &s->dir[slot];

	lshm_change_begin(s, slot);
	e->used = false;
	lshm_change_end(s, slot);
	s->used_bytes -= e->len;
}

static void
lshm_free(Lshm *s, int slot)
{
	int			i = ord_first(s, s->dir[slot].off);

	SIFT_INVARIANT(i < s->nord && s->ord[i] == slot, "a record of the shared statistics is not in the order of the ring");
	lshm_drop(s, slot);
	memmove(s->ord + i, s->ord + i + 1, (s->nord-- - i - 1) * sizeof(int16));
}

/* the records that [off, off + len) covers are evicted, and leave the order of the ring in one move: the number of them */
static int
lshm_evict_range(Lshm *s, Size off, Size len)
{
	int			first = ord_first(s, off),
				end = first;

	while (end < s->nord && s->dir[s->ord[end]].off < off + len)
		lshm_drop(s, s->ord[end++]);
	memmove(s->ord + first, s->ord + end, (s->nord - end) * sizeof(int16));
	s->nord -= end - first;
	return end - first;
}

static void
lshm_enter(Lshm *s, int slot, const LshmEnt *e)
{
	LshmEnt    *d = &s->dir[slot];
	int			i;

	lshm_change_begin(s, slot);
	d->stamp = e->stamp;
	d->off = e->off;
	d->len = e->len;
	d->rel = e->rel;
	d->db = e->db;
	d->kind = e->kind;
	d->scope = e->scope;
	d->used = true;
	pg_atomic_write_u64(&d->tick, pg_atomic_read_u64(&s->tick));
	lshm_change_end(s, slot);
	i = ord_first(s, e->off);
	memmove(s->ord + i + 1, s->ord + i, (s->nord++ - i) * sizeof(int16));
	s->ord[i] = (int16) slot;
	s->used_bytes += e->len;
}

/* the records of the index and kind older than the stamp leave the directory: whether one of a newer stamp stays */
static bool
lshm_clear_older(Lshm *s, RelFileNumber rel, bool scope, LshmKind kind, uint64 stamp)
{
	bool		newer = false;

	for (int i = lshm_bucket(rel); i < lshm_bucket(rel) + LSHM_WAYS; i++)
		if (s->dir[i].used && s->dir[i].rel == rel && s->dir[i].db == MyDatabaseId && s->dir[i].scope == scope && s->dir[i].kind == kind)
		{
			if (s->dir[i].stamp > stamp)
				newer = true;
			else
				lshm_free(s, i);
		}
	return newer;
}

/* a free place in the bucket of the index, the oldest of the bucket if all are taken */
static int
lshm_place(Lshm *s, RelFileNumber rel)
{
	int			slot = lshm_bucket(rel);

	for (int i = slot; i < lshm_bucket(rel) + LSHM_WAYS; i++)
		if (!s->dir[i].used || (s->dir[slot].used && pg_atomic_read_u64(&s->dir[i].tick) < pg_atomic_read_u64(&s->dir[slot].tick)))
			slot = i;
	return slot;
}

/*
 * enters rec (its header not yet filled in) into the ring as the record of the index under the stamp; it replaces the older record of the same kind and those it overlaps.
 * A writer that entered the directory while this one copied its record took the place this one chose or holds a record of the index as well: the directory is looked at again before the entry
 * goes in, and a record of a newer stamp stays in place of this one
 */
static void
lshm_put(RelFileNumber rel, bool scope, uint64 stamp, char *rec, Size len, LshmHdr h)
{
	Lshm	   *s = lshm_get();
	Size		off;
	int			slot,
				evicted;

	if (s == NULL || len > s->size)
		return;
	h.magic = LSHM_MAGIC;
	h.crc = record_crc(rec, len);
	memcpy(rec, &h, sizeof(h));

	LWLockAcquire(&s->write_lock.lock, LW_EXCLUSIVE);
	off = s->head + len > s->size ? 0 : s->head;
	s->head = off + len;
	pg_atomic_write_u64(&s->tick, pg_atomic_read_u64(&s->tick) + 1);
	/* the record of the same index and kind (an older stamp) is replaced: it is in the bucket of the index; those the new one covers are evicted */
	(void) lshm_clear_older(s, rel, scope, h.kind, PG_UINT64_MAX);
	evicted = lshm_evict_range(s, off, len);
	evicted += s->dir[lshm_place(s, rel)].used;
	LWLockRelease(&s->write_lock.lock);
	sift_shm_note_eviction(SIFT_SHM_COL_LEAF, evicted);
	memcpy(s->data + off, rec, len);
	LWLockAcquire(&s->write_lock.lock, LW_EXCLUSIVE);
	(void) lshm_evict_range(s, off, len);
	if (!lshm_clear_older(s, rel, scope, h.kind, stamp))
	{
		slot = lshm_place(s, rel);
		if (s->dir[slot].used)
			lshm_free(s, slot);
		lshm_enter(s, slot, &(LshmEnt) {.rel = rel, .db = MyDatabaseId, .scope = scope, .kind = h.kind, .stamp = stamp, .off = off, .len = len});
	}
	LWLockRelease(&s->write_lock.lock);
}

void
sift_col_leafshm_write(RelFileNumber rel, bool scope, uint64 stamp, int ncols, const SiftColLeafStats *st, const SiftColZones *zs)
{
	int			nsamples = 0;
	Size		len;
	char	   *rec,
			   *p;

	for (int c = 0; c < zs->ncols; c++)
		nsamples += zs->lo[c] != NULL;
	len = record_size(LSHM_LEAF, ncols, zs->n, nsamples);
	rec = palloc(len);
	p = rec + sizeof(LshmHdr);
	memcpy(p, st, ncols * sizeof(SiftColLeafStats));
	p += ncols * sizeof(SiftColLeafStats);
	for (int c = 0; c < zs->ncols; c++)
		if (zs->lo[c] != NULL)
		{
			uint64		cc = c;

			memcpy(p, &cc, sizeof(cc));
			p += sizeof(cc);
			memcpy(p, zs->lo[c], zs->n * sizeof(int64));
			p += zs->n * sizeof(int64);
			memcpy(p, zs->hi[c], zs->n * sizeof(int64));
			p += zs->n * sizeof(int64);
		}
	lshm_put(rel, scope, stamp, rec, len, (LshmHdr) {.kind = LSHM_LEAF, .ncols = ncols, .nsamples = nsamples, .zn = zs->n, .zunits = zs->units, .zstride = zs->stride});
	pfree(rec);
}

void
sift_col_leafshm_pre_write(RelFileNumber rel, bool scope, uint64 stamp, const SiftColPreStats *pre)
{
	Size		len = record_size(LSHM_PRE, 0, 0, 0);
	char	   *rec = palloc(len);

	memcpy(rec + sizeof(LshmHdr), pre, sizeof(*pre));
	lshm_put(rel, scope, stamp, rec, len, (LshmHdr) {.kind = LSHM_PRE});
	pfree(rec);
}

/* sift.shared_memory(): the bytes of the records the ring holds and the reads that found one and that did not */
bool
sift_col_leafshm_stat(SiftShmStat *st)
{
	Lshm	   *s = sift_dsm_find(LSHM_NAME, NULL);

	if (s == NULL)
		return false;
	sift_tranche_register(s->tranche, LSHM_TRANCHE);
	LWLockAcquire(&s->write_lock.lock, LW_SHARED);
	st->used += s->used_bytes;
	LWLockRelease(&s->write_lock.lock);
	st->hits = st->misses = 0;
	for (int i = 0; i < LSHM_COUNTS; i++)
	{
		st->hits += pg_atomic_read_u64(&s->count[i].hits);
		st->misses += pg_atomic_read_u64(&s->count[i].misses);
	}
	st->has_used = st->has_counts = true;
	return true;
}
