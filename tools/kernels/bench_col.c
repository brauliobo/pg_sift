/*
 * Column kernel throughput per level: "bench col". Same method and table as the
 * scoring kernels (bench.c): best of 300 batches, ns per call, rows per call, G rows/s and input GB/s ("L1 GB/s" counts the input
 * bytes per row; the arrays fit the L1/L2 cache). Every level's output is compared with the scalar one first.
 */
#include "harness.h"

#include "col/kern_int.h"
#include "col/codec_int.h"

#define N			4096			/* rows of a filter call */
#define NW			1152			/* words of a selection of a full unit (73728 rows) */
#define NZ			1024			/* units of a segment */

static SiftRng	rng;
static const SiftColKernels *levels[3];
static int	nlevels;

static void *
rand_array(size_t bytes)
{
	uint64	   *p = kt_alloc(bytes + SIFT_COL_PAD);

	for (size_t i = 0; i < (bytes + SIFT_COL_PAD) / 8; i++)
		p[i] = sift_rng_next(&rng);
	return p;
}

/* A bit vector with each bit set with probability 2^-density (density >= 1) */
static uint64 *
rand_bits(uint32 nwords, int density)
{
	uint64	   *p = kt_alloc((size_t) nwords * 8 + SIFT_COL_PAD);

	memset(p, 0, (size_t) nwords * 8 + SIFT_COL_PAD);
	for (uint32 i = 0; i < nwords; i++)
	{
		p[i] = sift_rng_next(&rng);
		for (int j = 1; j < density; j++)
			p[i] &= sift_rng_next(&rng);
	}
	return p;
}

static void
same(const char *kernel, int l, const void *got, const void *want, size_t bytes)
{
	if (memcmp(got, want, bytes))
		printf("%s differs at %s\n", kernel, kt_level_name[l]);
}

static void
bench_range(void)
{
	static const char *const name[] = {"range u8", "range u16", "range u32", "range u64"};
	uint64	   *want = kt_alloc(SIFT_COL_NWORDS(N) * 8),
			   *got = kt_alloc(SIFT_COL_NWORDS(N) * 8);

	for (int w = 0; w < 4; w++)
	{
		void	   *a = rand_array((size_t) N << w);
		uint64		max = w == SIFT_COLW_64 ? ~UINT64CONST(0) : (UINT64CONST(1) << (8 << w)) - 1,
					lo = sift_rng_next(&rng) & max,
					span = max / 4;

		sift_col_kernels_for(SIFT_SIMD_SCALAR)->range[w] (a, N, lo, span, want);
		for (int l = 0; l < nlevels; l++)
		{
			levels[l]->range[w] (a, N, lo, span, got);
			same(name[w], l, got, want, SIFT_COL_NWORDS(N) * 8);
			row(name[w], "n=4096, 25% pass", kt_level_name[l], BENCH(levels[l]->range[w] (a, N, lo, span, got)), N, 1 << w);
		}
		free(a);
	}
	free(want), free(got);
}

static void
bench_lut(void)
{
	static const char *const name[] = {"lut u8", "lut u16", "lut u32"};
	uint64	   *want = kt_alloc(SIFT_COL_NWORDS(N) * 8),
			   *got = kt_alloc(SIFT_COL_NWORDS(N) * 8);

	for (int w = 0; w < 3; w++)
	{
		uint32		ncodes = w == 0 ? 256 : w == 1 ? 65536 : 4096;
		uint64	   *lut = rand_bits(ncodes / 64, 1);
		void	   *a = kt_alloc((size_t) N << w);

		for (uint32 i = 0; i < N; i++)
		{
			uint32		c = sift_rng_below(&rng, ncodes);

			memcpy((uint8 *) a + ((size_t) i << w), &c, 1 << w);
		}
		sift_col_kernels_for(SIFT_SIMD_SCALAR)->lut[w] (a, N, lut, want);
		for (int l = 0; l < nlevels; l++)
		{
			levels[l]->lut[w] (a, N, lut, got);
			same(name[w], l, got, want, SIFT_COL_NWORDS(N) * 8);
			row(name[w], w == 0 ? "256 codes" : w == 1 ? "65536 codes" : "4096 codes", kt_level_name[l],
				BENCH(levels[l]->lut[w] (a, N, lut, got)), N, 1 << w);
		}
		free(a), free(lut);
	}
	free(want), free(got);
}

