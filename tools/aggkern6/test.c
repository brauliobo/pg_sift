/*
 * Equivalence tests of the 2048-row operators (src/agg/kern6*.c).
 *
 *   test [thousands]   random cases per level (default 20), plus the length grid
 *
 * Every SIMD table is compared bit for bit with the scalar table, and the scalar table with a reference written out in this file
 * (per-bit loops, __int128 sums, the eight-accumulator float order). The inputs are allocated with exactly the bytes a kernel may
 * read (no padding), so a sanitizer build (SANITIZE=1) reports a read past the last row, and the selection vector has exactly
 * SIFT_SEL_SLACK entries past its last index.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agg/group/ht.h"
#include "agg/kern6_int.h"

#undef printf
#undef fprintf
#undef snprintf

static void *xalloc(size_t n);

#define AK6C_ALLOC(n)	xalloc(n)
#define AK6C_FREE(p)	free(p)
#include "agg/kern6_cases.h"
#include "core/testrng.h"

SiftCpu		sift_cpu;

void
sift_cpu_register(SiftDispatchRebind fn)
{
	fn(&sift_cpu);
}

static void
cpu_init(void)
{
#if defined(__x86_64__)
	uint32		f = 0;
	const uint32 avx2 = SIFT_CPU_AVX2 | SIFT_CPU_BMI2 | SIFT_CPU_POPCNT;
	const uint32 avx512 = avx2 | SIFT_CPU_AVX512 | SIFT_CPU_AVX512_VPOPCNT | SIFT_CPU_AVX512_VBMI2;

	__builtin_cpu_init();
	f |= __builtin_cpu_supports("popcnt") ? SIFT_CPU_POPCNT : 0;
	f |= __builtin_cpu_supports("bmi2") ? SIFT_CPU_BMI2 : 0;
	f |= __builtin_cpu_supports("avx2") ? SIFT_CPU_AVX2 : 0;
	f |= __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
		__builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512dq") && __builtin_cpu_supports("avx512cd") ? SIFT_CPU_AVX512 : 0;
	f |= __builtin_cpu_supports("avx512vpopcntdq") ? SIFT_CPU_AVX512_VPOPCNT : 0;
	f |= __builtin_cpu_supports("avx512vbmi2") ? SIFT_CPU_AVX512_VBMI2 : 0;
	sift_cpu.flags = f;
	sift_cpu.detected = (f & avx512) == avx512 ? SIFT_SIMD_AVX512 : (f & avx2) == avx2 ? SIFT_SIMD_AVX2 : SIFT_SIMD_SCALAR;
#elif defined(SIFT_HAVE_NEON)
	sift_cpu.flags = SIFT_CPU_NEON;
	sift_cpu.detected = SIFT_SIMD_NEON;
#endif
	sift_cpu.active = sift_cpu.detected;
}

static uint64 rng_s = 0x9E3779B97F4A7C15;

static uint64
rnd(void)
{
	return sift_splitmix_next(&rng_s);
}

static long nchecks,
			nfail;

#define CHECK(cond, ...) do { \
	nchecks++; \
	if (!(cond)) \
	{ \
		if (nfail++ < 20) \
			fprintf(stderr, __VA_ARGS__); \
	} \
} while (0)

/* Exactly n bytes, so that a sanitizer sees an access past them */
static void *
xalloc(size_t n)
{
	void	   *p;

	if (posix_memalign(&p, 64, n ? n : 1))
		abort();
	return p;
}

static int
rbit(const uint64 *m, int i)
{
	return (m[i >> 6] >> (i & 63)) & 1;
}

/* ---- data ---- */

static const int64 edge64[] = {0, 1, -1, INT64_MAX, INT64_MIN, INT64_MAX - 1, INT64_MIN + 1, 0xFFFFFFFFLL, 0x100000000LL,
	-0x100000000LL, 0x7FFFFFFFLL, -0x80000000LL};
static const uint64 edgef[] = {0, UINT64CONST(0x8000000000000000), UINT64CONST(0x7FF0000000000000), UINT64CONST(0xFFF0000000000000),
	UINT64CONST(0x7FF8000000000000), UINT64CONST(0x7FF8000000000001), UINT64CONST(0xFFF8000000000123), UINT64CONST(0x0000000000000001),
	UINT64CONST(0x7FEFFFFFFFFFFFFF), UINT64CONST(0x3FF0000000000000), UINT64CONST(0xBFF0000000000000)};

