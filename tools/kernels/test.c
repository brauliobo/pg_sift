/*
 * Kernel equivalence and accuracy tests.
 *
 *   test [millions]   random calls per kernel family (default 2), plus the
 *                     exhaustive tf x dl grid and the bound/top-k checks
 *
 * Every SIMD table is compared bit for bit with the scalar table, including
 * the bytes it must leave unchanged. Scores are also compared with a double
 * precision evaluation of the same formula (bound: 7 ulp, header comment).
 */
#include "harness.h"

#include <pthread.h>
#include <stdatomic.h>

#define NTHREADS	(getenv("KT_THREADS") ? atoi(getenv("KT_THREADS")) : 8)
#define MAXPAGES	4

typedef struct Tab
{
	const char *name;
	const SiftKernels *k;
} Tab;

static Tab	tabs[4];
static int	ntabs;
static atomic_long ncalls,
			nlanes,
			nfail;
static long iters;

#define FAIL(...) do { \
	if (atomic_fetch_add(&nfail, 1) < 20) \
		fprintf(stderr, __VA_ARGS__); \
} while (0)


static float4
pick_f(SiftRng *r, const float4 *v, int n, float4 lo, float4 hi)
{
	return sift_rng_below(r, 2) ? v[sift_rng_below(r, n)] : lo + (float4) sift_rng_unit(r) * (hi - lo);
}

static void
rand_bm25(SiftRng *r, SiftBm25 *bm)
{
	static const float4 k1s[] = {0, 1e-6f, 0.5f, 1.2f, 2.0f, 1000.0f};
	static const float4 bs[] = {0, 1e-6f, 0.25f, 0.75f, 1.0f};
	static const float4 avgs[] = {3.6e-15f, 0.5f, 1, 37.25f, 1000, 65535, 1e9f};

	sift_kern_bm25_init(bm, pick_f(r, k1s, lengthof(k1s), 0, 3), pick_f(r, bs, lengthof(bs), 0, 1),
						pick_f(r, avgs, lengthof(avgs), 1, 2000));
}

static uint16
rand_dl(SiftRng *r)
{
	switch (sift_rng_below(r, 4))
	{
		case 0:
			return (uint16) sift_rng_next(r);
		case 1:
			return (uint16[]) {0, 1, 65535, 65534}[sift_rng_below(r, 4)];
		default:
			return sift_rng_below(r, 300);
	}
}

static uint8
rand_tf(SiftRng *r, int p1)
{
	uint32		x = sift_rng_below(r, 100);

	if (x < p1)
		return 1;
	if (x < 96)
		return 2 + sift_rng_below(r, 15);
	return x < 98 ? 17 + sift_rng_below(r, 239) : (uint8[]) {16, 17, 18, 255}[sift_rng_below(r, 4)];
}

/* Random mask; maxoff[p] = row length needed for page p */
static int
rand_mask(SiftRng *r, uint64 *mask, int npages, int *maxoff)
{
	static const int offs[] = {1, 2, 7, 8, 9, 15, 16, 17, 30, 63, 64, 65, 128, 291, 511, 512};
	static const double dens[] = {0, 0.02, 0.1, 0.25, 0.5, 0.9, 1};
	int			n = 0;

	memset(mask, 0, SIFT_KERN_WORDS(npages) * 8);
	for (int p = 0; p < npages; p++)
	{
		double		d = dens[sift_rng_below(r, lengthof(dens))];

		maxoff[p] = sift_rng_below(r, 3) ? offs[sift_rng_below(r, lengthof(offs))] : 1 + sift_rng_below(r, 512);
		for (int o = 0; o < maxoff[p]; o++)
			if (sift_rng_unit(r) < d)
				mask[p * 8 + o / 64] |= UINT64CONST(1) << (o % 64), n++;
	}
	return n;
}

static void
fill_garbage(SiftRng *r, void *p, size_t n)
{
	for (size_t i = 0; i < n; i += 8)
	{
		uint64		x = sift_rng_next(r);

		memcpy((char *) p + i, &x, Min(8, n - i));
	}
}