static void
bench_bits(void)
{
	uint64	   *a = rand_bits(NW, 1),
			   *b = rand_bits(NW, 1),
			   *dst = kt_alloc(NW * 8),
			   *dense = rand_bits(N / 64, 1),
			   *sparse = rand_bits(N / 64, 7);
	uint32	   *idx = kt_alloc(N * 4 + SIFT_COL_PAD),
			   *ref = kt_alloc(N * 4 + SIFT_COL_PAD);
	const SiftColKernels *s = sift_col_kernels_for(SIFT_SIMD_SCALAR);

	for (int l = 0; l < nlevels; l++)
	{
		const SiftColKernels *k = levels[l];
		const char *n = kt_level_name[l];
		uint32		nd = s->bits_to_idx(dense, N / 64, 7, ref),
					ns;

		if (k->bits_count(a, NW) != s->bits_count(a, NW) || k->bits_and_count(a, b, NW) != s->bits_and_count(a, b, NW))
			printf("bits_count differs at %s\n", n);
		ns = k->bits_to_idx(dense, N / 64, 7, idx);
		if (ns != nd || memcmp(idx, ref, nd * 4))
			printf("bits_to_idx differs at %s\n", n);
		row("bits_count", "73728 bits", n, BENCH(k->bits_count(a, NW)), NW * 64, 0.125);
		row("bits_and_count", "73728 bits", n, BENCH(k->bits_and_count(a, b, NW)), NW * 64, 0.25);
		row("bits_and", "73728 bits", n, BENCH(k->bits_and(dst, b, NW)), NW * 64, 0.25);
		row("bits_to_idx", "n=4096, 50% set", n, BENCH(k->bits_to_idx(dense, N / 64, 7, idx)), N, 0.125);
		row("bits_to_idx", "n=4096, 0.8% set", n, BENCH(k->bits_to_idx(sparse, N / 64, 7, idx)), N, 0.125);
	}
	free(a), free(b), free(dst), free(dense), free(sparse), free(idx), free(ref);
}

static void
bench_zone(void)
{
	int64	   *mn = rand_array(NZ * 8),
			   *mx = rand_array(NZ * 8);
	uint64	  (*set)[4] = rand_array(NZ * 32);
	uint32	   *nnull = rand_array(NZ * 4),
			   *nvals = rand_array(NZ * 4);
	uint64	   *want = kt_alloc(SIFT_COL_NWORDS(NZ) * 8),
			   *got = kt_alloc(SIFT_COL_NWORDS(NZ) * 8),
				probe[4] = {1 << 3, 0, UINT64CONST(1) << 40, 0},
				keep0[NZ / 64];
	const SiftColKernels *s = sift_col_kernels_for(SIFT_SIMD_SCALAR);

	for (uint32 i = 0; i < NZ; i++)
	{
		mn[i] = (int64) (sift_rng_next(&rng) >> 40);
		mx[i] = mn[i] + (int64) (sift_rng_next(&rng) >> 44);
		nvals[i] = sift_rng_below(&rng, 4);
		nnull[i] = sift_rng_below(&rng, 4);
	}
	memset(keep0, 0xFF, sizeof(keep0));
	s->zone_range(mn, mx, NZ, 1 << 22, 3 << 22, want);
	for (int l = 0; l < nlevels; l++)
	{
		const SiftColKernels *k = levels[l];

		k->zone_range(mn, mx, NZ, 1 << 22, 3 << 22, got);
		same("zone_range", l, got, want, SIFT_COL_NWORDS(NZ) * 8);
		row("zone_range", "1024 units", kt_level_name[l], BENCH(k->zone_range(mn, mx, NZ, 1 << 22, 3 << 22, got)), NZ, 16);
		row("zone_set", "1024 units", kt_level_name[l], BENCH(memcpy(got, keep0, sizeof(keep0)); k->zone_set((const uint64 (*)[4]) set, NZ, probe, got)), NZ, 32);
		row("zone_notnull", "1024 units", kt_level_name[l], BENCH(memcpy(got, keep0, sizeof(keep0)); k->zone_notnull(nnull, nvals, NZ, got)), NZ, 8);
	}
	free(mn), free(mx), free(set), free(nnull), free(nvals), free(want), free(got);
}

