/*
 * Kernel throughput per level, plus the rejected bm25_accum designs and a
 * memory bandwidth reference. Single thread pinned to KT_CPU (default 2);
 * see BENCH. Run under flock /tmp/sift-perf.lock. "bench col" runs the column kernels (bench_col.c) and "bench shred" the shredder
 * kernels against their composition from the shared slots (bench_shred.c) instead.
 *
 * A block is 16 heap pages of maxoff tuples; a term matches each tuple with
 * probability d. "L1 B/lane" counts the bytes a kernel touches per lane.
 */
#define _GNU_SOURCE
#include "harness.h"

#include <immintrin.h>
#include <pthread.h>
#include <sched.h>

#define NPAGES		16

int			alt_bm25_ymm(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
						 const float4 *knorm, float4 w);
int			alt_bm25_compress(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
							  const float4 *knorm, float4 w, float4 *dense);
int			alt_bm25_tfvar(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
						   const float4 *knorm, float4 w, int var);

typedef struct BenchBlock
{
	int			pop;
	uint64	   *mask;
	float4	   *kn,
			   *sc,
			   *other,
			   *vals,
			   *dense;
	uint8	   *tf;
	uint16	   *slot,
			   *dl[NPAGES];
} BenchBlock;

static Rng	rng;
static const SiftKernels *levels[3];
static int	nlevels;

static void
block_init(BenchBlock *b, int maxoff, double d)
{
	SiftBm25	bm;

	b->mask = kt_alloc(NPAGES * 64);
	b->kn = kt_alloc(NPAGES * SIFT_KERN_SLOTS * 4);
	b->sc = kt_alloc(NPAGES * SIFT_KERN_SLOTS * 4);
	b->other = kt_alloc(NPAGES * SIFT_KERN_SLOTS * 4);
	b->vals = kt_alloc(NPAGES * SIFT_KERN_SLOTS * 4);
	b->dense = kt_alloc(NPAGES * SIFT_KERN_SLOTS * 4 + 64);
	b->slot = kt_alloc(NPAGES * SIFT_KERN_SLOTS * 2);
	b->tf = kt_alloc(NPAGES * SIFT_KERN_SLOTS + SIFT_KERN_PAD);
	memset(b->mask, 0, NPAGES * 64);
	b->pop = 0;
	for (int p = 0; p < NPAGES; p++)
	{
		b->dl[p] = kt_alloc(SIFT_KERN_SLOTS * 2);
		for (int o = 0; o < maxoff; o++)
		{
			b->dl[p][o] = 20 + rng_below(&rng, 400);
			if (rng_unit(&rng) < d)
				b->mask[p * 8 + o / 64] |= UINT64CONST(1) << (o % 64);
		}
	}
	for (int i = 0; i < NPAGES * SIFT_KERN_SLOTS; i++)
	{
		b->sc[i] = (float4) rng_unit(&rng) * 10;
		b->other[i] = (float4) rng_unit(&rng) * 10;
		b->tf[i] = rng_below(&rng, 4) ? 1 : 2 + rng_below(&rng, 6);
	}
	sift_kern_bm25_init(&bm, 1.2f, 0.75f, 180);
	sift_kernels_scalar.knorm(b->kn, (const uint16 *const *) b->dl, b->mask, NPAGES, &bm);
	for (int i = 0; i < NPAGES * 8; i++)
		b->pop += __builtin_popcountll(b->mask[i]);
}

void
row(const char *kernel, const char *scen, const char *lvl, double ns, double lanes, double bytes)
{
	printf("%-12s %-22s %-12s %9.1f %8.0f %9.2f %9.1f\n", kernel, scen, lvl, ns, lanes,
		   lanes / ns, lanes * bytes / ns);
}

