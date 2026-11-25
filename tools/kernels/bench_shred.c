/*
 * Shredder kernels against their composition from the shared tables: for every
 * kernel of SiftShredKernels, the fused kernel (one pass over the state bytes) and what the shared slots give for the same answer,
 *   state_bits   = sift_colk->range[SIFT_COLW_8] twice (state != VALUE, state == OVERFLOW),
 *   state_counts = range per state + bits_count (the fourth count is the rest),
 * Same method as bench_col.c (best of 300 batches). The composition gets the padding it needs (the shredder's arrays do not have it),
 * which favours it. "shared/fused" above 1.03 means the shared slots lose by more than the 3 percent the unification allows, and
 * the fused kernel stays. Every composed result is compared with the fused one first.
 */
#include "harness.h"

#include "core/simd_kern.h"
#include "col/codec/int.h"
#include "col/shred/shred.h"

#undef snprintf

static SiftRng	rng;

static void
verdict(const char *kernel, const char *scen, int l, double fused, double shared)
{
	printf("%-14s %-24s %-8s %10.1f %10.1f %8.3f  %s\n", kernel, scen, kt_level_name[l], fused, shared, shared / fused,
		   shared / fused > 1.03 ? "keep fused" : "shared ok");
}

/* State bytes: VALUE with probability pct percent, the rest spread over the other three */
static uint8 *
rand_state(uint32 n, int pct)
{
	uint8	   *s = kt_alloc(n + SIFT_COL_PAD);

	for (uint32 i = 0; i < n; i++)
		s[i] = sift_rng_below(&rng, 100) < pct ? SIFT_SHRED_VALUE : (uint8) (sift_rng_below(&rng, 3) == 0 ? SIFT_SHRED_OVERFLOW : sift_rng_below(&rng, 2));
	memset(s + n, 0, SIFT_COL_PAD);
	return s;
}

static void
bench_state(uint32 n, const char *size, int pct)
{
	char		scen[64];
	uint8	   *state = rand_state(n, pct);
	uint64	   *nulls = kt_alloc(SIFT_COL_NWORDS(n) * 8),
			   *ovf = kt_alloc(SIFT_COL_NWORDS(n) * 8),
			   *tmp = kt_alloc(SIFT_COL_NWORDS(n) * 8),
			   *want_n = kt_alloc(SIFT_COL_NWORDS(n) * 8),
			   *want_o = kt_alloc(SIFT_COL_NWORDS(n) * 8);

	snprintf(scen, sizeof(scen), "%s, %d%% VALUE", size, pct);
	for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_AVX512 && l <= sift_cpu.detected; l++)
	{
		const SiftShredKernels *f = sift_shred_kernels_for(l);
		const SiftColKernels *c = sift_col_kernels_for(l);
		uint32		counts[4],
					got[4];

		f->state_bits(state, n, want_n, want_o);
		c->range[SIFT_COLW_8] (state, n, SIFT_SHRED_VALUE + 1, 254, nulls);
		c->range[SIFT_COLW_8] (state, n, SIFT_SHRED_OVERFLOW, 0, ovf);
		if (memcmp(nulls, want_n, (n + 63) / 64 * 8) || memcmp(ovf, want_o, (n + 63) / 64 * 8))
			printf("state_bits composed differs at %s\n", kt_level_name[l]);
		verdict("state_bits", scen, l, BENCH(f->state_bits(state, n, want_n, want_o)),
				BENCH(c->range[SIFT_COLW_8] (state, n, SIFT_SHRED_VALUE + 1, 254, nulls);
					  c->range[SIFT_COLW_8] (state, n, SIFT_SHRED_OVERFLOW, 0, ovf)));

		f->state_counts(state, n, counts);
		for (int s = 0; s < 3; s++)
		{
			c->range[SIFT_COLW_8] (state, n, s, 0, tmp);
			got[s] = c->bits_count(tmp, SIFT_COL_NWORDS(n));
		}
		got[3] = n - got[0] - got[1] - got[2];
		if (memcmp(counts, got, sizeof(got)))
			printf("state_counts composed differs at %s\n", kt_level_name[l]);
		verdict("state_counts", scen, l, BENCH(f->state_counts(state, n, counts)),
				BENCH(for (int s = 0; s < 3; s++)
					  {
						  c->range[SIFT_COLW_8] (state, n, s, 0, tmp);
						  got[s] = c->bits_count(tmp, SIFT_COL_NWORDS(n));
					  }
					  got[3] = n - got[0] - got[1] - got[2]));
	}
	free(state), free(nulls), free(ovf), free(tmp), free(want_n), free(want_o);
}

