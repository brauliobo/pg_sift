/*
 * core/par/arena_bench.c - the cost of the arena: a bump, a rewind, the dispatch of sift_alloc inside a scope and the entry of a scope (a setjmp).
 */
#include "postgres.h"

#include <time.h>

#include "core/par/arena.h"
#include "test/common/check.h"

#define N 20000000

static double
nanos(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static bool
scope_a(SiftArena *a, SiftErr *err, size_t *sum)
{
	SIFT_ARENA_TRY(a, err)
		for (int i = 0; i < 64; i++)
			*sum += (size_t) sift_alloc(128) & 1;
	SIFT_ARENA_END
}

static bool
enter_a(SiftArena *a, SiftErr *err)
{
	SIFT_ARENA_TRY(a, err)
	SIFT_ARENA_END
}

int
main(void)
{
	SiftArena  *a = sift_arena_create(CurrentMemoryContext, 1 << 20);
	SiftErr		err;
	size_t		sum = 0;
	double		t = nanos();

	for (int i = 0; i < N / 64; i++)
	{
		for (int j = 0; j < 64; j++)
			sum += (size_t) sift_arena_alloc(a, 128, 64) & 1;
		sift_arena_reset(a);
	}
	printf("bump + reset          %6.2f ns/alloc\n", (nanos() - t) / N);
	t = nanos();
	for (int i = 0; i < N / 64; i++)
	{
		SIFT_CHECK(scope_a(a, &err, &sum));
		sift_arena_reset(a);
	}
	printf("sift_alloc in a scope %6.2f ns/alloc (a scope of 64 allocs)\n", (nanos() - t) / N);
	t = nanos();
	for (int i = 0; i < N / 10; i++)
		SIFT_CHECK(enter_a(a, &err));
	printf("scope enter + leave   %6.2f ns/scope\n", (nanos() - t) / (N / 10));
	return sum == 12345;
}
