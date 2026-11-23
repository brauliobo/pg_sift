/*
 * par_core.c - core/par: the radix streams, the top-K heap and its bound, the pool, and the morsel driver, on four threads that the trap shim flags as workers (no palloc, no ereport).
 */
#include "postgres.h"

#include "core/hash.h"
#include "core/par/radix.h"
#include "unit.h"

#define WORKERS		4
#define SLAB		(8 << 20)

static uint64
mix(uint64 i)
{
	return sift_fmix64(i + 1);
}

/* ---- radix: a record is its value, the bytes that follow it (a multiple of 8) and those bytes, all equal to the low byte of the value ---- */

typedef struct RadixJob
{
	SiftRadixOut out[WORKERS];
	SiftArena	arena[WORKERS];
	uint32		per_worker;
} RadixJob;

static uint32
payload_of(uint64 i, uint64 v)
{
	return i % 5 == 0 ? 8 * (1 + v % 40) : 0;
}

static void
radix_scatter(void *arg, int t)
{
	RadixJob   *j = arg;

	for (uint32 i = 0; i < j->per_worker; i++)
	{
		const uint64 v = mix((uint64) t * j->per_worker + i);
		const uint64 n = payload_of(i, v);
		uint8	   *rec = sift_radix_reserve(&j->out[t], sift_radix_part(v), 16 + n);

		CHECK(rec != NULL);
		memcpy(rec, &v, 8);
		memcpy(rec + 8, &n, 8);
		memset(rec + 16, (int) (v & 0xff), n);
	}
}

typedef struct RadixSeen
{
	uint64		sum;
	uint64		n;
	uint32		part;
} RadixSeen;

static bool
radix_chunk(void *arg, const uint8 *data, uint32 len)
{
	RadixSeen  *s = arg;

	for (uint32 off = 0; off < len;)
	{
		uint64		v,
					n;

		CHECK(len - off >= 16);
		memcpy(&v, data + off, 8);
		memcpy(&n, data + off + 8, 8);
		CHECK(sift_radix_part(v) == s->part && len - off >= 16 + n);
		for (uint64 b = 0; b < n; b++)
			CHECK(data[off + 16 + b] == (uint8) (v & 0xff));
		s->sum += v;
		s->n++;
		off += 16 + n;
	}
	return true;
}

static void
test_radix_scatter(void)
{
	static RadixJob job;
	uint8	   *slab[WORKERS];
	uint64		expect = 0;
	RadixSeen	all = {0};

	job.per_worker = 10000;
	for (int t = 0; t < WORKERS; t++)
	{
		slab[t] = aligned_alloc(SIFT_ARENA_ALIGN, SLAB);
		CHECK(slab[t] != NULL);
		sift_arena_init(&job.arena[t], slab[t], SLAB);
		sift_radix_init(&job.out[t], &job.arena[t]);
	}
	unit_run_workers(WORKERS, radix_scatter, &job);
	for (uint64 i = 0; i < (uint64) WORKERS * job.per_worker; i++)
		expect += mix(i);
	for (uint32 p = 0; p < SIFT_RADIX_PARTS; p++)
		for (int t = 0; t < WORKERS; t++)
		{
			RadixSeen	s = {.part = p};

			CHECK(sift_radix_scan(&job.out[t], p, radix_chunk, &s));
			CHECK(s.n == sift_radix_nrec(&job.out[t], p));
			all.sum += s.sum;
			all.n += s.n;
		}
	CHECK(all.n == (uint64) WORKERS * job.per_worker && all.sum == expect);
	for (int t = 0; t < WORKERS; t++)
		free(slab[t]);
}

static bool
radix_stop(void *arg, const uint8 *data, uint32 len)
{
	return false;
}

static void
test_radix_full_arena(void)
{
	static uint8 slab[8192] __attribute__((aligned(SIFT_ARENA_ALIGN)));
	SiftArena	a;
	SiftRadixOut o;
	const uint64 v = 7;
	uint64		n;

	sift_arena_init(&a, slab, sizeof(slab));
	sift_radix_init(&o, &a);
	for (int i = 0; i < 8; i++)
		CHECK(sift_radix_put(&o, 3, &v, 8));
	CHECK(sift_radix_nrec(&o, 3) == 8);
	CHECK(sift_radix_reserve(&o, 3, 60000) == NULL && sift_radix_nrec(&o, 3) == 8);
	CHECK(!sift_radix_scan(&o, 3, radix_stop, NULL));
	while (sift_radix_put(&o, 5, &v, 8))
		;
	n = sift_radix_nrec(&o, 5);
	CHECK(n > 0 && !sift_radix_put(&o, 5, &v, 8) && sift_radix_nrec(&o, 5) == n && a.used <= a.cap);
	CHECK(sift_radix_scan(&o, 0, radix_stop, NULL));
}

int
main(void)
{
	test_radix_scatter();
	test_radix_full_arena();
	puts("par_core: ok");
	return 0;
}