/* knorm, bm25_accum, max_merge, select_gt on one random page run */
static void
check_mask_kernels(SiftRng *r)
{
	int			npages = 1 + sift_rng_below(r, MAXPAGES),
				nslots = npages * SIFT_KERN_SLOTS,
				maxoff[MAXPAGES],
				pop,
				ref_n;
	uint64		mask[MAXPAGES * 8];
	uint16	   *dl[MAXPAGES];
	SiftBm25	bm;
	float4	   *kn0 = kt_alloc(nslots * 4),
			   *kn = kt_alloc(nslots * 4),
			   *ref = kt_alloc(nslots * 4),
			   *out = kt_alloc(nslots * 4),
			   *sc0 = kt_alloc(nslots * 4),
			   *other = kt_alloc(nslots * 4),
				w = sift_rng_below(r, 8) ? (float4) (sift_rng_unit(r) * 1e4) : 0,
				theta,
			   *vref = kt_alloc(nslots * 4),
			   *vout = kt_alloc(nslots * 4);
	uint16	   *sref = kt_alloc(nslots * 2),
			   *sout = kt_alloc(nslots * 2);
	uint8	   *tf;
	bool		tf1 = sift_rng_below(r, 8) == 0,
				valid = true;

	pop = rand_mask(r, mask, npages, maxoff);
	rand_bm25(r, &bm);
	tf = malloc(pop + SIFT_KERN_PAD);
	for (int i = 0; i < pop; i++)
		tf[i] = rand_tf(r, 75);
	for (int p = 0; p < npages; p++)
	{
		dl[p] = malloc(maxoff[p] * 2);
		for (int o = 0; o < maxoff[p]; o++)
			dl[p][o] = rand_dl(r);
	}
	fill_garbage(r, kn0, nslots * 4);
	fill_garbage(r, sc0, nslots * 4);
	fill_garbage(r, other, nslots * 4);
	for (int s = 0; s < nslots; s++)
		if (mask[s / 64] >> (s % 64) & 1)
		{
			sc0[s] = sift_rng_below(r, 4) ? (float4) (sift_rng_unit(r) * 1e5) : 0;
			other[s] = sift_rng_below(r, 4) ? (float4) (sift_rng_unit(r) * 1e5) : sc0[s];
		}

	/* knorm */
	memcpy(ref, kn0, nslots * 4);
	sift_kernels_scalar.knorm(ref, (const uint16 *const *) dl, mask, npages, &bm);
	memcpy(kn, ref, nslots * 4);
	for (int t = 1; t < ntabs; t++)
	{
		memcpy(out, kn0, nslots * 4);
		tabs[t].k->knorm(out, (const uint16 *const *) dl, mask, npages, &bm);
		if (memcmp(out, ref, nslots * 4))
			FAIL("knorm mismatch on %s\n", tabs[t].name);
	}

	/* bm25_accum */
	memcpy(ref, sc0, nslots * 4);
	if (sift_kernels_scalar.bm25_accum(ref, mask, npages, tf1 ? NULL : tf, kn, w) != pop)
		FAIL("bm25_accum scalar count\n");
	for (int s = 0; s < nslots; s++)
		if (mask[s / 64] >> (s % 64) & 1)
			valid &= isfinite(ref[s]) && ref[s] >= 0;
	if (!valid)
		FAIL("bm25_accum non-finite score (k1 %g b %g avgdl %g w %g)\n", bm.k1, bm.b, bm.avgdl, w);
	for (int t = 1; t < ntabs; t++)
	{
		memcpy(out, sc0, nslots * 4);
		if (tabs[t].k->bm25_accum(out, mask, npages, tf1 ? NULL : tf, kn, w) != pop ||
			memcmp(out, ref, nslots * 4))
			FAIL("bm25_accum mismatch on %s\n", tabs[t].name);
	}

	/* bm25_sel and prune_gt on a random subset of the slots (cand may reach past mask) */
	{
		uint64		cand[MAXPAGES * 8],
					cref[MAXPAGES * 8],
					cout[MAXPAGES * 8];
		int			nref;
		float4		thr = (float4[]) {-INFINITY, INFINITY, 0, 5e4f, sc0[sift_rng_below(r, nslots)]}[sift_rng_below(r, 5)];

		for (int i = 0; i < npages * 8; i++)
			cand[i] = sift_rng_below(r, 4) ? (sift_rng_next(r) & (mask[i] | sift_rng_next(r))) : mask[i];
		memcpy(ref, sc0, nslots * 4);
		if (sift_kernels_scalar.bm25_sel(ref, mask, cand, npages, tf1 ? NULL : tf, kn, w) != pop)
			FAIL("bm25_sel scalar count\n");
		for (int s = 0; s < nslots; s++)
			if (!(mask[s / 64] >> (s % 64) & 1 && cand[s / 64] >> (s % 64) & 1) && memcmp(&ref[s], &sc0[s], 4))
				FAIL("bm25_sel scalar touched slot %d\n", s);
		memcpy(cref, cand, sizeof(cref));
		nref = sift_kernels_scalar.prune_gt(cref, sc0, npages, thr);
		for (int s = 0; s < nslots; s++)
			if ((cref[s / 64] >> (s % 64) & 1) != ((cand[s / 64] >> (s % 64) & 1) && sc0[s] > thr))
				FAIL("prune_gt scalar slot %d\n", s);
		for (int t = 1; t < ntabs; t++)
		{
			memcpy(out, sc0, nslots * 4);
			if (tabs[t].k->bm25_sel(out, mask, cand, npages, tf1 ? NULL : tf, kn, w) != pop ||
				memcmp(out, ref, nslots * 4))
				FAIL("bm25_sel mismatch on %s\n", tabs[t].name);
			memcpy(cout, cand, sizeof(cout));
			if (tabs[t].k->prune_gt(cout, sc0, npages, thr) != nref || memcmp(cout, cref, npages * 8 * 8))
				FAIL("prune_gt mismatch on %s\n", tabs[t].name);
		}
	}

	/* max_merge */
	memcpy(ref, sc0, nslots * 4);
	sift_kernels_scalar.max_merge(ref, other, mask, npages);
	for (int t = 1; t < ntabs; t++)
	{
		memcpy(out, sc0, nslots * 4);
		tabs[t].k->max_merge(out, other, mask, npages);
		if (memcmp(out, ref, nslots * 4))
			FAIL("max_merge mismatch on %s\n", tabs[t].name);
	}

	/* select_gt, often with theta equal to a stored score */
	if (pop && sift_rng_below(r, 2))
	{
		int			s;

		do
			s = sift_rng_below(r, nslots);
		while (!(mask[s / 64] >> (s % 64) & 1));
		theta = sc0[s];
	}
	else
		theta = (float4[]) {-INFINITY, INFINITY, 0, 5e4f}[sift_rng_below(r, 4)];
	ref_n = sift_kernels_scalar.select_gt(sref, vref, sc0, mask, npages, theta);
	for (int t = 1; t < ntabs; t++)
	{
		int			n = tabs[t].k->select_gt(sout, vout, sc0, mask, npages, theta);

		if (n != ref_n || memcmp(sout, sref, n * 2) || memcmp(vout, vref, n * 4))
			FAIL("select_gt mismatch on %s\n", tabs[t].name);
	}

	atomic_fetch_add(&ncalls, 4 * ntabs);
	atomic_fetch_add(&nlanes, 4L * ntabs * pop);
	for (int p = 0; p < npages; p++)
		free(dl[p]);
	free(tf);
	free(kn0), free(kn), free(ref), free(out), free(sc0), free(other);
	free(vref), free(vout), free(sref), free(sout);
}