/* The running ends of np runs of len elements, and their lengths */
static void
ends_to_lens(const uint32 *end, uint32 np, uint32 *lens)
{
	for (uint32 p = 0, prev = 0; p < np; prev = end[p++])
		lens[p] = end[p] - prev;
}

static void
lens_to_ends(const uint32 *lens, uint32 np, uint32 *end)
{
	for (uint32 p = 0, run = 0; p < np; p++)
		end[p] = run += lens[p];
}

/*
 * The owner of each element of a list column (SiftColKernels.expand_parents, running ends in, parent ordinals out) and the run
 * expansion of the codec (SiftCcKernels.rle_expand32, values and lengths in): the same stores of a broadcast value, in two input
 * shapes. One from the other: parents = rle_expand32 of 0, 1, 2... with the lengths taken from the ends; rle_expand32 = the ends from
 * the lengths, expand_parents, and a gather of the values by parent.
 */
static void
bench_runs(uint32 len)
{
	enum {N = 4096};
	char		scen[64];
	uint32		np = N / len;
	uint32	   *end = kt_alloc(N * 4 + SIFT_COL_PAD),
			   *lens = kt_alloc(N * 4 + SIFT_COL_PAD),
			   *iota = kt_alloc(N * 4 + SIFT_COL_PAD),
			   *vals = kt_alloc(N * 4 + SIFT_COL_PAD),
			   *parent = kt_alloc(N * 4 + SIFT_COL_PAD),
			   *want = kt_alloc(N * 4 + SIFT_COL_PAD),
			   *got = kt_alloc(N * 4 + SIFT_COL_PAD);

	snprintf(scen, sizeof(scen), "n=4096, runs of %u", len);
	for (uint32 p = 0; p < np; p++)
	{
		lens[p] = len;
		iota[p] = p;
		vals[p] = (uint32) sift_rng_next(&rng);
	}
	lens_to_ends(lens, np, end);
	for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_AVX512 && l <= sift_cpu.detected; l++)
	{
		const SiftColKernels *c = sift_col_kernels_for(l);
		const SiftCcKernels *k = sift_cc_kernels_for(l);

		c->expand_parents(end, np, want);
		ends_to_lens(end, np, lens);
		k->rle_expand32(iota, lens, np, got);
		if (memcmp(got, want, N * 4))
			printf("expand_parents composed differs at %s\n", kt_level_name[l]);
		verdict("expand_parents", scen, l, BENCH(c->expand_parents(end, np, parent)),
				BENCH(ends_to_lens(end, np, lens); k->rle_expand32(iota, lens, np, got)));

		k->rle_expand32(vals, lens, np, want);
		lens_to_ends(lens, np, end);
		c->expand_parents(end, np, parent);
		c->gather_u32(vals, parent, N, got);
		if (memcmp(got, want, N * 4))
			printf("rle_expand32 composed differs at %s\n", kt_level_name[l]);
		verdict("rle_expand32", scen, l, BENCH(k->rle_expand32(vals, lens, np, got)),
				BENCH(lens_to_ends(lens, np, end); c->expand_parents(end, np, parent); c->gather_u32(vals, parent, N, got)));
	}
	free(end), free(lens), free(iota), free(vals), free(parent), free(want), free(got);
}

void
bench_shred(void)
{
	sift_rng_seed(&rng, 42);
	printf("%-14s %-24s %-8s %10s %10s %8s\n", "kernel", "scenario", "level", "fused ns", "shared ns", "shared/fused");
	for (int pct = 100; pct >= 90; pct -= 10)
	{
		bench_state(4096, "n=4096", pct);
		bench_state(4 << 20, "n=4Mi", pct);
	}
	for (uint32 len = 1; len <= 64; len *= 4)
		bench_runs(len);
}