static int64
gen64(int style)
{
	if (style == 0)
		return (int64) rnd();
	if (style == 1)
		return edge64[rnd() % (sizeof(edge64) / sizeof(edge64[0]))];
	if (style == 2)
		return (int64) (rnd() % 1000) - 500;
	return rnd() & 1 ? edge64[rnd() % (sizeof(edge64) / sizeof(edge64[0]))] : (int64) rnd();
}

static double
genf(int style)
{
	uint64		b;
	double		d;

	if (style == 0 || rnd() % 8)
		d = (double) ((int64) (rnd() % 2000001) - 1000000) / 64.0;
	else
	{
		b = edgef[rnd() % (sizeof(edgef) / sizeof(edgef[0]))];
		memcpy(&d, &b, 8);
	}
	if (style == 2 && rnd() % 4 == 0)
		d *= 1e300;
	return d;
}

/* A bitmap of n bits (the bits >= n zero): kind 0 all, 1 none, 2 random, 3 sparse, 4 dense */
static uint64 *
gen_mask(int n, int kind)
{
	const int	nw = (n + 63) / 64;
	uint64	   *m = xalloc(nw * 8);

	for (int w = 0; w < nw; w++)
	{
		uint64		x = kind == 0 ? ~UINT64CONST(0) : kind == 1 ? 0 : kind == 2 ? rnd() : kind == 3 ? rnd() & rnd() & rnd() & rnd() : rnd() | rnd() | rnd();

		m[w] = x;
	}
	if (nw && (n & 63))
		m[nw - 1] &= (UINT64CONST(1) << (n & 63)) - 1;
	return m;
}

/* A bitmap with garbage at the bits >= n of the last word (what a del bitmap may hold) */
static uint64 *
gen_garbage_mask(int n, int kind)
{
	const int	nw = (n + 63) / 64;
	uint64	   *m = gen_mask(n, kind);

	if (nw && (n & 63))
		m[nw - 1] |= rnd() & ~((UINT64CONST(1) << (n & 63)) - 1);
	return m;
}

static const SiftAggKernelsV6 *tabs[4];
static const char *names[4];
static int	ntabs;

#include "test_sbbf.h"

static const int lens[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 191, 192, 193, 255, 256, 257, 511, 512,
	513, 1000, 1023, 1024, 1025, 2000, 2047, 2048};

/* ---- the case runners ---- */

static void
case_masks(int n)
{
	const int	nw = (n + 63) / 64,
				kinds[] = {0, 1, 2, 3, 4};
	const SiftAggKernelsV6 *s = &sift_aggkern6_scalar;

	for (int ka = 0; ka < 5; ka++)
		for (int kb = 0; kb < 5; kb++)
		{
			uint64	   *a = gen_mask(n, kinds[ka]),
					   *b = gen_garbage_mask(n, kinds[kb]),
					   *ref = xalloc(nw * 8),
					   *got = xalloc(nw * 8);
			uint16	   *sel_ref = xalloc((n + SIFT_SEL_SLACK) * 2 + 2),
					   *sel_got = xalloc((n + SIFT_SEL_SLACK) * 2 + 2);
			uint16		pc = 0,
						k;

			for (int i = 0; i < n; i++)
				pc += rbit(a, i) && !rbit(b, i);
			memcpy(ref, a, nw * 8);
			CHECK(s->mask_andn_del(ref, b, n) == pc, "scalar mask_andn_del popcount, n = %d\n", n);
			for (int i = 0; i < n; i++)
				CHECK(rbit(ref, i) == (rbit(a, i) && !rbit(b, i)), "scalar mask_andn_del bit %d, n = %d\n", i, n);
			for (int t = 1; t < ntabs; t++)
			{
				memcpy(got, a, nw * 8);
				CHECK(tabs[t]->mask_andn_del(got, b, n) == pc && !memcmp(got, ref, nw * 8), "%s mask_andn_del differs, n = %d\n", names[t], n);
			}
			pc = 0;
			for (int i = 0; i < n; i++)
				pc += rbit(a, i) && rbit(b, i);
			CHECK(s->mask_and(ref, a, b, n) == pc, "scalar mask_and popcount, n = %d\n", n);
			for (int i = 0; i < n; i++)
				CHECK(rbit(ref, i) == (rbit(a, i) && rbit(b, i)), "scalar mask_and bit %d, n = %d\n", i, n);
			for (int t = 1; t < ntabs; t++)
			{
				memset(got, 0xA5, nw * 8);
				CHECK(tabs[t]->mask_and(got, a, b, n) == pc && !memcmp(got, ref, nw * 8), "%s mask_and differs, n = %d\n", names[t], n);
				memcpy(got, a, nw * 8);
				CHECK(tabs[t]->mask_and(got, got, b, n) == pc && !memcmp(got, ref, nw * 8), "%s mask_and in place differs, n = %d\n", names[t], n);
			}
			k = s->mask_to_sel16(sel_ref, b, n);
			{
				uint16		e = 0;

				for (int i = 0; i < n; i++)
					if (rbit(b, i))
						CHECK(sel_ref[e++] == i, "scalar mask_to_sel16 entry, n = %d\n", n);
				CHECK(k == e, "scalar mask_to_sel16 count %d != %d, n = %d\n", k, e, n);
			}
			for (int t = 1; t < ntabs; t++)
			{
				CHECK(tabs[t]->mask_to_sel16(sel_got, b, n) == k && !memcmp(sel_got, sel_ref, k * 2), "%s mask_to_sel16 differs, n = %d\n", names[t], n);
			}
			free(a);
			free(b);
			free(ref);
			free(got);
			free(sel_ref);
			free(sel_got);
		}
}