/* Encodes random tf at random stream positions, decodes in random pieces */
static void
check_tf(SiftRng *r)
{
	uint32		count = sift_rng_below(r, 8) ? sift_rng_below(r, 2000) : sift_rng_below(r, 70000),
				m = 0,
				e = 0;
	SiftTfPos	p0 = {sift_rng_below(r, 128), sift_rng_below(r, 64), sift_rng_below(r, 32)},
				pe;
	uint8	   *tf = malloc(count + 1),
			   *out = malloc(count + SIFT_KERN_PAD),
			   *blob;
	size_t		l1n,
				nibn;
	int			p1 = (int[]) {0, 50, 75, 95, 100}[sift_rng_below(r, 5)];

	for (uint32 i = 0; i < count; i++)
	{
		tf[i] = rand_tf(r, p1);
		m += tf[i] > 1;
		e += tf[i] > 16;
	}
	l1n = (p0.rank + count + 7) / 8;
	nibn = (p0.nib + m + 1) / 2;
	blob = calloc(l1n + nibn + p0.esc + e + SIFT_KERN_PAD, 1);
	pe = p0;
	sift_kern_tf_encode(blob, blob + l1n, blob + l1n + nibn, &pe, tf, count);
	if (pe.rank != p0.rank + count || pe.nib != p0.nib + m || pe.esc != p0.esc + e)
		FAIL("tf_encode position\n");
	for (int t = 0; t < ntabs; t++)
	{
		SiftTfPos	pos = p0;

		for (uint32 done = 0; done < count;)
		{
			uint32		piece = sift_rng_below(r, 3) ? count - done : sift_rng_below(r, count - done + 1);

			tabs[t].k->tf_decode(out + done, blob, blob + l1n, blob + l1n + nibn, &pos, piece);
			done += piece;
		}
		if (memcmp(out, tf, count) || memcmp(&pos, &pe, sizeof(pos)))
			FAIL("tf_decode mismatch on %s (count %u)\n", tabs[t].name, count);
	}
	atomic_fetch_add(&ncalls, ntabs);
	atomic_fetch_add(&nlanes, (long) ntabs * count);
	free(tf), free(out), free(blob);
}

