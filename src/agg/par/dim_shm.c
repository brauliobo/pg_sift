/*
 * agg/par/dim_shm.c - the shared dimension table of agg/par/dim_shm.h.
 */
#include "postgres.h"

#include "executor/nodeHash.h"
#include "port/pg_bitutils.h"
#include "utils/wait_event.h"

#include "agg/par/dim_shm.h"
#include "core/compat.h"

/* the phases of the barrier: the one it is in is the one a participant that attaches starts with */
enum
{
	PH_COUNT,
	PH_SCAN,
	PH_BASES,
	PH_ADD,
	PH_FILL,
	PH_DONE
};

#define QUEUE_SPAN		TYPEALIGN(PG_CACHE_LINE_SIZE, sift_morsel_size(1))
#define HDR_SPAN		TYPEALIGN(PG_CACHE_LINE_SIZE, sizeof(SiftDimShm))
#define N_GRANULES(n)	(((n) + SIFT_DIM_BLOCK_ROWS - 1) / SIFT_DIM_BLOCK_ROWS)
#define MAX_ROWS		(UINT32_C(1) << 31)
#define N_BLOCKS(nb)	(((nb) + SIFT_DIM_SCAN_BUCKETS - 1) / SIFT_DIM_SCAN_BUCKETS)
/* the matrix of histograms may be this many times the bytes of the table it counts */
#define HIST_SHARE		4

static uint32
buckets_for(uint32 nrows)
{
	Assert(nrows <= MAX_ROWS);
	return pg_nextpower2_32(Max(nrows, 16));
}

/* The bytes of the layout with n_queues queues and the matrix of n_slots histograms */
static Size
layout_bytes(uint32 nrows, uint32 n_queues, uint32 n_slots)
{
	const uint32 nb = buckets_for(nrows);

	return HDR_SPAN + n_queues * QUEUE_SPAN + TYPEALIGN(PG_CACHE_LINE_SIZE, mul_size(sizeof(uint32), N_BLOCKS(nb))) +
		TYPEALIGN(PG_CACHE_LINE_SIZE, mul_size(sizeof(uint32), (Size) nb + 1)) +
		TYPEALIGN(PG_CACHE_LINE_SIZE, mul_size(sizeof(SiftDimEntry), nrows)) +
		TYPEALIGN(PG_CACHE_LINE_SIZE, mul_size(mul_size(sizeof(uint32), nb), n_slots));
}

/* The participants whose histograms the table has, or 0 for a table that counts with atomic adds: a table of few buckets, whose matrix is a small share of its size and fits */
static uint32
slots_for(uint32 nrows, uint32 n_participants)
{
	const uint32 nb = buckets_for(nrows);
	const Size	table = layout_bytes(nrows, 1, 0);

	if (nb > SIFT_DIM_HIST_BUCKETS || mul_size(mul_size(sizeof(uint32), nb), n_participants) > table * HIST_SHARE ||
		layout_bytes(nrows, 1, n_participants) > get_hash_memory_limit() / 2)
		return 0;
	return n_participants;
}

static Size
layout_size(uint32 nrows, uint32 n_participants)
{
	const uint32 slots = slots_for(nrows, n_participants);

	return layout_bytes(nrows, slots ? 1 : 2, slots);
}

Size
sift_dim_shm_size(uint32 nrows, uint32 n_participants)
{
	return add_size(layout_size(nrows, n_participants), PG_CACHE_LINE_SIZE);
}

Size
sift_dim_shm_size_upto(uint32 max_rows, uint32 n_participants)
{
	return add_size(Max(layout_bytes(max_rows, 2, 0), layout_bytes(Min(max_rows, SIFT_DIM_HIST_BUCKETS), 1, n_participants)), PG_CACHE_LINE_SIZE);
}

bool
sift_dim_shm_fits(uint32 nrows, uint32 n_participants)
{
	return nrows <= MAX_ROWS && layout_size(nrows, n_participants) <= get_hash_memory_limit() / 2;
}

static SiftMorselQueue *
queue_of(SiftDimShm *d, int i)
{
	return (SiftMorselQueue *) ((char *) d + d->queue_off[i]);
}