static void
case_hash(int n)
{
	const SiftAggKernelsV6 *s = &sift_aggkern6_scalar;

	for (int usesel = 0; usesel < 2; usesel++)
		for (int mix = 0; mix < 2; mix++)
			for (int wide = 0; wide < 2; wide++)
			{
				uint16	   *sel = NULL;
				int			nrows = n;
				uint64	   *h0 = xalloc(n * 8 + 8),
						   *href = xalloc(n * 8 + 8),
						   *hgot = xalloc(n * 8 + 8);
				uint64	   *m = usesel ? gen_mask(n, 2) : NULL;
				void	   *v;

				if (usesel)
				{
					sel = xalloc((n + SIFT_SEL_SLACK) * 2);
					nrows = s->mask_to_sel16(sel, m, n);
				}
				v = xalloc(n * (wide ? 8 : 4));
				for (int i = 0; i < n; i++)
				{
					if (wide)
						((int64 *) v)[i] = gen64(3);
					else
						((int32 *) v)[i] = (int32) gen64(3);
				}
				for (int i = 0; i < n; i++)
					h0[i] = rnd();
				for (int i = 0; i < nrows; i++)
				{
					const int	r = sel ? sel[i] : i;
					const uint64 x = wide ? (uint64) ((int64 *) v)[r] : (uint64) (int64) ((int32 *) v)[r];

					href[i] = sift_aggk6_hash1(mix ? ((h0[i] << 5) | (h0[i] >> 59)) ^ x : x);
				}
				for (int t = 0; t < ntabs; t++)
				{
					memcpy(hgot, h0, n * 8);
					if (wide)
						tabs[t]->hash_i64(hgot, v, sel, nrows, mix);
					else
						tabs[t]->hash_i32(hgot, v, sel, nrows, mix);
					CHECK(!memcmp(hgot, href, nrows * 8), "%s hash_i%d sel=%d mix=%d differs from the reference, n = %d\n", names[t], wide ? 64 : 32,
						  usesel, mix, n);
				}
				free(h0);
				free(href);
				free(hgot);
				free(m);
				free(sel);
				free(v);
			}
}

