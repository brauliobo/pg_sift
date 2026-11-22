/*
 * agg/par/morsel_test.c - test-only SQL function of the guided work queue (agg/par/morsel.c), compiled only with SIFT_TESTING and
 * registered by the pg_sift_test extension: the leader and 0, 1, 2, 4 or 8 workers take every granule of a list of parts whose sizes
 * are as skewed as a Zipf law (the first part holds half of the granules, the next a quarter, and so on, some parts hold none) and
 * count the visits of each granule in the shared segment. Every granule must be visited once, every claim must lie within one part
 * and have the guided length, and the last claim of the list is one granule. The participants start together: each waits at a gate
 * until the leader has seen every launched worker arrive.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "funcapi.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/xact.h"
#include "agg/par/morsel.h"
#include "miscadmin.h"
#include "storage/s_lock.h"
#include "storage/shm_toc.h"

PG_FUNCTION_INFO_V1(sift_test_morsel_par);
PGDLLEXPORT void sift_morsel_test_worker(dsm_segment *seg, shm_toc *toc);

#define KEY_SHARED	UINT64CONST(0x5200000000000003)

typedef struct Shared
{
	uint32		parts;
	uint32		total;
	uint32		participants;
	pg_atomic_uint32 arrived pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint32 go pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint64 claims pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint64 bad_claims;
	pg_atomic_uint32 max_run;
	pg_atomic_uint32 tail_run;	/* the length of the claim that ends the list */
	uint32		sizes[FLEXIBLE_ARRAY_MEMBER];	/* the granules of each part, then the queue, then the visits */
} Shared;

static Size
queue_off(uint32 parts)
{
	return TYPEALIGN(PG_CACHE_LINE_SIZE, offsetof(Shared, sizes) + sizeof(uint32) * parts);
}

static Size
visits_off(uint32 parts)
{
	return queue_off(parts) + TYPEALIGN(PG_CACHE_LINE_SIZE, sift_morsel_size(parts));
}

static Size
shared_size(uint32 parts, uint32 total)
{
	return PG_CACHE_LINE_SIZE + visits_off(parts) + (Size) total * sizeof(pg_atomic_uint32);
}

static SiftMorselQueue *
queue_of(Shared *s)
{
	return (SiftMorselQueue *) ((char *) s + queue_off(s->parts));
}

static pg_atomic_uint32 *
visits_of(Shared *s)
{
	return (pg_atomic_uint32 *) ((char *) s + visits_off(s->parts));
}

/* The sizes of the parts: half of the granules, a quarter, an eighth..., every fifth part empty, the list turned by seed */
static void
part_sizes(uint32 *sizes, uint32 parts, uint32 granules, uint64 seed)
{
	uint32		left = granules;

	for (uint32 k = 0; k < parts; k++)
	{
		const uint32 g = k % 5 == 3 ? 0 : (k == parts - 1 ? left : left / 2);

		sizes[(k + seed) % parts] = g;
		left -= g;
	}
}

static Shared *
shared_init(void *raw, uint32 parts, uint32 granules, uint64 seed, uint32 participants)
{
	Shared	   *s = (Shared *) TYPEALIGN(PG_CACHE_LINE_SIZE, raw);
	uint32		total = 0;

	memset(s, 0, offsetof(Shared, sizes));
	s->parts = parts;
	part_sizes(s->sizes, parts, granules, seed);
	for (uint32 k = 0; k < parts; k++)
		total += s->sizes[k];
	s->total = total;
	s->participants = participants;
	pg_atomic_init_u32(&s->arrived, 0);
	pg_atomic_init_u32(&s->go, 0);
	pg_atomic_init_u64(&s->claims, 0);
	pg_atomic_init_u64(&s->bad_claims, 0);
	pg_atomic_init_u32(&s->max_run, 0);
	pg_atomic_init_u32(&s->tail_run, 0);
	sift_morsel_init(queue_of(s), parts, s->sizes, participants);
	for (uint32 g = 0; g < total; g++)
		pg_atomic_init_u32(&visits_of(s)[g], 0);
	return s;
}

static uint32
flat_start(const Shared *s, uint32 part)
{
	uint32		at = 0;

	for (uint32 k = 0; k < part; k++)
		at += s->sizes[k];
	return at;
}

