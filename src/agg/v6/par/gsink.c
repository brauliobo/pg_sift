/*
 * agg/v6/par/gsink.c - the streams of agg/v6/par/gsink.h.
 *
 * A record is the length of a text key (0 for integers), the key words, the count, the sums, then the bytes of a text key to a multiple of 8.
 */
#include "postgres.h"

#include "agg/v6/par/gsink.h"
#include "core/par/pool.h"
#include "core/par/radix.h"

typedef struct Stream
{
	SiftArena  *arena;
	SiftRadixOut out;
} Stream;

struct SiftV6GSink
{
	SiftParGroupSpec gs;
	int			nthreads;
	atomic_bool full;
	Stream	   *s[SIFT_PAR_MAX_THREADS];
};

static uint32
rec_words(const SiftParGroupSpec *gs)
{
	return 1 + gs->kw + 1 + gs->nsum;
}

static uint32
rec_tail(const SiftParGroupSpec *gs, uint32 len)
{
	return gs->str && len != SIFT_STR_NULL ? (len + 7) & ~7u : 0;
}

uint64
sift_v6_gsink_record_bytes(const SiftParGroupSpec *gs)
{
	return 8 * rec_words(gs) + (gs->str ? SIFT_V6_GSINK_STR : 0);
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
		k->s[t] = MemoryContextAllocAligned(cx, sizeof(Stream), SIFT_ARENA_ALIGN, 0);
		k->s[t]->arena = sift_arena_create(cx, bytes);
		sift_radix_init(&k->s[t]->out, k->s[t]->arena);
	}
	return k;
}

void
sift_v6_gsink_emit(SiftV6GSink *k, int worker, const SiftParGroupRow *row)
{
	const SiftParGroupSpec *gs = &k->gs;
	const uint32 words = rec_words(gs),
				tail = rec_tail(gs, row->len);
	uint64	   *rec = sift_radix_reserve(&k->s[worker]->out, 0, words * 8 + tail);

	if (rec == NULL)
	{
		atomic_store_explicit(&k->full, true, memory_order_relaxed);
		return;
	}
	rec[0] = gs->str ? row->len : 0;
	memcpy(rec + 1, row->key, gs->kw * sizeof(uint64));
	rec[1 + gs->kw] = row->count;
	memcpy(rec + 2 + gs->kw, row->sum, gs->nsum * sizeof(uint64));
	if (tail > 0)
	{
		rec[words + tail / 8 - 1] = 0;
		memcpy(rec + words, row->str, row->len);
	}
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
	const SiftParGroupSpec *gs = d->gs;

	for (uint32 off = 0; off < len;)
	{
		const uint64 *rec = (const uint64 *) (b + off);
		SiftParGroupRow row = {0};
		const uint32 tail = rec_tail(gs, (uint32) rec[0]);

		row.len = gs->str ? (uint32) rec[0] : 0;
		memcpy(row.key, rec + 1, gs->kw * sizeof(uint64));
		row.count = rec[1 + gs->kw];
		memcpy(row.sum, rec + 2 + gs->kw, gs->nsum * sizeof(uint64));
		row.str = tail > 0 ? (const uint8 *) (rec + rec_words(gs)) : NULL;
		d->fold(d->arg, &row);
		off += rec_words(gs) * 8 + tail;
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
