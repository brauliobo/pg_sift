/*
 * Equivalence tests of the aggregation operators.
 *
 *   test [thousands]   random cases per level and thread (default 20), plus the length grid 0..4096
 *
 * Every SIMD table is compared bit for bit with the scalar table over all operators (src/sift_aggkern_cases.h: lengths including
 * multiples of 64 +- 1, thresholds at the boundaries, empty / full / sparse masks, NaN / Inf / -0 doubles, INT_MIN / INT_MAX, codes at
 * the LUT bounds, garbage in the padding). The scalar table itself is checked against a few values written out by hand.
 * KT_THREADS sets the number of threads (default 8).
 */
#include "harness.h"

#include <pthread.h>
#include <stdatomic.h>

#include "sift_aggkern_cases.h"

#define NTHREADS	(getenv("KT_THREADS") ? atoi(getenv("KT_THREADS")) : 8)

typedef struct Tab
{
	const char *name;
	const SiftAggKernels *k;
} Tab;

static Tab	tabs[3];
static int	ntabs;
static atomic_long ncases,
			nfail;
static long iters;

#define FAIL(...) do { \
	if (atomic_fetch_add(&nfail, 1) < 20) \
		fprintf(stderr, __VA_ARGS__); \
} while (0)

/* Runs the case on scalar and every level; reports the first operator that differs */
static void
check_case(const AgkCase *c, uint64 seed)
{
	static __thread AgkRes ref,
				got;

	agk_run(&sift_aggkern_scalar, c, &ref);
	for (int t = 1; t < ntabs; t++)
	{
		int			d;

		agk_run(tabs[t].k, c, &got);
		if ((d = agk_first_diff(&ref, &got)) >= 0)
			FAIL("%s differs from scalar in %s (step %d), n = %d, case seed %llu\n", tabs[t].name, ref.label[d], d, c->n,
				 (unsigned long long) seed);
	}
	atomic_fetch_add(&ncases, 1);
}

static void
one_case(uint64 seed, int n)
{
	SiftRng		r;
	AgkCase		c;

	sift_rng_seed(&r, seed);
	agk_case_gen(&r, &c, n);
	check_case(&c, seed);
	agk_case_free(&c);
}

static void *
worker(void *arg)
{
	uint64		id = (uintptr_t) arg;

	for (long i = 0; i < iters; i++)
		one_case(id * 1000000007ULL + i + 1, -1);
	return NULL;
}

/* Every length 0..300, every multiple of 64 and its neighbours, the maximum; a few cases each */
static void *
grid_worker(void *arg)
{
	uint64		id = (uintptr_t) arg;

	for (int n = id; n <= SIFT_AGG_VEC; n += NTHREADS)
	{
		if (n > 300 && n % 64 > 1 && n % 64 < 63)
			continue;
		for (int rep = 0; rep < 6; rep++)
			one_case(UINT64CONST(0x5EED0000) + (uint64) n * 16 + rep, n);
	}
	return NULL;
}

static void
run(void *(*fn) (void *), const char *what)
{
	pthread_t	th[64];
	int			nt = Min(NTHREADS, 64);

	for (int i = 0; i < nt; i++)
		pthread_create(&th[i], NULL, fn, (void *) (uintptr_t) i);
	for (int i = 0; i < nt; i++)
		pthread_join(th[i], NULL);
	printf("%s: %ld cases, %ld failures\n", what, (long) ncases, (long) nfail);
}

/* ---- the scalar table against values written out by hand ---- */

static int	nspec;

