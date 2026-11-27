/*
 * bench_on_core_a1.c - SiftU64Set (core/u64set.h) against the scan it replaces: the answers are the same, and the cost of N lookups in a set of N keys grows with N, not with N squared.
 * Standalone: test/bench_on/run.sh builds it with the calls of the server in test/bench_on/host.c, with ASan and UBSan.
 */
#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#undef printf
#undef fprintf

#include "core/u64set.h"

static double
clock_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e9 + t.tv_nsec;
}

static uint64
mix(uint64 x)
{
	x ^= x >> 33;
	x *= UINT64CONST(0xff51afd7ed558ccd);
	x ^= x >> 33;
	return x;
}

static bool
scan_has(const uint64 *keys, uint32 n, uint64 k)
{
	for (uint32 i = 0; i < n; i++)
		if (keys[i] == k)
			return true;
	return false;
}

static double
run(uint32 n, bool check)
{
	uint64	   *keys = malloc(n * sizeof(uint64));
	SiftU64Set	set;
	double		t0,
				t1;
	uint32		hits = 0;

	for (uint32 i = 0; i < n; i++)
		keys[i] = (i % 17 == 0) ? i / 17 : mix(i);
	sift_u64set_init(&set, n);
	t0 = clock_ns();
	for (uint32 i = 0; i < n; i++)
		(void) sift_u64set_add(&set, keys[i]);
	for (uint32 i = 0; i < n; i++)
		hits += sift_u64set_has(&set, i % 2 ? keys[i] : keys[i] + 1);
	t1 = clock_ns();
	if (check)
		for (uint32 i = 0; i < n; i++)
		{
			uint64		k = i % 2 ? keys[i] : keys[i] + 1;

			if (sift_u64set_has(&set, k) != scan_has(keys, n, k))
			{
				fprintf(stderr, "FAIL: key %llu\n", (unsigned long long) k);
				exit(1);
			}
		}
	if (hits < n / 2)
	{
		fprintf(stderr, "FAIL: %u hits of %u\n", hits, n);
		exit(1);
	}
	sift_u64set_free(&set);
	free(keys);
	return (t1 - t0) / (2.0 * n);
}

int
main(void)
{
	double		small,
				large;

	(void) run(63, true);
	(void) run(1000, true);
	small = run(1000, false);
	large = run(100000, false);
	printf("u64set: %.1f ns/op at N=1000, %.1f ns/op at N=100000 (ratio %.2f; a scan would be 100x)\n", small, large, large / small);
	if (large > 20 * small)
	{
		fprintf(stderr, "FAIL: the cost per operation grows with N\n");
		return 1;
	}
	printf("ok\n");
	return 0;
}
