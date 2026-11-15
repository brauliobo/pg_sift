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
 * The table is a ring of bytes (sift.col_leaf_cache_kb, the DSM registry: nothing is allocated at postmaster start) and a directory of LSHM_DIR entries. A
 * writer takes its place in the ring and clears the entries it covers under the spinlock, copies outside of it and enters the directory when done; a record
 * carries a checksum, so a reader that copied bytes another writer was overwriting (a ring that wrapped) finds out and plans as if there were no record.
 */
#include "postgres.h"

#include "port/pg_crc32c.h"
#include "storage/spin.h"

#include "col/dsmreg.h"
#include "col/leafshm.h"

/* a place for each index of a partitioned table of a few thousand partitions to keep its record, in buckets of LSHM_WAYS: a lookup scans the bucket of the index under the spinlock */
#define LSHM_DIR		4096
#define LSHM_WAYS		8
#define LSHM_MAGIC		0x4C534831
/* the name carries the layout, as for the shared verdicts */
#define LSHM_NAME		"sift_col_leaf_stats_5"

int			sift_col_leaf_cache_kb = 16384;

typedef struct LshmEnt
{
	bool		used;
	RelFileNumber rel;
	Oid			db;
	LshmKind	kind;
	uint64		stamp;
	Size		off,
				len;
	uint64		tick;			/* oldest goes when the directory is full */
}			LshmEnt;

typedef struct Lshm
{
	slock_t		mutex;
	uint64		tick;
	Size		size;
	Size		head;
	LshmEnt		dir[LSHM_DIR];
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

static bool lshm_failed;

static void
lshm_init(SIFT_DSM_INIT_ARGS)
{
	Lshm	   *s = ptr;

	memset(s, 0, offsetof(Lshm, data));
	SpinLockInit(&s->mutex);
	s->size = (Size) sift_col_leaf_cache_kb * 1024;
}

static Lshm *
lshm_get(void)
{
	if (sift_col_leaf_cache_kb <= 0 || lshm_failed)
		return NULL;
	if (lshm == NULL)
		lshm = sift_dsm_attach(LSHM_NAME, offsetof(Lshm, data) + (Size) sift_col_leaf_cache_kb * 1024, lshm_init, &lshm_failed,
							   "the statistics of the column indexes", "sift.col_leaf_cache_kb", false);
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

bool
sift_col_leafshm_peek(RelFileNumber rel, LshmKind kind, uint64 *stamp)
{
	Lshm	   *s = lshm_get();
	bool		found = false;

	if (s == NULL)
		return false;
	SpinLockAcquire(&s->mutex);
	for (int i = lshm_bucket(rel); i < lshm_bucket(rel) + LSHM_WAYS; i++)
		if (s->dir[i].used && s->dir[i].rel == rel && s->dir[i].db == MyDatabaseId && s->dir[i].kind == kind)
		{
			*stamp = s->dir[i].stamp;
			found = true;
			break;
		}
	SpinLockRelease(&s->mutex);
	return found;
}

/* a copy of the record the ring holds for the index under the stamp, NULL: none */
static char *
lshm_copy(RelFileNumber rel, LshmKind kind, uint64 stamp, Size *len)
{
	Lshm	   *s = lshm_get();
	Size		off = 0;
	char	   *rec;

	if (s == NULL)
		return NULL;
	*len = 0;
	SpinLockAcquire(&s->mutex);
	for (int i = lshm_bucket(rel); i < lshm_bucket(rel) + LSHM_WAYS; i++)
		if (s->dir[i].used && s->dir[i].rel == rel && s->dir[i].db == MyDatabaseId && s->dir[i].kind == kind && s->dir[i].stamp == stamp)
		{
			off = s->dir[i].off;
			*len = s->dir[i].len;
			s->dir[i].tick = ++s->tick;
			break;
		}
	SpinLockRelease(&s->mutex);
	if (*len < sizeof(LshmHdr))
		return NULL;
	rec = palloc(*len);
	memcpy(rec, s->data + off, *len);
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
sift_col_leafshm_read(RelFileNumber rel, uint64 stamp, int ncols, SiftColLeafStats *st, SiftColZones *zs)
{
	Size		len;
	char	   *rec = lshm_copy(rel, LSHM_LEAF, stamp, &len);
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
sift_col_leafshm_pre_read(RelFileNumber rel, uint64 stamp, SiftColPreStats *pre)
{
	Size		len;
	char	   *rec = lshm_copy(rel, LSHM_PRE, stamp, &len);
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

/* enters rec (its header not yet filled in) into the ring as the record of the index under the stamp; it replaces the older record of the same kind and those it overlaps */
static void
lshm_put(RelFileNumber rel, uint64 stamp, char *rec, Size len, LshmHdr h)
{
	Lshm	   *s = lshm_get();
	Size		off;
	int			slot;

	if (s == NULL || len > s->size)
		return;
	h.magic = LSHM_MAGIC;
	h.crc = record_crc(rec, len);
	memcpy(rec, &h, sizeof(h));

	SpinLockAcquire(&s->mutex);
	off = s->head + len > s->size ? 0 : s->head;
	s->head = off + len;
	for (int i = 0; i < LSHM_DIR; i++)
	{
		LshmEnt    *e = &s->dir[i];

		/* the record of the same index and kind (an older stamp), and those the new one covers, go */
		if (e->used && ((e->rel == rel && e->db == MyDatabaseId && e->kind == h.kind) || (e->off < off + len && off < e->off + e->len)))
			e->used = false;
	}
	/* a free place in the bucket of the index, the oldest of the bucket if all are taken */
	slot = lshm_bucket(rel);
	for (int i = slot; i < lshm_bucket(rel) + LSHM_WAYS; i++)
		if (!s->dir[i].used || (s->dir[slot].used && s->dir[i].tick < s->dir[slot].tick))
			slot = i;
	SpinLockRelease(&s->mutex);
	memcpy(s->data + off, rec, len);
	SpinLockAcquire(&s->mutex);
	s->dir[slot] = (LshmEnt) {.used = true, .rel = rel, .db = MyDatabaseId, .kind = h.kind, .stamp = stamp, .off = off, .len = len, .tick = ++s->tick};
	SpinLockRelease(&s->mutex);
}

void
sift_col_leafshm_write(RelFileNumber rel, uint64 stamp, int ncols, const SiftColLeafStats *st, const SiftColZones *zs)
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
	lshm_put(rel, stamp, rec, len, (LshmHdr) {.kind = LSHM_LEAF, .ncols = ncols, .nsamples = nsamples, .zn = zs->n, .zunits = zs->units, .zstride = zs->stride});
	pfree(rec);
}

void
sift_col_leafshm_pre_write(RelFileNumber rel, uint64 stamp, const SiftColPreStats *pre)
{
	Size		len = record_size(LSHM_PRE, 0, 0, 0);
	char	   *rec = palloc(len);

	memcpy(rec + sizeof(LshmHdr), pre, sizeof(*pre));
	lshm_put(rel, stamp, rec, len, (LshmHdr) {.kind = LSHM_PRE});
	pfree(rec);
}