static void
bench_block(const char *scen, int maxoff, double d)
{
	BenchBlock		b;
	SiftBm25	bm;

	block_init(&b, maxoff, d);
	sift_kern_bm25_init(&bm, 1.2f, 0.75f, 180);
	for (int l = 0; l < nlevels; l++)
	{
		const SiftKernels *k = levels[l];
		const char *n = kt_level_name[l];

		row("bm25_accum", scen, n, BENCH(k->bm25_accum(b.sc, b.mask, NPAGES, b.tf, b.kn, 0.5f)), b.pop, 13);
		row("bm25 tf=1", scen, n, BENCH(k->bm25_accum(b.sc, b.mask, NPAGES, NULL, b.kn, 0.5f)), b.pop, 12);
		row("knorm", scen, n, BENCH(k->knorm(b.kn, (const uint16 *const *) b.dl, b.mask, NPAGES, &bm)), b.pop, 6);
		row("max_merge", scen, n, BENCH(k->max_merge(b.sc, b.other, b.mask, NPAGES)), b.pop, 12);
		row("select 50%", scen, n, BENCH(k->select_gt(b.slot, b.vals, b.other, b.mask, NPAGES, 5.0f)), b.pop, 4);
		row("select 1%", scen, n, BENCH(k->select_gt(b.slot, b.vals, b.other, b.mask, NPAGES, 9.9f)), b.pop, 4);
	}
	if (nlevels == 3)
	{
		float4	   *s0 = kt_alloc(NPAGES * SIFT_KERN_SLOTS * 4),
				   *s1 = kt_alloc(NPAGES * SIFT_KERN_SLOTS * 4),
				   *orig = kt_alloc(NPAGES * SIFT_KERN_SLOTS * 4);
		const SiftKernels *k = levels[2];

		memcpy(orig, b.sc, NPAGES * SIFT_KERN_SLOTS * 4);
		memcpy(s0, orig, NPAGES * SIFT_KERN_SLOTS * 4);
		memcpy(s1, orig, NPAGES * SIFT_KERN_SLOTS * 4);
		k->bm25_accum(s0, b.mask, NPAGES, b.tf, b.kn, 0.5f);
		alt_bm25_ymm(s1, b.mask, NPAGES, b.tf, b.kn, 0.5f);
		if (memcmp(s0, s1, NPAGES * SIFT_KERN_SLOTS * 4))
			printf("alt ymm differs\n");
		memcpy(s1, orig, NPAGES * SIFT_KERN_SLOTS * 4);
		alt_bm25_compress(s1, b.mask, NPAGES, b.tf, b.kn, 0.5f, b.dense);
		if (memcmp(s0, s1, NPAGES * SIFT_KERN_SLOTS * 4))
			printf("alt compress differs\n");
		for (int v = 0; v < 3; v++)
		{
			static const char *const vn[] = {"tf-expandb", "tf-expandd", "tf-pshufb"};

			memcpy(s1, orig, NPAGES * SIFT_KERN_SLOTS * 4);
			alt_bm25_tfvar(s1, b.mask, NPAGES, b.tf, b.kn, 0.5f, v);
			if (memcmp(s0, s1, NPAGES * SIFT_KERN_SLOTS * 4))
				printf("alt %s differs\n", vn[v]);
			row("bm25_accum", scen, vn[v], BENCH(alt_bm25_tfvar(b.sc, b.mask, NPAGES, b.tf, b.kn, 0.5f, v)), b.pop, 13);
		}
		row("bm25_accum", scen, "alt-ymm", BENCH(alt_bm25_ymm(b.sc, b.mask, NPAGES, b.tf, b.kn, 0.5f)), b.pop, 13);
		row("bm25_accum", scen, "alt-compress", BENCH(alt_bm25_compress(b.sc, b.mask, NPAGES, b.tf, b.kn, 0.5f, b.dense)), b.pop, 13);
		row("bm25_accum", scen, "avx512-1pg", BENCH(
			for (int p = 0, r = 0; p < NPAGES; p++)
				r += k->bm25_accum(b.sc + p * SIFT_KERN_SLOTS, b.mask + p * 8, 1, b.tf + r, b.kn + p * SIFT_KERN_SLOTS, 0.5f)),
			b.pop, 13);
		free(s0), free(s1), free(orig);
	}
}

static void
bench_tf(const char *scen, int p1, uint32 count)
{
	uint8	   *tf = kt_alloc(count + SIFT_KERN_PAD),
			   *out = kt_alloc(count + SIFT_KERN_PAD),
			   *blob = calloc(count * 2 + SIFT_KERN_PAD, 1);
	SiftTfPos	end = {0};
	size_t		l1n = (count + 7) / 8,
				nibn;

	for (uint32 i = 0; i < count; i++)
		tf[i] = rng_below(&rng, 100) < p1 ? 1 : rng_below(&rng, 50) ? 2 + rng_below(&rng, 15) : 17 + rng_below(&rng, 200);
	for (uint32 i = 0; i < count; i++)
		end.nib += tf[i] > 1;
	nibn = (end.nib + 1) / 2;
	end = (SiftTfPos) {0};
	sift_kern_tf_encode(blob, blob + l1n, blob + l1n + nibn, &end, tf, count);
	for (int l = 0; l < nlevels; l++)
	{
		double		ns = BENCH(SiftTfPos pos = {0};
							   levels[l]->tf_decode(out, blob, blob + l1n, blob + l1n + nibn, &pos, count));

		if (memcmp(out, tf, count))
			printf("tf_decode wrong\n");
		row("tf_decode", scen, kt_level_name[l], ns, count, (l1n + nibn + end.esc) / (double) count);
	}
	free(tf), free(out), free(blob);
}