static pg_atomic_uint32 *
counts_of(SiftDimShm *d)
{
	return (pg_atomic_uint32 *) ((char *) d + d->counts_off);
}

static uint32 *
totals_of(SiftDimShm *d)
{
	return (uint32 *) ((char *) d + d->totals_off);
}

static uint32 *
hist_of(SiftDimShm *d, uint32 slot)
{
	return (uint32 *) ((char *) d + d->hist_off) + (Size) slot * d->nbuckets;
}

SiftDimShm *
sift_dim_shm_init(void *raw, uint32 nrows, uint32 n_participants)
{
	SiftDimShm *d = (SiftDimShm *) TYPEALIGN(PG_CACHE_LINE_SIZE, raw);
	const uint32 granules = N_GRANULES(nrows);
	const uint32 slots = slots_for(nrows, n_participants);
	Size		at = HDR_SPAN;
	pg_atomic_uint32 *counts;

	memset(d, 0, sizeof(*d));
	d->magic = SIFT_DIM_SHM_MAGIC;
	d->nrows = nrows;
	d->nbuckets = buckets_for(nrows);
	d->n_blocks = N_BLOCKS(d->nbuckets);
	d->n_slots = slots;
	d->queue_off[0] = at;
	at += QUEUE_SPAN;
	if (!slots)
	{
		d->queue_off[1] = at;
		at += QUEUE_SPAN;
	}
	d->totals_off = at;
	d->counts_off = d->totals_off + TYPEALIGN(PG_CACHE_LINE_SIZE, sizeof(uint32) * d->n_blocks);
	d->entries_off = d->counts_off + TYPEALIGN(PG_CACHE_LINE_SIZE, sizeof(uint32) * ((Size) d->nbuckets + 1));
	d->hist_off = d->entries_off + sizeof(SiftDimEntry) * nrows;
	d->hist_off = TYPEALIGN(PG_CACHE_LINE_SIZE, d->hist_off);
	d->total_bytes = d->hist_off + TYPEALIGN(PG_CACHE_LINE_SIZE, sizeof(uint32) * (Size) d->nbuckets * slots);
	Assert(d->total_bytes == layout_size(nrows, n_participants));
	sift_morsel_init(queue_of(d, 0), 1, &granules, n_participants);
	if (!slots)
		sift_morsel_init(queue_of(d, 1), 1, &granules, n_participants);
	pg_atomic_init_u32(&d->scan_cursor, 0);
	pg_atomic_init_u32(&d->add_cursor, 0);
	pg_atomic_init_u32(&d->next_slot, 0);
	BarrierInit(&d->barrier, 0);
	counts = counts_of(d);
	for (uint32 b = 0; b < d->nbuckets; b++)
		pg_atomic_init_u32(&counts[b], 0);
	pg_atomic_init_u32(&counts[d->nbuckets], nrows);
	memset(hist_of(d, 0), 0, sizeof(uint32) * (Size) d->nbuckets * slots);
	return d;
}

static inline uint32
bucket_of_hash(const SiftDimShm *d, uint64 h)
{
	return (uint32) h & (d->nbuckets - 1);
}

/* What a participant keeps between the phases: the histogram it counts into and the granules it counted, which it places in the fill phase */
typedef struct Local
{
	uint32		slot;
	SiftMorsel *claimed;
	uint32		n_claimed;
	uint32		cap_claimed;
} Local;

static void
claimed_add(Local *l, const SiftMorsel *m)
{
	if (l->n_claimed == l->cap_claimed)
	{
		l->cap_claimed = Max(16, l->cap_claimed * 2);
		l->claimed = l->claimed ? repalloc(l->claimed, l->cap_claimed * sizeof(SiftMorsel)) : palloc(l->cap_claimed * sizeof(SiftMorsel));
	}
	l->claimed[l->n_claimed++] = *m;
}

/* The granule g: its n keys, then the hash of each */
static uint32
granule_hashes(SiftDimShm *d, uint32 g, SiftDimKeys keys, void *arg, int64 *kb, uint64 *h)
{
	const uint32 first = g * SIFT_DIM_BLOCK_ROWS,
				n = Min(SIFT_DIM_BLOCK_ROWS, d->nrows - first);

	keys(arg, first, n, kb);
	for (uint32 i = 0; i < n; i++)
		h[i] = sift_aggk6_hash1((uint64) kb[i]);
	return n;
}