static int
popcount_words(const uint64 *w, int n)
{
	int			c = 0;

	for (int i = 0; i < n; i++)
		c += __builtin_popcountll(w[i]);
	return c;
}

static void
check_misc(SiftRng *r)
{
	int			nwords = 1 + sift_rng_below(r, 16),
				pop = 0,
				n = sift_rng_below(r, 300),
				off = sift_rng_below(r, 4),
				bit0 = sift_rng_below(r, 130);
	uint64		base[16],
				emask[16],
				outr[16],
				outs[16],
				pm[4],
				pr[4];
	uint64	   *rb;
	uint8		vm[64],
				bounds[304];
	float4		ubr[304],
				ubs[304],
				ub0[304],
				w = (float4) sift_rng_unit(r) * 100;
	double		d = sift_rng_unit(r);

	for (int i = 0; i < nwords; i++)
		pop += __builtin_popcountll(base[i] = sift_rng_below(r, 4) ? sift_rng_next(r) & sift_rng_next(r) : sift_rng_next(r));
	for (int i = 0; i < nwords; i++)
		emask[i] = sift_rng_below(r, 3) ? sift_rng_next(r) : ~UINT64CONST(0);
	rb = malloc(((bit0 + pop + 63) / 64) * 8 + 8);
	fill_garbage(r, rb, ((bit0 + pop + 63) / 64) * 8);
	if (sift_kernels_scalar.expand(outr, emask, base, rb, bit0, nwords) != popcount_words(outr, nwords))
		FAIL("expand count\n");
	for (int i = 0, pos = bit0; i < nwords; pos += __builtin_popcountll(base[i]), i++)
		for (int b = 0, j = 0; b < 64; b++)
			if (base[i] >> b & 1)
			{
				if ((outr[i] >> b & 1) != ((rb[(pos + j) / 64] >> ((pos + j) % 64) & 1) & (emask[i] >> b & 1)))
					FAIL("expand scalar bit\n");
				j++;
			}
			else if (outr[i] >> b & 1)
				FAIL("expand outside base\n");
	for (int t = 1; t < ntabs; t++)
		if (tabs[t].k->expand(outs, emask, base, rb, bit0, nwords) != popcount_words(outr, nwords) || memcmp(outs, outr, nwords * 8))
			FAIL("expand mismatch on %s\n", tabs[t].name);

	fill_garbage(r, vm, 64);
	sift_kernels_scalar.vm_allvisible(pr, vm);
	for (int i = 0; i < 256; i++)
		if ((pr[i / 64] >> (i % 64) & 1) != ((vm[i / 4] >> (i % 4 * 2)) & 1))
			FAIL("vm_allvisible scalar bit %d\n", i);
	for (int t = 1; t < ntabs; t++)
	{
		tabs[t].k->vm_allvisible(pm, vm);
		if (memcmp(pm, pr, 32))
			FAIL("vm_allvisible mismatch on %s\n", tabs[t].name);
	}

	for (int i = 0; i < n + off; i++)
	{
		bounds[i] = sift_rng_below(r, 4) ? (uint8) sift_rng_next(r) : (uint8[]) {0, 1, 255}[sift_rng_below(r, 3)];
		ub0[i] = (float4) (d * 1e6 * sift_rng_unit(r));
	}
	memcpy(ubr, ub0, sizeof(ub0));
	sift_kernels_scalar.ub_accum(ubr + off, bounds + off, w, n);
	for (int t = 1; t < ntabs; t++)
	{
		memcpy(ubs, ub0, sizeof(ub0));
		tabs[t].k->ub_accum(ubs + off, bounds + off, w, n);
		if (memcmp(ubs, ubr, sizeof(ubs)))
			FAIL("ub_accum mismatch on %s\n", tabs[t].name);
	}
	atomic_fetch_add(&ncalls, 3 * ntabs);
	atomic_fetch_add(&nlanes, (long) ntabs * (64 * nwords + 256 + n));
	free(rb);
}

