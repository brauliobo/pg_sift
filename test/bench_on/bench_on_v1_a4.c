/*
 * bench_on_v1_a4.c - KeyIdx (table/v1/keyidx.h), the index of the segment lists of a sift_table, against the scan it replaces: the same positions, and the cost of S lookups in S entries
 * grows with S. Standalone, with the allocator calls of the header provided here: test/bench_on/run.sh builds it with ASan and UBSan.
 */
#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#undef printf
#undef fprintf

#include "table/v1/keyidx.h"

void *
MemoryContextAlloc(MemoryContext context, Size size)
{
	return malloc(size);
}

void
pfree(void *p)
{
	free(p);
}

static double
now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e9 + t.tv_nsec;
}

static int
scan_find(const uint64 *gen, int n, uint64 k)
{
	for (int i = 0; i < n; i++)
		if (gen[i] == k)
			return i;
	return -1;
}

static double
run(int n, bool check)
{
	uint64	   *gen = malloc(n * sizeof(uint64));
	KeyIdx		idx = {0};
	double		t0,
				t1;

	for (int i = 0; i < n; i++)
		gen[i] = (uint64) i * 2654435761u + (i % 3 == 0 ? 0 : UINT64CONST(1) << 40);
	t0 = now();
	for (int i = 0; i < n; i++)
		keyidx_put(NULL, &idx, gen[i], i);
	for (int i = 0; i < n; i++)
		if (keyidx_find(&idx, gen[i]) != i || keyidx_find(&idx, gen[i] + 1) != -1)
		{
			fprintf(stderr, "FAIL: lookup of entry %d\n", i);
			exit(1);
		}
	t1 = now();
	if (check)
		for (int i = 0; i < n; i++)
			if (keyidx_find(&idx, gen[i]) != scan_find(gen, n, gen[i]) || keyidx_find(&idx, gen[i] ^ 7) != scan_find(gen, n, gen[i] ^ 7))
			{
				fprintf(stderr, "FAIL: the scan and the index differ at %d\n", i);
				exit(1);
			}
	keyidx_clear(&idx);
	if (keyidx_find(&idx, gen[0]) != -1)
	{
		fprintf(stderr, "FAIL: a cleared index finds a key\n");
		exit(1);
	}
	keyidx_put(NULL, &idx, gen[0], 5);
	if (keyidx_find(&idx, gen[0]) != 5)
	{
		fprintf(stderr, "FAIL: a cleared index does not take a key\n");
		exit(1);
	}
	keyidx_free(&idx);
	free(gen);
	return (t1 - t0) / (3.0 * n);
}

int
main(void)
{
	double		small,
				large;

	(void) run(1, true);
	(void) run(1000, true);
	small = run(1000, false);
	large = run(100000, false);
	printf("keyidx: %.1f ns/op at S=1000, %.1f ns/op at S=100000 (ratio %.2f; a scan would be 100x)\n", small, large, large / small);
	if (large > 20 * small)
	{
		fprintf(stderr, "FAIL: the cost per operation grows with S\n");
		return 1;
	}
	printf("ok\n");
	return 0;
}