/* The count phase: the keys of the granules the queue gives, into the histogram of the participant or one atomic add on the bucket of each, and into its own copy of the filter */
static void
count_phase(SiftDimShm *d, Local *l, SiftDimKeys keys, void *arg, SiftSipFilter *shared_sip)
{
	pg_atomic_uint32 *counts = counts_of(d);
	uint32	   *mine = NULL;
	SiftSipFilter *local_sip = NULL;
	int64		kb[SIFT_DIM_BLOCK_ROWS];
	uint64		h[SIFT_DIM_BLOCK_ROWS];
	SiftMorsel	m;

	l->slot = pg_atomic_fetch_add_u32(&d->next_slot, 1);
	if (d->n_slots)
	{
		Assert(l->slot < d->n_slots);
		mine = hist_of(d, l->slot);
	}
	if (shared_sip)
		local_sip = sift_sip_init_at(palloc_aligned(sift_sip_size(shared_sip->n_blocks), SIFT_SBBF_BLOCK_BYTES, 0), shared_sip->key_type,
									 shared_sip->n_blocks);
	while (sift_morsel_claim(queue_of(d, 0), l->slot, &m))
	{
		if (mine)
			claimed_add(l, &m);
		for (uint32 g = m.first; g < m.first + m.n; g++)
		{
			const uint32 n = granule_hashes(d, g, keys, arg, kb, h);

			for (uint32 i = 0; i < n; i++)
			{
				if (mine)
					mine[bucket_of_hash(d, h[i])]++;
				else
					pg_atomic_fetch_add_u32(&counts[bucket_of_hash(d, h[i])], 1);
				if (local_sip)
					sift_sip_insert_i64_hashed(local_sip, kb[i], h[i]);
			}
		}
	}
	if (local_sip)
	{
		sift_sip_merge_atomic(shared_sip, local_sip);
		pfree(local_sip);
	}
}

/* The fill phase: the rows of the granules the participant counted (its histogram gives the places), or the granules the second queue gives (an atomic subtract on the bucket) */
static void
fill_phase(SiftDimShm *d, Local *l, SiftDimKeys keys, void *arg)
{
	SiftDimEntry *entries = (SiftDimEntry *) ((char *) d + d->entries_off);
	pg_atomic_uint32 *counts = counts_of(d);
	uint32	   *mine = d->n_slots && l->n_claimed ? hist_of(d, l->slot) : NULL;
	int64		kb[SIFT_DIM_BLOCK_ROWS];
	uint64		h[SIFT_DIM_BLOCK_ROWS];
	SiftMorsel	m;

	for (uint32 c = 0; mine && c < l->n_claimed; c++)
		for (uint32 g = l->claimed[c].first; g < l->claimed[c].first + l->claimed[c].n; g++)
		{
			const uint32 first = g * SIFT_DIM_BLOCK_ROWS,
						n = granule_hashes(d, g, keys, arg, kb, h);

			for (uint32 i = 0; i < n; i++)
			{
				const uint32 b = bucket_of_hash(d, h[i]);
				SiftDimEntry *e = &entries[mine[b]++ + totals_of(d)[b / SIFT_DIM_SCAN_BUCKETS]];

				e->key = kb[i];
				e->row = first + i;
				e->pad = 0;
			}
		}
	while (!d->n_slots && sift_morsel_claim(queue_of(d, 1), l->slot, &m))
		for (uint32 g = m.first; g < m.first + m.n; g++)
		{
			const uint32 first = g * SIFT_DIM_BLOCK_ROWS,
						n = granule_hashes(d, g, keys, arg, kb, h);

			for (uint32 i = 0; i < n; i++)
			{
				SiftDimEntry *e = &entries[pg_atomic_sub_fetch_u32(&counts[bucket_of_hash(d, h[i])], 1)];

				e->key = kb[i];
				e->row = first + i;
				e->pad = 0;
			}
		}
}

