/*
 * test/unit/harness.h - what the standalone tests of test/unit share: the check that counts a failure, the clock, a generator of numbers, and the timing of a run at N and at 8N.
 */
#ifndef SIFT_UNIT_HARNESS_H
#define SIFT_UNIT_HARNESS_H

#include <time.h>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define UNIT_QUERIES	5000
#else
#define UNIT_QUERIES	15000
#endif

static int	failures = 0;

#define CHECK(cond, ...) \
	do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double
now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static uint64 rng_state = 88172645463325252ULL;

static uint64
rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

/* A ratio T(8N)/T(N) of the best of seven runs: the run is repeated so that a noisy machine does not decide it */
typedef double (*TimedRun) (int n);

static void
report(const char *name, TimedRun run, int n, double bound)
{
	double		t1 = 1e30,
				t8 = 1e30;

	for (int k = 0; k < 7; k++)
	{
		t1 = Min(t1, run(n));
		t8 = Min(t8, run(8 * n));
	}
	printf("%-28s N=%-6d T(N)=%8.3f ms T(8N)=%9.3f ms ratio=%6.2f bound<=%.1f %s\n", name, n, t1 * 1e3, t8 * 1e3, t8 / t1, bound, t8 / t1 <= bound ? "ok" : "SLOW");
	CHECK(t8 / t1 <= bound, "%s scales as %.2f (bound %.1f)", name, t8 / t1, bound);
}

#endif							/* SIFT_UNIT_HARNESS_H */
