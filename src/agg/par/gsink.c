/*
 * agg/par/gsink.c - the streams of agg/par/gsink.h: a group is the partial-aggregate record of agg/par/group_key.h, all of them in partition 0 of the stream of the thread.
 */
#include "postgres.h"

#include "agg/par/group_key.h"
#include "agg/par/gsink.h"
#include "core/par/pool.h"
#include "core/popcount.h"

typedef struct GSinkStream
{
	SiftArena  *arena;
	SiftRadixOut out;
} GSinkStream;

struct SiftV6GSink
{
	SiftParGroupSpec gs;
	int			nthreads;
	atomic_bool full;
	GSinkStream *s[SIFT_PAR_MAX_THREADS];
};

uint64
sift_v6_gsink_record_bytes(const SiftParGroupSpec *gs)
{
	return 8 * (gs->nk + gs->nsum + 1) + (uint64) sift_pc64(gs->strs) * SIFT_V6_GSINK_STR;
}

SiftV6GSink *
sift_v6_gsink_new(MemoryContext cx, const SiftParGroupSpec *gs, int nthreads, size_t bytes)
{
	SiftV6GSink *k = MemoryContextAllocZero(cx, sizeof(SiftV6GSink));

	k->gs = *gs;
	k->nthreads = nthreads;
	atomic_init(&k->full, false);
	for (int t = 0; t < nthreads; t++)
	{
		k->s[t] = MemoryContextAllocAligned(cx, sizeof(GSinkStream), SIFT_ARENA_ALIGN, 0);
		k->s[t]->arena = sift_arena_create(cx, bytes);
		sift_radix_init(&k->s[t]->out, k->s[t]->arena);
	}
	return k;
}

void
sift_v6_gsink_emit(SiftV6GSink *k, int worker, const SiftParGroupRow *row)
{
	if (!pgk_put_to(&k->gs, &k->s[worker]->out, 0, true, row))
		atomic_store_explicit(&k->full, true, memory_order_relaxed);
}

uint64
sift_v6_gsink_records(const SiftV6GSink *k)
{
	uint64		n = 0;

	for (int t = 0; t < k->nthreads; t++)
		n += sift_radix_nrec(&k->s[t]->out, 0);
	return n;
}

bool
sift_v6_gsink_full(const SiftV6GSink *k)
{
	return atomic_load_explicit(&k->full, memory_order_relaxed);
}

typedef struct Drain
{
	const SiftParGroupSpec *gs;
	SiftV6GSinkFold fold;
	void	   *arg;
} Drain;

static bool
drain_chunk(void *arg, const uint8 *b, uint32 len)
{
	const Drain *d = arg;

	for (uint32 off = 0; off < len;)
	{
		SiftParGroupRow row = {0};
		uint32		h32[SIFT_PAR_GROUP_MAX_KEYS];

		off += pgk_parse(d->gs, true, b + off, &row, h32);
		d->fold(d->arg, &row);
	}
	return true;
}

void
sift_v6_gsink_drain(SiftV6GSink *k, SiftV6GSinkFold fold, void *arg)
{
	Drain		d = {.gs = &k->gs,.fold = fold,.arg = arg};

	for (int t = 0; t < k->nthreads; t++)
		sift_radix_scan(&k->s[t]->out, 0, drain_chunk, &d);
}