static void
bench_misc(void)
{
	uint8	   *bounds = kt_alloc(4096),
				vm[64];
	float4	   *ub = kt_alloc(4096 * 4);
	uint64		base[8],
				rb[8],
				out[8],
				pm[4];

	for (int i = 0; i < 4096; i++)
		bounds[i] = (uint8) rng_next(&rng), ub[i] = 0;
	for (int i = 0; i < 8; i++)
		base[i] = rng_next(&rng), rb[i] = rng_next(&rng);
	for (int i = 0; i < 64; i++)
		vm[i] = (uint8) rng_next(&rng);
	for (int l = 0; l < nlevels; l++)
	{
		const SiftKernels *k = levels[l];

		row("ub_accum", "n=16 (one SB)", kt_level_name[l], BENCH(k->ub_accum(ub, bounds, 1e-9f, 16)), 16, 9);
		row("ub_accum", "n=4096", kt_level_name[l], BENCH(k->ub_accum(ub, bounds, 1e-9f, 4096)), 4096, 9);
		row("expand", "8 words, 50%", kt_level_name[l], BENCH(k->expand(out, base, base, rb, 0, 8)), 512, 0.25);
		row("vm_allvisible", "one SB", kt_level_name[l], BENCH(k->vm_allvisible(pm, vm)), 256, 0.25);
	}
	free(bounds), free(ub);
}

/* Read bandwidth reference: AVX-512 sum over a 1 GiB buffer, 1 and 8 threads */
#define BW_BYTES	(UINT64CONST(1) << 30)
static uint8 *bw_buf;

static SIFT_TARGET_AVX512 uint64
bw_read(const uint8 *p, size_t n)
{
	__m512i		a = _mm512_setzero_si512(),
				b = a;

	for (size_t i = 0; i < n; i += 128)
	{
		a = _mm512_add_epi64(a, _mm512_load_si512(p + i));
		b = _mm512_add_epi64(b, _mm512_load_si512(p + i + 64));
	}
	return _mm512_reduce_add_epi64(_mm512_add_epi64(a, b));
}

static void *
bw_thread(void *arg)
{
	size_t		part = BW_BYTES / 8;

	return (void *) (uintptr_t) bw_read(bw_buf + (uintptr_t) arg * part, part);
}

static void
bench_bandwidth(void)
{
	volatile uint64 sink;
	double		t0,
				ns;

	bw_buf = kt_alloc(BW_BYTES);
	memset(bw_buf, 1, BW_BYTES);
	ns = BENCH(sink = bw_read(bw_buf, BW_BYTES));
	printf("DRAM read, 1 thread:  %.1f GB/s\n", BW_BYTES / ns);
	ns = 1e300;
	for (int r = 0; r < 5; r++)
	{
		pthread_t	th[8];

		t0 = now_ns();
		for (uintptr_t i = 0; i < 8; i++)
			pthread_create(&th[i], NULL, bw_thread, (void *) i);
		for (int i = 0; i < 8; i++)
			pthread_join(th[i], NULL);
		ns = Min(ns, now_ns() - t0);
	}
	printf("DRAM read, 8 threads: %.1f GB/s\n", BW_BYTES / ns);
	(void) sink;
	free(bw_buf);
}

int
main(int argc, char **argv)
{
	cpu_set_t	set;
	int			cpu = getenv("KT_CPU") ? atoi(getenv("KT_CPU")) : 2;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
	kt_cpu_init();
	rng_seed(&rng, 42);
	for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_AVX512 && sift_kernels_for(l); l++)
		levels[nlevels++] = sift_kernels_for(l);

	if (argc > 1 && strcmp(argv[1], "shred") == 0)
	{
		bench_shred();
		return 0;
	}
	printf("%-12s %-22s %-12s %9s %8s %9s %9s\n", "kernel", "scenario", "level", "ns/call",
		   "lanes", "Glanes/s", "L1 GB/s");
	if (argc > 1 && strcmp(argv[1], "col") == 0)
	{
		bench_col();
		return 0;
	}
	bench_block("30 tup/pg, d=1", 30, 1);
	bench_block("30 tup/pg, d=0.25", 30, 0.25);
	bench_block("30 tup/pg, d=0.03", 30, 0.03);
	bench_block("291 tup/pg, d=0.5", 291, 0.5);
	bench_tf("P(tf>1)=0.25", 75, 65536);
	bench_tf("P(tf>1)=0.75", 25, 65536);
	bench_tf("P(tf>1)=0.02", 98, 65536);
	bench_misc();
	bench_bandwidth();
	return 0;
}