static void
case_sums(int n)
{
	const SiftAggKernelsV6 *s = &sift_aggkern6_scalar;

	for (int style = 0; style < 4; style++)
		for (int mk = -1; mk < 5; mk++)
		{
			int32	   *v32 = xalloc(n * 4);
			int64	   *v64 = xalloc(n * 8);
			SiftI128   *v128 = xalloc(n * 16);
			double	   *vf = xalloc(n * 8);
			uint64	   *m = mk < 0 ? NULL : gen_mask(n, mk);
			SiftI128	init = {rnd(), (int64) (rnd() >> (rnd() % 64))},
						r128,
						g128;
			__int128	ref = 0;
			ak6_u128	ref128 = ak6_u128_of(init),
						refi = ak6_u128_of(init);
			int64		ref32 = 0,
						c64,
						g64;
			double		a[8] = {0, 0, 0, 0, 0, 0, 0, 0},
						reff;
			bool		any = false;
			__int128	mn = 0,
						mx = 0;
			SiftI128	gmn,
						gmx,
						rmn,
						rmx;

			for (int i = 0; i < n; i++)
			{
				v32[i] = (int32) gen64(style);
				v64[i] = gen64(style);
				v128[i].lo = (uint64) gen64(style);
				v128[i].hi = gen64(style == 2 ? 2 : 3);
				vf[i] = genf(style % 3);
			}
			for (int i = 0; i < n; i++)
			{
				const bool	on = !m || rbit(m, i);

				if (on)
				{
					ref32 += v32[i];
					ref += v64[i];
					refi += ak6_u128_of(v128[i]);
					{
						__int128	x = (__int128) ak6_u128_of(v128[i]);

						if (!any || x < mn)
							mn = x;
						if (!any || x > mx)
							mx = x;
						any = true;
					}
				}
				a[i & 7] += on ? vf[i] : 0.0;
			}
			ref128 += (ak6_u128) ref;
			reff = ak6_f64_tree(a, 3.5);
			for (int t = 0; t < ntabs; t++)
			{
				const SiftAggKernelsV6 *k = tabs[t];
				bool		fits = ref >= INT64_MIN && ref <= INT64_MAX;

				CHECK(k->sum_i32(v32, m, n) == ref32, "%s sum_i32 differs, n = %d\n", names[t], n);
				r128 = k->sum_i64_to_i128(v64, m, n, init);
				CHECK(ak6_u128_of(r128) == ref128, "%s sum_i64_to_i128 differs, n = %d style %d\n", names[t], n, style);
				c64 = 12345;
				g64 = c64;
				CHECK(k->sum_i64_checked(v64, m, n, &g64) == fits && (fits ? g64 == (int64) ref : g64 == c64),
					  "%s sum_i64_checked differs, n = %d\n", names[t], n);
				g128 = k->sum_i128(v128, m, n, init);
				CHECK(ak6_u128_of(g128) == refi, "%s sum_i128 differs, n = %d style %d\n", names[t], n, style);
				{
					double		f = k->sum_f64_fast(vf, m, n, 3.5);

					CHECK(!memcmp(&f, &reff, 8), "%s sum_f64_fast differs (%a vs %a), n = %d\n", names[t], f, reff, n);
				}
				gmn.lo = gmx.lo = 77;
				gmn.hi = gmx.hi = 77;
				rmn = gmn;
				rmx = gmx;
				CHECK(k->minmax_i128(&gmn, &gmx, v128, m, n) == any, "%s minmax_i128 any differs, n = %d\n", names[t], n);
				if (any)
				{
					rmn = ak6_i128_of((ak6_u128) mn);
					rmx = ak6_i128_of((ak6_u128) mx);
				}
				CHECK(!memcmp(&gmn, &rmn, 16) && !memcmp(&gmx, &rmx, 16), "%s minmax_i128 differs, n = %d\n", names[t], n);
			}
			(void) s;
			free(v32);
			free(v64);
			free(v128);
			free(vf);
			free(m);
		}
}