/* The block of buckets [lo, hi) of block k */
static void
block_range(const SiftDimShm *d, uint32 k, uint32 *lo, uint32 *hi)
{
	*lo = k * SIFT_DIM_SCAN_BUCKETS;
	*hi = Min(*lo + SIFT_DIM_SCAN_BUCKETS, d->nbuckets);
}

/*
 * The running sum of the counts within each block the participant claims, and the sum of the block. With histograms the sum is exclusive, and each
 * participant's histogram of a bucket becomes where its rows of the bucket start within the block, after those of the participants before it.
 */
static void
scan_phase(SiftDimShm *d)
{
	pg_atomic_uint32 *counts = counts_of(d);
	const uint32 used = Min(pg_atomic_read_u32(&d->next_slot), d->n_slots);
	uint32		k;

	while ((k = pg_atomic_fetch_add_u32(&d->scan_cursor, 1)) < d->n_blocks)
	{
		uint32		lo,
					hi,
					sum = 0;

		block_range(d, k, &lo, &hi);
		for (uint32 b = lo; b < hi; b++)
		{
			if (d->n_slots)
			{
				pg_atomic_write_u32(&counts[b], sum);
				for (uint32 s = 0; s < used; s++)
				{
					const uint32 c = hist_of(d, s)[b];

					hist_of(d, s)[b] = sum;
					sum += c;
				}
			}
			else
			{
				sum += pg_atomic_read_u32(&counts[b]);
				pg_atomic_write_u32(&counts[b], sum);
			}
		}
		totals_of(d)[k] = sum;
	}
}

/* The sum of the blocks before each block, in place of its sum */
static void
bases_phase(SiftDimShm *d)
{
	uint32		before = 0;

	for (uint32 k = 0; k < d->n_blocks; k++)
	{
		const uint32 sum = totals_of(d)[k];

		totals_of(d)[k] = before;
		before += sum;
	}
	Assert(before == d->nrows);
}

static void
add_phase(SiftDimShm *d)
{
	pg_atomic_uint32 *counts = counts_of(d);
	uint32		k;

	while ((k = pg_atomic_fetch_add_u32(&d->add_cursor, 1)) < d->n_blocks)
	{
		const uint32 base = totals_of(d)[k];
		uint32		lo,
					hi;

		block_range(d, k, &lo, &hi);
		for (uint32 b = lo; b < hi; b++)
			pg_atomic_write_u32(&counts[b], pg_atomic_read_u32(&counts[b]) + base);
	}
}

static uint32
barrier_event(void)
{
	static uint32 ev = 0;

	if (ev == 0)
		ev = WaitEventExtensionNew("SiftDimShmBarrier");
	return ev;
}

void
sift_dim_shm_build_with_sip(SiftDimShm *d, SiftDimKeys keys, void *arg, SiftSipFilter *shared_sip)
{
	Local		l = {0};

	Assert(d->magic == SIFT_DIM_SHM_MAGIC);
	switch (BarrierAttach(&d->barrier))
	{
		case PH_COUNT:
			count_phase(d, &l, keys, arg, shared_sip);
			BarrierArriveAndWait(&d->barrier, barrier_event());
			sift_fallthrough;
		case PH_SCAN:
			scan_phase(d);
			if (BarrierArriveAndWait(&d->barrier, barrier_event()))
			{
				bases_phase(d);
				if (shared_sip)
					sift_sip_finalize(shared_sip);
			}
			sift_fallthrough;
		case PH_BASES:
			BarrierArriveAndWait(&d->barrier, barrier_event());
			sift_fallthrough;
		case PH_ADD:
			add_phase(d);
			BarrierArriveAndWait(&d->barrier, barrier_event());
			sift_fallthrough;
		case PH_FILL:
			fill_phase(d, &l, keys, arg);
			BarrierArriveAndWait(&d->barrier, barrier_event());
			sift_fallthrough;
		default:
			break;
	}
	BarrierDetach(&d->barrier);
	if (l.claimed)
		pfree(l.claimed);
}

void
sift_dim_shm_build(SiftDimShm *d, SiftDimKeys keys, void *arg)
{
	sift_dim_shm_build_with_sip(d, keys, arg, NULL);
}
