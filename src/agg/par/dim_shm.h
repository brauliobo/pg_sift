/*
 * agg/par/dim_shm.h - one dimension table in shared memory for every participant of a parallel join, built by all of them and read by
 * all of them with no lock: a participant that builds its own copy costs the scan, the build and the memory of the table once more
 * for each of them.
 *
 * The table is a hash of int64 keys laid out by bucket, in three arrays: the entries (key, row) sorted by bucket, the start of each
 * bucket, and no pointer anywhere, so it is valid at any address of any process. It is built in five phases of a barrier, each by
 * every participant that is attached, claiming its units from a cursor:
 *   count    the rows, in granules of SIFT_DIM_BLOCK_ROWS from a morsel queue;
 *   scan     blocks of SIFT_DIM_SCAN_BUCKETS buckets: the running sum of the counts within the block, and the sum of the block;
 *   bases    the participant the barrier elects: the sums of the blocks turned into the sum of the blocks before them (a few words);
 *   add      the blocks again: the base added, so that the count of a bucket is where its entries start;
 *   fill     the rows again: each row is placed in the entries of its bucket.
 * A table of at most SIFT_DIM_HIST_BUCKETS buckets (the dimensions of a star schema) is built with no atomic operation on a word that
 * another participant writes: a participant counts into a histogram of its own (a row of a matrix of participants by buckets), the scan
 * gives each participant the start of its rows in each bucket (after those of the participants before it), and the fill is its own
 * granules, which it kept, written to places no other participant writes. A bigger table counts and places with one atomic add on the
 * bucket for each run of equal buckets in a granule, so that a skewed key costs one add for the rows of the granule, not one for each.
 * A participant that attaches late does the phases that are left and skips the others. After the last the table is never written:
 * a lookup is two loads of the starts and a scan of the entries of the bucket, which hold every row of the key (a key may be in
 * many rows of the dimension).
 *
 * The keys of the build side may be given to a join filter (agg/sip.h) in the same pass: the hash that picks the bucket of a key is the hash
 * of the filter's Bloom blocks, so the filter costs one more insert in the count phase, no more scan and no more hash.
 *
 * The keys come from the caller in blocks, so that the dimension is read where it is: a heap scan, a column, an array.
 */
#ifndef SIFT_AGG_PAR_DIM_SHM_H
#define SIFT_AGG_PAR_DIM_SHM_H

#include "agg/kern6.h"
#include "core/par/morsel.h"
#include "agg/sip.h"
#include "port/atomics.h"
#include "storage/barrier.h"

#define SIFT_DIM_SHM_MAGIC		0x44494D36U
/* rows of a granule of the count and fill phases */
#define SIFT_DIM_BLOCK_ROWS		1024
/* buckets of a block of the scan and add phases */
#define SIFT_DIM_SCAN_BUCKETS	4096
/* the most buckets of a table built from histograms of the participants (a matrix of participants by buckets of 4 bytes) */
#define SIFT_DIM_HIST_BUCKETS	65536

typedef struct SiftDimEntry
{
	int64		key;
	uint32		row;
	uint32		pad;
} SiftDimEntry;

/* Writes the keys of the rows [first, first + n) of the dimension to keys */
typedef void (*SiftDimKeys) (void *arg, uint32 first, uint32 n, int64 *keys);

typedef struct SiftDimShm
{
	uint32		magic;
	uint32		nrows;
	uint32		nbuckets;		/* a power of 2, at least nrows */
	uint32		n_blocks;		/* of the scan and add phases */
	uint32		n_slots;		/* the participants of the histograms: 0 for a table that counts with atomic adds */
	Size		queue_off[2];	/* the morsel queues of the count phase and, with atomic adds, of the fill phase */
	Size		totals_off;		/* uint32[n_blocks] */
	Size		counts_off;		/* pg_atomic_uint32[nbuckets + 1]: the last is nrows */
	Size		entries_off;	/* SiftDimEntry[nrows] */
	Size		hist_off;		/* uint32[n_slots][nbuckets] */
	Size		total_bytes;
	Barrier		barrier pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint32 scan_cursor pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint32 add_cursor pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint32 next_slot pg_attribute_aligned(PG_CACHE_LINE_SIZE);
} pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftDimShm;