/*
 * The block UB covers the float32 score: one document, n terms, bounds from
 * avgdl_ref, scores and UB weights at avgdl_now. same: n copies of one term, the
 * rounding of every addition going the same way (a plan has up to 32768 leaves).
 */
static void
check_bound(SiftRng *r, int n, bool same)
{
	SiftBm25	ref,
				now;
	uint16		dl = rand_dl(r);
	const uint16 *row = &dl;
	uint64		mask[8] = {1};
	float4		kn[SIFT_KERN_SLOTS] __attribute__((aligned(64))),
				sc[SIFT_KERN_SLOTS] __attribute__((aligned(64))) = {0},
				ub = 0;
	double		ndocs = 1 + sift_rng_below(r, 1u << 30),
				scale = (double[]) {1, 1, 0.5, 1.0001, 3}[sift_rng_below(r, 5)];

	rand_bm25(r, &ref);
	sift_kern_bm25_init(&now, ref.k1, ref.b, (float4) (ref.avgdl * scale));
	sift_kernels_scalar.knorm(kn, &row, mask, 1, &now);
	uint8		tf = 0;
	float4		w = 0;

	for (int t = 0; t < n; t++)
	{
		uint8		b;

		if (!same || t == 0)
		{
			tf = rand_tf(r, 50);
			w = sift_kern_term_weight((float4) (sift_rng_unit(r) * 10), sift_kern_idf(ndocs, 1 + sift_rng_below(r, (uint32) ndocs)), ref.k1);
		}
		b = sift_kern_bound_u8(&ref, tf, dl);

		sift_kernels_scalar.bm25_accum(sc, mask, 1, &tf, kn, w);
		sift_kernels_scalar.ub_accum(&ub, &b, sift_kern_ub_weight(w, now.avgdl, ref.avgdl, n), 1);
	}
	if (!(sc[0] <= ub))
		FAIL("bound below score: %.9g > %.9g (n %d dl %u)\n", sc[0], ub, n, dl);
	atomic_fetch_add(&nlanes, n);
}

static int
hit_cmp(const void *a, const void *b)
{
	const SiftHit *x = a,
			   *y = b;

	return sift_hit_better(x->score, x->key, y) ? -1 : sift_hit_better(y->score, y->key, x);
}