static void
bench_decode(void)
{
	uint8	   *src = rand_array(N * 4);
	uint32	   *dst = kt_alloc(N * 4 + SIFT_COL_PAD),
			   *ref = kt_alloc(N * 4 + SIFT_COL_PAD),
			   *idx = kt_alloc(N * 4 + SIFT_COL_PAD);
	int64	   *d64 = kt_alloc(N * 8),
			   *r64 = kt_alloc(N * 8);
	void	   *a = rand_array(N * 8);
	const SiftColKernels *s = sift_col_kernels_for(SIFT_SIMD_SCALAR);
	static const uint32 widths[] = {7, 13, 20, 29};
	static const char *const scen[] = {"7 bits, n=4096", "13 bits, n=4096", "20 bits, n=4096", "29 bits, n=4096"};

	for (uint32 i = 0; i < N; i++)
		idx[i] = sift_rng_below(&rng, N);
	for (int wi = 0; wi < lengthof(widths); wi++)
	{
		s->unpack(src, widths[wi], N, ref);
		for (int l = 0; l < nlevels; l++)
		{
			levels[l]->unpack(src, widths[wi], N, dst);
			same("unpack", l, dst, ref, N * 4);
			row("unpack", scen[wi], kt_level_name[l], BENCH(levels[l]->unpack(src, widths[wi], N, dst)), N, widths[wi] / 8.0);
		}
	}
	for (int w = 0; w < 4; w += 3)
	{
		s->for_i64[w] (a, N, -5, r64);
		for (int l = 0; l < nlevels; l++)
		{
			levels[l]->for_i64[w] (a, N, -5, d64);
			same("for_i64", l, d64, r64, N * 8);
			row(w ? "for_i64 u64" : "for_i64 u8", "n=4096", kt_level_name[l], BENCH(levels[l]->for_i64[w] (a, N, -5, d64)), N, 1 << w);
		}
	}
	s->for_i64_sel[SIFT_COLW_32] (a, idx, N, -5, r64);
	s->gather_u32(a, idx, N, ref);
	for (int l = 0; l < nlevels; l++)
	{
		levels[l]->for_i64_sel[SIFT_COLW_32] (a, idx, N, -5, d64);
		same("for_i64_sel", l, d64, r64, N * 8);
		levels[l]->gather_u32(a, idx, N, dst);
		same("gather_u32", l, dst, ref, N * 4);
		row("for_i64_sel u32", "4096 of 4096", kt_level_name[l], BENCH(levels[l]->for_i64_sel[SIFT_COLW_32] (a, idx, N, -5, d64)), N, 8);
		row("gather_u32", "4096 of 4096", kt_level_name[l], BENCH(levels[l]->gather_u32(a, idx, N, dst)), N, 8);
	}
	free(src), free(dst), free(ref), free(idx), free(d64), free(r64), free(a);
}

/* The consumer's lanes: float keys back to doubles in place, a NULL bitmap as 0/1 flags */
static void
bench_lanes(void)
{
	int64	   *keys = rand_array(N * 8),
			   *want = kt_alloc(N * 8 + SIFT_COL_PAD),
			   *work = kt_alloc(N * 8 + SIFT_COL_PAD);
	uint64	   *bits = rand_bits(SIFT_COL_NWORDS(N), 2);
	uint8	   *ref = kt_alloc(N + SIFT_COL_PAD),
			   *flags = kt_alloc(N + SIFT_COL_PAD);

	memcpy(want, keys, N * 8);
	levels[0]->f8_from_keys(want, N);
	levels[0]->bits_to_flags(bits, N, ref);
	for (int l = 0; l < nlevels; l++)
	{
		memcpy(work, keys, N * 8);
		levels[l]->f8_from_keys(work, N);
		same("f8_from_keys", l, work, want, N * 8);
		levels[l]->bits_to_flags(bits, N, flags);
		same("bits_to_flags", l, flags, ref, N);
		row("f8_from_keys", "n=4096, in place", kt_level_name[l], BENCH(levels[l]->f8_from_keys(work, N)), N, 8);
		row("bits_to_flags", "n=4096", kt_level_name[l], BENCH(levels[l]->bits_to_flags(bits, N, flags)), N, 1.0 / 8);
	}
	free(keys), free(want), free(work), free(bits), free(ref), free(flags);
}

