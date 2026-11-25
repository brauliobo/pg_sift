/*
 * bench.h - what the scaling tests share: the clock, the numbers of a workload (SplitMix64 of core/testrng.h from a fixed seed) and the timing of a run
 * at N and at 8N, whose ratio must stay under a bound.
 */
#ifndef SIFT_TEST_BENCH_H
#define SIFT_TEST_BENCH_H

#include <time.h>

#include "core/testrng.h"

#include "check.h"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define UNIT_QUERIES	5000
#else
#define UNIT_QUERIES	15000
#endif

static uint64_t bench_state __attribute__((unused)) = UINT64_C(88172645463325252);

#define rnd() sift_splitmix_next(&bench_state)

static inline double
now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* A ratio T(8N)/T(N) of the best of seven runs: the run is repeated so that a noisy machine does not decide it */
typedef double (*TimedRun) (int n);

static inline void
report(const char *name, TimedRun run, int n, double bound)
{
	double		t1 = 1e30,
				t8 = 1e30;

	for (int k = 0; k < 7; k++)
	{
		double		a = run(n),
					b = run(8 * n);

		t1 = a < t1 ? a : t1;
		t8 = b < t8 ? b : t8;
	}
	printf("%-28s N=%-6d T(N)=%8.3f ms T(8N)=%9.3f ms ratio=%6.2f bound<=%.1f %s\n", name, n, t1 * 1e3, t8 * 1e3, t8 / t1, bound, t8 / t1 <= bound ? "ok" : "SLOW");
	SIFT_CHECKF(t8 / t1 <= bound, "%s scales as %.2f (bound %.1f)", name, t8 / t1, bound);
}

#endif							/* SIFT_TEST_BENCH_H */