static void
check_topk(SiftRng *r)
{
	int			n = sift_rng_below(r, 400),
				k = 1 + sift_rng_below(r, 40);
	SiftHit    *all = malloc((n + 1) * sizeof(SiftHit)),
			   *buf = malloc(k * sizeof(SiftHit));
	SiftTopK	t;

	sift_kern_topk_init(&t, buf, k, k);
	for (int i = 0; i < n; i++)
	{
		all[i].score = (float4) sift_rng_below(r, 20);
		all[i].key = sift_rng_below(r, 64) * 65536 + i;
		if (sift_kern_topk_admits(&t, all[i].score, all[i].key))
		{
			if (!(all[i].score > sift_kern_topk_theta(&t)))
				FAIL("topk theta excludes an admitted hit\n");
			sift_kern_topk_push(&t, all[i].score, all[i].key);
		}
	}
	qsort(all, n, sizeof(SiftHit), hit_cmp);
	if (sift_kern_topk_sort(&t) != Min(n, k))
		FAIL("topk size (n %d k %d)\n", n, k);
	for (int i = 0; i < Min(n, k); i++)
		if (buf[i].key != all[i].key || buf[i].score != all[i].score)
			FAIL("topk order (n %d k %d)\n", n, k);
	free(all), free(buf);
}

static void *
worker(void *arg)
{
	SiftRng			r;

	sift_rng_seed(&r, (uint64) (uintptr_t) arg);
	for (long i = 0; i < iters; i++)
	{
		check_mask_kernels(&r);
		check_misc(&r);
		check_bound(&r, sift_rng_below(&r, 4) ? 1 + sift_rng_below(&r, 8) : 1 + sift_rng_below(&r, SIFT_MAX_LEAVES), false);
		check_bound(&r, sift_rng_below(&r, 4) ? 1 + sift_rng_below(&r, 8) : 1 + sift_rng_below(&r, SIFT_MAX_LEAVES), false);
		if (i % 64 == 0)
			check_bound(&r, 1 + sift_rng_below(&r, SIFT_MAX_LEAVES * SIFT_MAX_PHRASE), true);
		if (i % 8 == 0)
			check_tf(&r);
		if (i % 16 == 0)
			check_topk(&r);
	}
	return NULL;
}

/*
 * Every tf (0..255) x dl (0..65535) for a grid of k1, b, avgdl: bit identity,
 * finiteness for tf >= 1, and the error against double precision.
 */
static atomic_int grid_next;
static int grid_stride = 1;		/* argv[2]: every Nth combination of k1, b and avgdl (1: all); coprime with 6, 5 and 7 it still meets every value of each */
static _Atomic double max_ulps;