/* The SCALE node's multiply and the ALP node's division into doubles (SiftCcKernels), each level against the scalar one; alp_f8 against the keys and the pass that turns them */
static void
bench_cc(void)
{
	uint32	   *u = rand_array(N * 4),
			   *a32 = rand_array(N * 4),
			   *w32 = kt_alloc(N * 4 + SIFT_COL_PAD),
			   *r32 = kt_alloc(N * 4 + SIFT_COL_PAD);
	int64	   *a64 = rand_array(N * 8),
			   *w64 = kt_alloc(N * 8 + SIFT_COL_PAD),
			   *r64 = kt_alloc(N * 8 + SIFT_COL_PAD),
			   *keys = kt_alloc(N * 8 + SIFT_COL_PAD);
	double	   *d = kt_alloc(N * 8 + SIFT_COL_PAD),
			   *rd = kt_alloc(N * 8 + SIFT_COL_PAD);
	const SiftCcKernels *s = sift_cc_kernels_for(SIFT_SIMD_SCALAR);

	memcpy(r32, a32, N * 4);
	s->scale32(r32, N, 7, 1000);
	memcpy(r64, a64, N * 8);
	s->scale64(r64, N, 7, 1000003);
	s->alp_f8(rd, u, N, -50000, 100.0);
	for (int l = 0; l < nlevels; l++)
	{
		const SiftCcKernels *k = sift_cc_kernels_for(SIFT_SIMD_SCALAR + l);

		memcpy(w32, a32, N * 4);
		k->scale32(w32, N, 7, 1000);
		same("scale32", l, w32, r32, N * 4);
		memcpy(w64, a64, N * 8);
		k->scale64(w64, N, 7, 1000003);
		same("scale64", l, w64, r64, N * 8);
		k->alp_f8(d, u, N, -50000, 100.0);
		same("alp_f8", l, d, rd, N * 8);
		row("scale32", "n=4096, in place", kt_level_name[l], BENCH(k->scale32(w32, N, 7, 1000)), N, 4);
		row("scale64", "n=4096, in place", kt_level_name[l], BENCH(k->scale64(w64, N, 7, 1000003)), N, 8);
		row("alp_f8", "n=4096, to doubles", kt_level_name[l], BENCH(k->alp_f8(d, u, N, -50000, 100.0)), N, 4);
		row("alp_keys_f8+f8_from_keys", "n=4096, the old two passes", kt_level_name[l],
			BENCH(k->alp_keys_f8(keys, u, N, -50000, 100.0); levels[l]->f8_from_keys(keys, N)), N, 4);
	}
	free(u), free(a32), free(w32), free(r32), free(a64), free(w64), free(r64), free(keys), free(d), free(rd);
}