static void
case_groups(int n)
{
	for (int usesel = 0; usesel < 4; usesel++)
		for (int ng = 1; ng <= 5; ng += 2)
		{
			const int	G = ng == 1 ? 1 : ng == 3 ? 7 : 300;
			const bool	sorted = usesel >= 2;
			uint32	   *gid = xalloc(n * 4);
			int32	   *v32 = xalloc(n * 4);
			int64	   *v64 = xalloc(n * 8);
			SiftI128   *v128 = xalloc(n * 16);
			double	   *vf = xalloc(n * 8);
			uint16	   *sel = NULL;
			uint64	   *m = usesel & 1 ? gen_mask(n, 2) : NULL;
			int			nrows = n;

			if (usesel & 1)
			{
				sel = xalloc((n + SIFT_SEL_SLACK) * 2);
				nrows = sift_aggkern6_scalar.mask_to_sel16(sel, m, n);
			}
			for (int i = 0; i < n; i++)
			{
				gid[i] = sorted ? (uint32) ((int64) i * G / (n ? n : 1)) : rnd() % G;
				v32[i] = (int32) gen64(3);
				v64[i] = gen64(3);
				v128[i].lo = (uint64) gen64(3);
				v128[i].hi = gen64(2);
				vf[i] = genf(2);
			}
			for (int t = 0; t < ntabs; t++)
			{
				const SiftAggKernelsV6 *k = tabs[t];
				int64	   *cnt = xalloc(G * 8),
						   *cref = xalloc(G * 8),
						   *s32 = xalloc(G * 8),
						   *r32 = xalloc(G * 8),
						   *s64 = xalloc(G * 8),
						   *r64 = xalloc(G * 8),
						   *mn = xalloc(G * 8),
						   *mx = xalloc(G * 8),
						   *rmn = xalloc(G * 8),
						   *rmx = xalloc(G * 8);
				SiftI128   *w1 = xalloc(G * 16),
						   *w1r = xalloc(G * 16),
						   *w2 = xalloc(G * 16),
						   *w2r = xalloc(G * 16);
				double	   *f = xalloc(G * 8),
						   *fr = xalloc(G * 8);

				memset(cnt, 0, G * 8);
				memset(cref, 0, G * 8);
				memset(s32, 0, G * 8);
				memset(r32, 0, G * 8);
				memset(s64, 0, G * 8);
				memset(r64, 0, G * 8);
				memset(w1, 0, G * 16);
				memset(w1r, 0, G * 16);
				memset(w2, 0, G * 16);
				memset(w2r, 0, G * 16);
				memset(f, 0, G * 8);
				memset(fr, 0, G * 8);
				for (int g = 0; g < G; g++)
				{
					mn[g] = rmn[g] = INT64_MAX;
					mx[g] = rmx[g] = INT64_MIN;
				}
				for (int i = 0; i < nrows; i++)
				{
					const int	r = sel ? sel[i] : i;
					const uint32 g = gid[r];

					cref[g]++;
					r32[g] = (int64) ((uint64) r32[g] + (uint64) (int64) v32[r]);
					r64[g] = (int64) ((uint64) r64[g] + (uint64) v64[r]);
					w1r[g] = ak6_i128_of(ak6_u128_of(w1r[g]) + (ak6_u128) (__int128) v64[r]);
					w2r[g] = ak6_i128_of(ak6_u128_of(w2r[g]) + ak6_u128_of(v128[r]));
					fr[g] = ak6_canon_nan(fr[g] + vf[r]);
					rmn[g] = Min(rmn[g], v64[r]);
					rmx[g] = Max(rmx[g], v64[r]);
				}
				k->count_gid(cnt, gid, sel, nrows);
				k->sum_gid_i32(s32, gid, v32, sel, nrows);
				k->sum_gid_i64(s64, gid, v64, sel, nrows);
				k->sum_gid_i64_to_i128(w1, gid, v64, sel, nrows);
				k->sum_gid_i128(w2, gid, v128, sel, nrows);
				k->sum_gid_f64_fast(f, gid, vf, sel, nrows);
				k->minmax_gid_i64(mn, mx, gid, v64, sel, nrows);
				CHECK(!memcmp(cnt, cref, G * 8), "%s count_gid differs, n = %d\n", names[t], n);
				CHECK(!memcmp(s32, r32, G * 8), "%s sum_gid_i32 differs, n = %d\n", names[t], n);
				CHECK(!memcmp(s64, r64, G * 8), "%s sum_gid_i64 differs, n = %d\n", names[t], n);
				CHECK(!memcmp(w1, w1r, G * 16), "%s sum_gid_i64_to_i128 differs, n = %d\n", names[t], n);
				CHECK(!memcmp(w2, w2r, G * 16), "%s sum_gid_i128 differs, n = %d\n", names[t], n);
				CHECK(!memcmp(f, fr, G * 8), "%s sum_gid_f64_fast differs, n = %d\n", names[t], n);
				CHECK(!memcmp(mn, rmn, G * 8) && !memcmp(mx, rmx, G * 8), "%s minmax_gid_i64 differs, n = %d\n", names[t], n);
				free(cnt); free(cref); free(s32); free(r32); free(s64); free(r64); free(mn); free(mx); free(rmn); free(rmx);
				free(w1); free(w1r); free(w2); free(w2r); free(f); free(fr);
			}
			free(gid);
			free(v32);
			free(v64);
			free(v128);
			free(vf);
			free(m);
			free(sel);
		}
}