#define EXPECT(cond) do { \
	if (!(cond)) { FAIL("spec: %s (line %d)\n", #cond, __LINE__); } \
	nspec++; \
} while (0)

static uint64
bits_of(SiftCmpOp op, const double *v, int n, double c)
{
	uint64	   *out = AGK_ALLOC(8);
	uint64		r;

	sift_aggkern_scalar.cmp_f64(out, v, n, op, c);
	r = *out;
	AGK_FREE(out);
	return r;
}

static void
spec_float_order(void)
{
	/* NaN, -0, +0, 1, +Inf, -Inf: PostgreSQL float8 order is -Inf < 0 = -0 < 1 < Inf < NaN, NaN = NaN */
	const double nan = __builtin_nan(""),
				v[] = {nan, -0.0, 0.0, 1, INFINITY, -INFINITY};

	EXPECT(bits_of(SCMP_EQ, v, 6, nan) == 0x01);
	EXPECT(bits_of(SCMP_NE, v, 6, nan) == 0x3E);
	EXPECT(bits_of(SCMP_LT, v, 6, nan) == 0x3E);
	EXPECT(bits_of(SCMP_LE, v, 6, nan) == 0x3F);
	EXPECT(bits_of(SCMP_GT, v, 6, nan) == 0x00);
	EXPECT(bits_of(SCMP_GE, v, 6, nan) == 0x01);
	EXPECT(bits_of(SCMP_EQ, v, 6, 0.0) == 0x06);
	EXPECT(bits_of(SCMP_EQ, v, 6, -0.0) == 0x06);
	EXPECT(bits_of(SCMP_LT, v, 6, 0.0) == 0x20);
	EXPECT(bits_of(SCMP_GT, v, 6, 0.0) == 0x19);
	EXPECT(bits_of(SCMP_GE, v, 6, INFINITY) == 0x11);
	EXPECT(bits_of(SCMP_LE, v, 6, 1) == 0x2E);
}

static void
spec_reductions(void)
{
	const double nan = __builtin_nan(""),
				zeros[] = {-0.0, 0.0, -0.0},
				mixed[] = {nan, 1, -5, nan},
				onlynan[] = {nan, nan},
				big[] = {1e16, 1, -1e16, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0};
	const uint64 sel[] = {0x5};
	uint64		zbits;
	double		mn = 7,
				mx = 7;
	int32		imn = 0,
				sidx = 0;
	uint32		prev = UINT32_MAX;
	const uint32 codes[] = {3, 3, 4, 4, 3, 5};
	const uint64 sel2[] = {0x2B};		/* rows 0, 1, 3, 5 */

	EXPECT(sift_aggkern_scalar.minmax_f64(&mn, &mx, zeros, NULL, 3));
	memcpy(&zbits, &mn, 8);
	EXPECT(zbits == 0);
	memcpy(&zbits, &mx, 8);
	EXPECT(zbits == 0);
	EXPECT(sift_aggkern_scalar.minmax_f64(&mn, &mx, mixed, NULL, 4) && mn == -5 && mx != mx);
	EXPECT(sift_aggkern_scalar.minmax_f64(&mn, &mx, onlynan, NULL, 2) && mn != mn && mx != mx);
	mn = mx = 7;
	EXPECT(!sift_aggkern_scalar.minmax_f64(&mn, &mx, mixed, (const uint64[]) {0}, 4) && mn == 7 && mx == 7);
	EXPECT(!sift_aggkern_scalar.minmax_i32(&imn, &sidx, (const int32[]) {5}, (const uint64[]) {0}, 1) && imn == 0);

	/* sum_f64: row i goes to accumulator i % 8, the result is ((a0 + a1) + (a2 + a3)) + ((a4 + a5) + (a6 + a7)) */
	{
		volatile double a0 = 1e16,
					a1 = 2,
					a2 = -1e16,
					a3 = 1;

		a0 += 1;
		EXPECT(sift_aggkern_scalar.sum_f64(big, NULL, 16) == (a0 + a1) + (a2 + a3));
	}
	EXPECT(sift_aggkern_scalar.sum_f64(big, sel, 3) == 0);
	EXPECT(sift_aggkern_scalar.sum_i32((const int32[]) {INT32_MIN, INT32_MAX, 5}, NULL, 3) == 4);

	/* count_changes: selected rows 0, 1, 3, 5 hold 3, 3, 4, 5: two changes from "3", three from "none" */
	prev = 3;
	EXPECT(sift_aggkern_scalar.count_changes(codes, sel2, 6, &prev) == 2 && prev == 5);
	prev = UINT32_MAX;
	EXPECT(sift_aggkern_scalar.count_changes(codes, sel2, 6, &prev) == 3 && prev == 5);
	prev = 9;
	EXPECT(sift_aggkern_scalar.count_changes(codes, (const uint64[]) {0}, 6, &prev) == 0 && prev == 9);
	EXPECT(sift_aggkern_scalar.count_changes(codes, NULL, 6, &prev) == 4 && prev == 5);
}

int
main(int argc, char **argv)
{
	double		thousands = argc > 1 ? atof(argv[1]) : 20;

	kt_cpu_init();
	sift_aggkern_init();
	tabs[ntabs++] = (Tab) {"scalar", &sift_aggkern_scalar};
	for (SiftSimdLevel l = SIFT_SIMD_AVX2; l <= SIFT_SIMD_AVX512; l++)
		if (sift_aggkern_for(l) != tabs[ntabs - 1].k)
			tabs[ntabs++] = (Tab) {kt_level_name[l], sift_aggkern_for(l)};
	printf("levels:");
	for (int t = 0; t < ntabs; t++)
		printf(" %s", tabs[t].name);
	printf("\n");
	spec_float_order();
	spec_reductions();
	printf("spec: %d checks, %ld failures\n", nspec, (long) nfail);
	iters = (long) (thousands * 1000);
	run(grid_worker, "grid");
	run(worker, "random");
	printf("%s\n", nfail ? "FAILED" : "OK");
	return nfail != 0;
}
