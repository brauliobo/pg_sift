/*
 * Column kernel throughput per level: "bench col". Same method and table as the
 * scoring kernels (bench.c): best of 300 batches, ns per call, rows per call, G rows/s and input GB/s ("L1 GB/s" counts the input
 * bytes per row; the arrays fit the L1/L2 cache). Every level's output is compared with the scalar one first.
 */
#include "harness.h"

#include "sift_col_kern_int.h"

#define N			4096			/* rows of a filter call */
#define NW			1152			/* words of a selection of a full unit (73728 rows) */
#define NZ			1024			/* units of a segment */

static Rng	rng;
static const SiftColKernels *levels[3];
static int	nlevels;

static void *
rand_array(size_t bytes)
{
	uint64	   *p = kt_alloc(bytes + SIFT_COL_PAD);

	for (size_t i = 0; i < (bytes + SIFT_COL_PAD) / 8; i++)
		p[i] = rng_next(&rng);
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
		p[i] = rng_next(&rng);
		for (int j = 1; j < density; j++)
			p[i] &= rng_next(&rng);
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
					lo = rng_next(&rng) & max,
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
			uint32		c = rng_below(&rng, ncodes);

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
		mn[i] = (int64) (rng_next(&rng) >> 40);
		mx[i] = mn[i] + (int64) (rng_next(&rng) >> 44);
		nvals[i] = rng_below(&rng, 4);
		nnull[i] = rng_below(&rng, 4);
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
		idx[i] = rng_below(&rng, N);
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

void
bench_col(void)
{
	rng_seed(&rng, 42);
	for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_AVX512 && l <= sift_cpu.detected; l++)
		levels[nlevels++] = sift_col_kernels_for(l);
	bench_range();
	bench_lut();
	bench_bits();
	bench_zone();
	bench_decode();
	bench_lanes();
}