static void *
grid_worker(void *arg)
{
	static const float4 k1s[] = {0, 1e-6f, 0.5f, 1.2f, 2.0f, 1000.0f};
	static const float4 bs[] = {0, 1e-6f, 0.25f, 0.75f, 1.0f};
	static const float4 avgs[] = {3.6e-15f, 0.5f, 1, 37.25f, 1000, 65535, 1e9f};
	const int	npages = 65536 / SIFT_KERN_SLOTS;
	uint64	   *mask = kt_alloc(npages * 64);
	uint16	   *dlv = kt_alloc(65536 * 2);
	const uint16 *rows[65536 / SIFT_KERN_SLOTS];
	float4	   *kn = kt_alloc(65536 * 4),
			   *ref = kt_alloc(65536 * 4),
			   *out = kt_alloc(65536 * 4);
	uint8	   *tf = kt_alloc(65536 + SIFT_KERN_PAD);
	double		worst = 0;
	int			g;

	memset(mask, 0xFF, npages * 64);
	for (int i = 0; i < 65536; i++)
		dlv[i] = i;
	for (int p = 0; p < npages; p++)
		rows[p] = dlv + p * SIFT_KERN_SLOTS;
	while ((g = atomic_fetch_add(&grid_next, 1) * grid_stride) < (int) (lengthof(k1s) * lengthof(bs) * lengthof(avgs)))
	{
		SiftBm25	bm;
		float4		w = 7.3f;

		sift_kern_bm25_init(&bm, k1s[g % 6], bs[g / 6 % 5], avgs[g / 30]);
		sift_kernels_scalar.knorm(kn, rows, mask, npages, &bm);
		for (int t = 1; t < ntabs; t++)
		{
			tabs[t].k->knorm(out, rows, mask, npages, &bm);
			if (memcmp(out, kn, 65536 * 4))
				FAIL("grid knorm mismatch on %s\n", tabs[t].name);
		}
		for (int tfv = 0; tfv < 256; tfv++)
		{
			memset(tf, tfv, 65536);
			memset(ref, 0, 65536 * 4);
			sift_kernels_scalar.bm25_accum(ref, mask, npages, tf, kn, w);
			for (int t = 1; t < ntabs; t++)
			{
				memset(out, 0, 65536 * 4);
				tabs[t].k->bm25_accum(out, mask, npages, tf, kn, w);
				if (memcmp(out, ref, 65536 * 4))
					FAIL("grid bm25 mismatch on %s (tf %d)\n", tabs[t].name, tfv);
			}
			for (int dl = 0; dl < 65536 && tfv; dl++)
			{
				double		k = (double) bm.k1 * (1.0 - bm.b + (double) bm.b * dl / bm.avgdl),
							exact = w * (tfv / (tfv + k)),
							ulps = fabs(ref[dl] - exact) / (exact * 0x1p-24);

				if (!isfinite(ref[dl]) || ulps > 7.0001)
					FAIL("grid error %.3f ulp: k1 %g b %g avgdl %g tf %d dl %d\n", ulps, bm.k1, bm.b, bm.avgdl, tfv, dl);
				worst = Max(worst, ulps);
			}
			atomic_fetch_add(&nlanes, 65536L * ntabs);
			atomic_fetch_add(&ncalls, ntabs);
		}
	}
	for (double cur = max_ulps; worst > cur && !atomic_compare_exchange_weak(&max_ulps, &cur, worst);)
		;
	free(mask), free(dlv), free(kn), free(ref), free(out), free(tf);
	return NULL;
}

static void
run(void *(*fn) (void *), const char *what)
{
	pthread_t	th[NTHREADS];

	atomic_store(&ncalls, 0);
	atomic_store(&nlanes, 0);
	for (long i = 0; i < NTHREADS; i++)
		pthread_create(&th[i], NULL, fn, (void *) (i + 1));
	for (int i = 0; i < NTHREADS; i++)
		pthread_join(th[i], NULL);
	printf("%-8s %12ld calls %14ld lanes  failures so far %ld\n", what,
		   atomic_load(&ncalls), atomic_load(&nlanes), atomic_load(&nfail));
}

int
main(int argc, char **argv)
{
	double		millions = argc > 1 ? atof(argv[1]) : 2;

	if (argc > 2)
		grid_stride = atoi(argv[2]);

	kt_cpu_init();
	tabs[ntabs++] = (Tab) {"scalar", &sift_kernels_scalar};
#if defined(__x86_64__)
	if (sift_kernels_for(SIFT_SIMD_AVX2))
	{
		tabs[ntabs++] = (Tab) {"avx2", &sift_kernels_avx2};
		tabs[ntabs++] = (Tab) {"avx2-nopdep", &sift_kernels_avx2_nopdep};
	}
	if (sift_kernels_for(SIFT_SIMD_AVX512))
		tabs[ntabs++] = (Tab) {"avx512", &sift_kernels_avx512};
#endif
	printf("levels:");
	for (int t = 0; t < ntabs; t++)
		printf(" %s", tabs[t].name);
	printf("\n");
	iters = (long) (millions * 1e6 / NTHREADS);
	run(worker, "random");
	run(grid_worker, "grid");
	printf("max error vs double: %.3f ulp (bound 7)\n", max_ulps);
	printf("%s\n", nfail ? "FAILED" : "OK");
	return nfail != 0;
}