/* Is the claim within its part, and no longer than the guided run of the granules the list has left from it (a lane has no more than that left) */
static bool
claim_ok(const Shared *s, const SiftMorsel *m, uint32 flat)
{
	return m->part < s->parts && m->n >= 1 && m->first + m->n <= s->sizes[m->part] && m->n <= sift_morsel_run(s->total - flat, s->participants);
}

static void
participant(Shared *s)
{
	SiftMorsel	m;

	while (sift_morsel_claim(queue_of(s), (uint32) (ParallelWorkerNumber + 1), &m))
	{
		const uint32 flat = m.part < s->parts ? flat_start(s, m.part) + m.first : 0;

		pg_atomic_fetch_add_u64(&s->claims, 1);
		if (!claim_ok(s, &m, flat))
		{
			pg_atomic_fetch_add_u64(&s->bad_claims, 1);
			continue;
		}
		for (uint32 g = 0; g < m.n; g++)
			pg_atomic_fetch_add_u32(&visits_of(s)[flat + g], 1);
		while (m.n > pg_atomic_read_u32(&s->max_run))
			pg_atomic_write_u32(&s->max_run, m.n);
		if (flat + m.n == s->total)
			pg_atomic_write_u32(&s->tail_run, m.n);
	}
}

void
sift_morsel_test_worker(dsm_segment *seg, shm_toc *toc)
{
	Shared	   *s = shm_toc_lookup(toc, KEY_SHARED, false);

	pg_atomic_fetch_add_u32(&s->arrived, 1);
	while (pg_atomic_read_u32(&s->go) == 0)
	{
		CHECK_FOR_INTERRUPTS();
		SPIN_DELAY();
	}
	participant(s);
}

/* (seed, workers, parts, granules): the launched workers, the claims, the granules, the granules not visited exactly once, the claims out of protocol, the longest claim, the length of the last claim */
Datum
sift_test_morsel_par(PG_FUNCTION_ARGS)
{
	const int	workers = PG_GETARG_INT32(1);
	const uint32 parts = PG_GETARG_INT32(2),
				granules = PG_GETARG_INT32(3);
	const Size	size = shared_size(parts, granules);
	ParallelContext *pcxt = NULL;
	Shared	   *s;
	int			launched = 0;
	int64		wrong = 0;
	TupleDesc	tupdesc;
	Datum		values[7];
	bool		nulls[7] = {false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (workers == 0)
		s = shared_init(palloc(size), parts, granules, PG_GETARG_INT64(0), 1);
	else
	{
		EnterParallelMode();
		pcxt = CreateParallelContext("pg_sift", "sift_morsel_test_worker", workers);
		shm_toc_estimate_chunk(&pcxt->estimator, size);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
		InitializeParallelDSM(pcxt);
		if (pcxt->seg == NULL)
			elog(ERROR, "no shared memory for the parallel test");
		s = shared_init(shm_toc_allocate(pcxt->toc, size), parts, granules, PG_GETARG_INT64(0), workers + 1);
		shm_toc_insert(pcxt->toc, KEY_SHARED, s);
		LaunchParallelWorkers(pcxt);
		launched = pcxt->nworkers_launched;
		while (pg_atomic_read_u32(&s->arrived) < launched)
		{
			CHECK_FOR_INTERRUPTS();
			SPIN_DELAY();
		}
		pg_atomic_write_u32(&s->go, 1);
	}
	participant(s);
	if (pcxt)
		WaitForParallelWorkersToFinish(pcxt);
	for (uint32 g = 0; g < s->total; g++)
		wrong += pg_atomic_read_u32(&visits_of(s)[g]) != 1;
	values[0] = Int32GetDatum(launched);
	values[1] = Int64GetDatum((int64) pg_atomic_read_u64(&s->claims));
	values[2] = Int64GetDatum(s->total);
	values[3] = Int64GetDatum(wrong);
	values[4] = Int64GetDatum((int64) pg_atomic_read_u64(&s->bad_claims));
	values[5] = Int32GetDatum((int32) pg_atomic_read_u32(&s->max_run));
	values[6] = Int32GetDatum((int32) pg_atomic_read_u32(&s->tail_run));
	if (pcxt)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
	}
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

#endif							/* SIFT_TESTING */
