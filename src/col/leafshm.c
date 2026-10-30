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
#include "utils/memutils.h"

#include "col/dsmreg.h"
#include "col/leafshm.h"

#define LSHM_DIR		16
#define LSHM_MAGIC		0x4C534831
/* the name carries the layout, as for the shared verdicts */
#define LSHM_NAME		"sift_col_leaf_stats_1"

int			sift_col_leaf_cache_kb = 4096;

typedef struct LshmEnt
{
	bool		used;
	RelFileNumber rel;
	Oid			db;
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

/* a record: the header, ncols leaf records, the pre-aggregate counts, then for each leaf with a sample its column number and the lo and hi arrays */
typedef struct LshmHdr
{
	uint32		magic;
	pg_crc32c	crc;			/* of everything after the header */
	uint32		ncols;
	uint32		nsamples;		/* leaves with arrays */
	uint32		zn;				/* units sampled */
	uint32		pad;
	uint64		zunits;
	uint64		zstride;
}			LshmHdr;

static Lshm *lshm;
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
record_size(int ncols, uint32 zn, int nsamples)
{
	return sizeof(LshmHdr) + ncols * sizeof(SiftColLeafStats) + sizeof(SiftColPreStats) + nsamples * (sizeof(uint64) + 2 * (Size) zn * sizeof(int64));
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
sift_col_leafshm_read(RelFileNumber rel, uint64 stamp, int ncols, SiftColLeafStats *st, SiftColPreStats *pre, SiftColZones *zs)
{
	Lshm	   *s = lshm_get();
	Size		off = 0,
				len = 0;
	char	   *rec;
	const char *p;
	LshmHdr		h;
	int			nsamples = 0;
	bool		whole = false;

	if (s == NULL)
		return false;
	SpinLockAcquire(&s->mutex);
	for (int i = 0; i < LSHM_DIR; i++)
		if (s->dir[i].used && s->dir[i].rel == rel && s->dir[i].db == MyDatabaseId && s->dir[i].stamp == stamp)
		{
			off = s->dir[i].off;
			len = s->dir[i].len;
			s->dir[i].tick = ++s->tick;
			break;
		}
	SpinLockRelease(&s->mutex);
	if (len < sizeof(LshmHdr))
		return false;
	rec = palloc(len);
	memcpy(rec, s->data + off, len);
	memcpy(&h, rec, sizeof(h));
	for (int c = 0; c < zs->ncols; c++)
		nsamples += zs->lo[c] != NULL;
	if (h.magic == LSHM_MAGIC && h.ncols == (uint32) ncols && h.nsamples == (uint32) nsamples && h.zn <= SIFT_COL_ZONE_SAMPLE &&
		len == record_size(ncols, h.zn, nsamples) && h.crc == record_crc(rec, len))
	{
		p = rec + sizeof(LshmHdr);
		memcpy(st, p, ncols * sizeof(SiftColLeafStats));
		p += ncols * sizeof(SiftColLeafStats);
		memcpy(pre, p, sizeof(SiftColPreStats));
		p += sizeof(SiftColPreStats);
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

void
sift_col_leafshm_write(RelFileNumber rel, uint64 stamp, int ncols, const SiftColLeafStats *st, const SiftColPreStats *pre, const SiftColZones *zs)
{
	Lshm	   *s = lshm_get();
	int			nsamples = 0;
	Size		len,
				off;
	char	   *rec,
			   *p;
	LshmHdr		h = {.magic = LSHM_MAGIC, .ncols = ncols, .zn = zs->n, .zunits = zs->units, .zstride = zs->stride};
	int			slot = -1;

	if (s == NULL)
		return;
	for (int c = 0; c < zs->ncols; c++)
		nsamples += zs->lo[c] != NULL;
	h.nsamples = nsamples;
	len = record_size(ncols, zs->n, nsamples);
	if (len > s->size)
		return;
	rec = palloc(len);
	p = rec + sizeof(LshmHdr);
	memcpy(p, st, ncols * sizeof(SiftColLeafStats));
	p += ncols * sizeof(SiftColLeafStats);
	memcpy(p, pre, sizeof(SiftColPreStats));
	p += sizeof(SiftColPreStats);
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
	h.crc = record_crc(rec, len);
	memcpy(rec, &h, sizeof(h));

	SpinLockAcquire(&s->mutex);
	off = s->head + len > s->size ? 0 : s->head;
	s->head = off + len;
	for (int i = 0; i < LSHM_DIR; i++)
	{
		LshmEnt    *e = &s->dir[i];

		/* the record of the same index (an older stamp), and those the new one covers, go; the oldest goes if all places are taken */
		if (e->used && ((e->rel == rel && e->db == MyDatabaseId) || (e->off < off + len && off < e->off + e->len)))
			e->used = false;
		if (slot < 0 && !e->used)
			slot = i;
	}
	if (slot < 0)
	{
		slot = 0;
		for (int i = 1; i < LSHM_DIR; i++)
			if (s->dir[i].tick < s->dir[slot].tick)
				slot = i;
	}
	SpinLockRelease(&s->mutex);
	memcpy(s->data + off, rec, len);
	SpinLockAcquire(&s->mutex);
	s->dir[slot] = (LshmEnt) {.used = true, .rel = rel, .db = MyDatabaseId, .stamp = stamp, .off = off, .len = len, .tick = ++s->tick};
	SpinLockRelease(&s->mutex);
	pfree(rec);
}