#include "bench.h"
#include "cases_wide.h"

/* The compare of 16 control bytes of the probe loops (agg/group/ht.h): the lanes a walk of the mask visits are the lanes that match, in order */
static void
case_probe(void)
{
	const uint8 vals[4] = {0, 0x80, 0x9C, 0xFF};
	uint8	   *c = xalloc(SIFT_HT_GROUP_WIDTH + 1);

	for (int round = 0; round < 4000; round++)
	{
		const uint8 b = vals[rnd() % 4];
		const uint8 *p = c + rnd() % 2;
		int			lane[SIFT_HT_GROUP_WIDTH],
					want = 0,
					got = 0;
		SiftHtMask	m;

		for (int i = 0; i < SIFT_HT_GROUP_WIDTH + 1; i++)
			c[i] = vals[rnd() % 4];
		for (int i = 0; i < SIFT_HT_GROUP_WIDTH; i++)
			if (p[i] == b)
				lane[want++] = i;
		for (m = sift_ht_match16_fast(p, b); m; SIFT_HT_MASK_CLEAR(m))
			CHECK(got < want && SIFT_HT_MASK_NEXT(m) == (uint32) lane[got++], "ht match16_fast lanes differ\n");
		CHECK(got == want, "ht match16_fast lane count %d != %d\n", got, want);
	}
	free(c);
}

static void
case_all(int n)
{
	case_masks(n);
	case_hash(n);
	case_sums(n);
	case_groups(n);
	case_pred(n);
	case_set_str(n);
	case_wide_hash(n);
	case_minmax(n);
	case_gid_f64(n);
}

/* Values written out by hand */
static void
spec_checks(void)
{
	for (int t = 0; t < ntabs; t++)
	{
		const SiftAggKernelsV6 *k = tabs[t];
		int64		v[4] = {INT64_MAX, 1, 0, 0},
					out = 7;
		int64		w[2] = {INT64_MIN, INT64_MIN};
		SiftI128	z = {0, 0},
					r;
		double		d[3] = {1.0, __builtin_nan(""), 2.0};
		double		x;
		uint64		bits[1] = {0x5};

		CHECK(!k->sum_i64_checked(v, NULL, 2, &out) && out == 7, "%s: INT64_MAX + 1 overflows\n", names[t]);
		CHECK(k->sum_i64_checked(v, NULL, 1, &out) && out == INT64_MAX, "%s: INT64_MAX alone fits\n", names[t]);
		r = k->sum_i64_to_i128(w, NULL, 2, z);
		CHECK(r.lo == 0 && r.hi == -1, "%s: 2 * INT64_MIN = -2^64\n", names[t]);
		CHECK(!k->sum_i64_checked(w, NULL, 2, &out), "%s: 2 * INT64_MIN overflows\n", names[t]);
		x = k->sum_f64_fast(d, NULL, 3, 0.0);
		CHECK(x != x, "%s: a NaN row makes a NaN sum\n", names[t]);
		x = k->sum_f64_fast(d, bits, 3, 0.0);
		CHECK(x == 3.0, "%s: a masked NaN row is not summed\n", names[t]);
	}
	for (int t = 0; t < ntabs; t++)
	{
		const SiftAggKernelsV6 *k = tabs[t];
		const double nan = __builtin_nan(""),
					inf = __builtin_inf();
		const double f[4] = {nan, inf, 1.0, -inf},
					z[4] = {-0.0, 0.0, nan, 1.0};
		const int64 run[4] = {5, 5, 7, 7},
					big[2] = {INT64_MAX, 3};
		const uint64 valid[1] = {0xB};
		uint64		o[1];
		double		mn,
					mx,
					zero = 0.0;
		SiftI128	c128 = {0, 0};
		const SiftI128 v128[2] = {{0, -1}, {1, 0}};
		int64		d[2];

		k->cmp_f64(o, f, 4, SCMP_EQ, nan);
		CHECK(o[0] == 0x1, "%s: NaN = NaN, nothing else is\n", names[t]);
		k->cmp_f64(o, f, 4, SCMP_GT, inf);
		CHECK(o[0] == 0x1, "%s: only NaN is above +Infinity\n", names[t]);
		k->cmp_f64(o, f, 4, SCMP_GE, inf);
		CHECK(o[0] == 0x3, "%s: +Infinity and NaN are at least +Infinity\n", names[t]);
		k->cmp_f64(o, z, 4, SCMP_EQ, zero);
		CHECK(o[0] == 0x3, "%s: -0.0 = +0.0\n", names[t]);
		k->cmp_i128(o, v128, 2, SCMP_LT, c128);
		CHECK(o[0] == 0x1, "%s: (hi -1, lo 0) < 0 < (hi 0, lo 1)\n", names[t]);
		k->stream_run_ne_i64(o, run, valid, 4);
		CHECK(o[0] == 0xD, "%s: NULL rows form a run of their own\n", names[t]);
		k->stream_run_ne_i64(o, run, NULL, 4);
		CHECK(o[0] == 0x5, "%s: runs of equal values\n", names[t]);
		CHECK(k->minmax_f64(&mn, &mx, z, NULL, 4) && !memcmp(&mn, &zero, 8) && mx != mx, "%s: minmax_f64 normalizes -0.0 and NaN\n", names[t]);
		k->mulw_i64(d, big, big, 2);
		CHECK(d[0] == 1 && d[1] == 9, "%s: INT64_MAX * INT64_MAX wraps to 1\n", names[t]);
		k->lin_i64(d, big, -2, big, 3, 2);
		CHECK(d[0] == INT64_MAX && d[1] == 3, "%s: lin_i64 wraps\n", names[t]);
	}
}