/* The other slots of SiftCcKernels: the unpack at the widths of the chunks (W bits a value), the prefix sums, the maximum, the run expansion, the float keys */
static void
bench_cc2(void)
{
	static const uint32 widths[] = {3, 11, 20};
	static const char *const shapes[] = {"n=4096, W=3", "n=4096, W=11", "n=4096, W=20"};
	enum {NR = N / 32, NRUN = 64};
	uint32	   *src = rand_array(4 * (size_t) NR * 32 + 64),
			   *d1 = kt_alloc(N * 4 + SIFT_COL_PAD),
			   *d2 = kt_alloc(N * 4 + SIFT_COL_PAD),
			   *a = rand_array(N * 4),
			   *vals = rand_array(NRUN * 4),
			   *lens = kt_alloc(NRUN * 4 + SIFT_COL_PAD);
	uint64	   *b1 = kt_alloc(N / 8 + SIFT_COL_PAD),
			   *b2 = kt_alloc(N / 8 + SIFT_COL_PAD);
	int64	   *q1 = kt_alloc(N * 8 + SIFT_COL_PAD),
			   *q2 = kt_alloc(N * 8 + SIFT_COL_PAD);
	const SiftCcKernels *s0 = sift_cc_kernels_for(SIFT_SIMD_SCALAR);
	uint32		total = 0;

	for (int i = 0; i < NRUN; i++)
		total += lens[i] = 1 + (uint32) (sift_rng_next(&rng) % 127);
	for (int l = 0; l < nlevels; l++)
	{
		const SiftCcKernels *k = sift_cc_kernels_for(SIFT_SIMD_SCALAR + l);
		const char *lv = kt_level_name[l];

		for (int w = 0; w < 3; w++)
		{
			s0->unpack32(src, widths[w], NR, 5, d1);
			k->unpack32(src, widths[w], NR, 5, d2);
			same("unpack32", l, d2, d1, N * 4);
			s0->unpack_range32(src, widths[w], NR, 1, 1000, b1);
			k->unpack_range32(src, widths[w], NR, 1, 1000, b2);
			same("unpack_range32", l, b2, b1, N / 8);
			row("unpack32", shapes[w], lv, BENCH(k->unpack32(src, widths[w], NR, 5, d2)), N, 4);
			row("unpack_range32", shapes[w], lv, BENCH(k->unpack_range32(src, widths[w], NR, 1, 1000, b2)), N, 4);
		}
		s0->prefix32(a, N, d1, 7, 3);
		k->prefix32(a, N, d2, 7, 3);
		same("prefix32", l, d2, d1, N * 4);
		s0->prefix64(a, N, q1, 7, 3);
		k->prefix64(a, N, q2, 7, 3);
		same("prefix64", l, q2, q1, N * 8);
		row("prefix32", "n=4096", lv, BENCH(k->prefix32(a, N, d2, 7, 3)), N, 4);
		row("prefix64", "n=4096", lv, BENCH(k->prefix64(a, N, q2, 7, 3)), N, 4);
		row("max32", "n=4096", lv, BENCH(k->max32(a, N)), N, 4);
		if (k->max32(a, N) != s0->max32(a, N))
			printf("max32 differs at %s\n", lv);
		s0->rle_expand32(vals, lens, NRUN, d1);
		k->rle_expand32(vals, lens, NRUN, d2);
		same("rle_expand32", l, d2, d1, 4 * (size_t) total);
		row("rle_expand32", "64 runs of 1..127", lv, BENCH(k->rle_expand32(vals, lens, NRUN, d2)), total, 4);
		s0->alp_keys_f8(q1, a, N, -50000, 100.0);
		k->alp_keys_f8(q2, a, N, -50000, 100.0);
		same("alp_keys_f8", l, q2, q1, N * 8);
		row("alp_keys_f8", "n=4096", lv, BENCH(k->alp_keys_f8(q2, a, N, -50000, 100.0)), N, 4);
		s0->alp_keys_f4(q1, a, N, -50000, 100.0f);
		k->alp_keys_f4(q2, a, N, -50000, 100.0f);
		same("alp_keys_f4", l, q2, q1, N * 8);
		row("alp_keys_f4", "n=4096", lv, BENCH(k->alp_keys_f4(q2, a, N, -50000, 100.0f)), N, 4);
		row("alp_f4", "n=4096, to doubles", lv, BENCH(k->alp_f4((double *) q2, a, N, -50000, 100.0f)), N, 4);
	}
	free(src), free(d1), free(d2), free(a), free(vals), free(lens), free(b1), free(b2), free(q1), free(q2);
}

void
bench_col(void)
{
	sift_rng_seed(&rng, 42);
	for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_AVX512 && l <= sift_cpu.detected; l++)
		levels[nlevels++] = sift_col_kernels_for(l);
	bench_range();
	bench_lut();
	bench_bits();
	bench_zone();
	bench_decode();
	bench_lanes();
	bench_cc();
	bench_cc2();
}