StaticAssertDecl(sizeof(SiftDimShm) % PG_CACHE_LINE_SIZE == 0 && offsetof(SiftDimShm, scan_cursor) % PG_CACHE_LINE_SIZE == 0 &&
				 offsetof(SiftDimShm, next_slot) % PG_CACHE_LINE_SIZE == 0, "the hot words of a shared dimension are on lines of their own");

/* The bytes to give sift_dim_shm_init for a dimension of nrows rows built by n_participants participants (leader and workers), whatever the alignment of what it gets */
extern Size sift_dim_shm_size(uint32 nrows, uint32 n_participants);
/* The bytes that hold the table of any number of rows up to max_rows (a dimension that is reduced by another before it is built: the rows that stay are known after the space is) */
extern Size sift_dim_shm_size_upto(uint32 max_rows, uint32 n_participants);
/* The table is within the share of work_mem * hash_mem_multiplier a shared dimension may take (half of it): when it is not, a join does not build it */
extern bool sift_dim_shm_fits(uint32 nrows, uint32 n_participants);
/* Lays the table out in raw (sift_dim_shm_size() bytes) and returns it: the one the workers are given */
extern SiftDimShm *sift_dim_shm_init(void *raw, uint32 nrows, uint32 n_participants);
/* Every participant calls it, once: it returns when the table is built, and from then on the table is read-only */
extern void sift_dim_shm_build(SiftDimShm *d, SiftDimKeys keys, void *arg);
/*
 * The same, and the keys go to shared_sip too (an int64 filter in shared memory, not finalized, of any size: each participant fills a copy of its own
 * of the same size and merges it before the count phase ends); the elected participant finalizes it before the scan phase ends. Every participant
 * passes the filter at its own address.
 */
extern void sift_dim_shm_build_with_sip(SiftDimShm *d, SiftDimKeys keys, void *arg, SiftSipFilter *shared_sip);

static inline pg_atomic_uint32 *
sift_dim_shm_counts(SiftDimShm *d)
{
	return (pg_atomic_uint32 *) ((char *) d + d->counts_off);
}

static inline const SiftDimEntry *
sift_dim_shm_entries(const SiftDimShm *d)
{
	return (const SiftDimEntry *) ((const char *) d + d->entries_off);
}

/* The bucket of a key: the low bits of its hash (the hash of the aggregate kernels, which a join computes for its probe keys anyway) */
static inline uint32
sift_dim_shm_bucket(const SiftDimShm *d, int64 key)
{
	return (uint32) sift_aggk6_hash1((uint64) key) & (d->nbuckets - 1);
}

/* The rows of a key: the entries of its bucket whose key it is */
typedef struct SiftDimIter
{
	const SiftDimEntry *cur;
	const SiftDimEntry *end;
	int64		key;
} SiftDimIter;

static inline void
sift_dim_shm_find(SiftDimShm *d, int64 key, SiftDimIter *it)
{
	pg_atomic_uint32 *counts = sift_dim_shm_counts(d);
	const uint32 b = sift_dim_shm_bucket(d, key);

	it->cur = sift_dim_shm_entries(d) + pg_atomic_read_u32(&counts[b]);
	it->end = sift_dim_shm_entries(d) + pg_atomic_read_u32(&counts[b + 1]);
	it->key = key;
}

/* The next row of the key, false when there is none */
static inline bool
sift_dim_shm_next(SiftDimIter *it, uint32 *row)
{
	for (; it->cur < it->end; it->cur++)
		if (it->cur->key == it->key)
		{
			*row = it->cur++->row;
			return true;
		}
	return false;
}

#endif							/* SIFT_AGG_PAR_DIM_SHM_H */