int
main(int argc, char **argv)
{
	const bool	bench = argc > 1 && !strcmp(argv[1], "bench");
	const long	iters = (argc > 1 && !bench ? atol(argv[1]) : 20) * 1000;

	cpu_init();
	tabs[0] = &sift_aggkern6_scalar;
	names[0] = "scalar";
	ntabs = 1;
	for (SiftSimdLevel l = SIFT_SIMD_AVX2; l <= SIFT_SIMD_TOP; l++)
		if (sift_aggkern6_for(l) != sift_aggkern6_for(l - 1))
		{
			tabs[ntabs] = sift_aggkern6_for(l);
			names[ntabs++] = sift_simd_level_name(l);
		}
#if defined(__x86_64__)
	if (ntabs > 0 && sift_aggkern6_for(SIFT_SIMD_AVX512) == &sift_aggkern6_avx512)
	{
		tabs[ntabs] = &sift_aggkern6_avx512_conflict;
		names[ntabs++] = "avx512-vpconflict";
	}
#elif defined(SIFT_HAVE_NEON)
	if (ntabs > 0)
	{
		tabs[ntabs] = &sift_aggkern6_neon_vector;
		names[ntabs++] = "neon-vector";
	}
#endif
	printf("levels:");
	for (int t = 0; t < ntabs; t++)
		printf(" %s", names[t]);
	printf("\n");
	if (bench)
	{
		bench_main();
		return 0;
	}
	spec_checks();
	sbbf_checks();
	case_probe();
	printf("spec: %ld checks, %ld failures\n", nchecks, nfail);
	for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++)
		case_all(lens[i]);
	printf("grid: %ld checks, %ld failures\n", nchecks, nfail);
	for (long i = 0; i < iters / 20; i++)
		for (int t = 1; t < ntabs; t++)
			CHECK(ak6c_equiv(&sift_aggkern6_scalar, tabs[t], i + 1, -1) == 0, "%s differs from scalar on shared case %ld\n", names[t], i + 1);
	printf("shared cases: %ld checks, %ld failures\n", nchecks, nfail);
	for (long i = 0; i < iters / 10; i++)
		case_all((int) (rnd() % 2049));
	printf("random: %ld checks, %ld failures\n", nchecks, nfail);
	puts(nfail ? "FAIL" : "OK");
	return nfail != 0;
}

/* sift_simd_level_name of core/cpu.c, for the names above */
const char *
sift_simd_level_name(SiftSimdLevel level)
{
#if defined(SIFT_HAVE_NEON)
	return level == SIFT_SIMD_NEON ? "neon" : "scalar";
#else
	return level == SIFT_SIMD_AVX512 ? "avx512" : level == SIFT_SIMD_AVX2 ? "avx2" : "scalar";
#endif
}
