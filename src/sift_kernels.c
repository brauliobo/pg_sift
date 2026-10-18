/*
 * sift_kernels.c - scalar reference kernels (the specification every SIMD
 * level matches bit for bit), dispatch, BM25 constants, bounds, tf stream
 * encoding and the top-k heap.
 */
#include "sift_host.h"

#include <math.h>

#include "sift_kernels_int.h"
#include "sift_popcount.h"

const SiftKernels *sift_kern = &sift_kernels_scalar;

int
sift_kern_expand_scalar(uint64 *out, const uint64 *base, const uint64 *rankbits, int nwords)
{
	uint32		pos = 0;

	for (int i = 0; i < nwords; i++)
	{
		uint64		m = base[i],
					src = sift_kern_rank_bits(rankbits, pos, sift_pc64(m)),
					r = 0;

		pos += sift_pc64(m);
		for (; m; m &= m - 1, src >>= 1)
			r |= (src & 1) ? m & -m : 0;
		out[i] = r;
	}
	return pos;
}

static void
tf_decode_scalar(uint8 *tf, const uint8 *l1, const uint8 *nib, const uint8 *esc,
				 SiftTfPos *pos, uint32 count)
{
	for (uint32 i = 0; i < count; i++, pos->rank++)
	{
		uint32		n;

		if (!((l1[pos->rank >> 3] >> (pos->rank & 7)) & 1))
		{
			tf[i] = 1;
			continue;
		}
		n = (nib[pos->nib >> 1] >> ((pos->nib & 1) * 4)) & 15;
		pos->nib++;
		tf[i] = n < 15 ? n + 2 : sift_kern_esc_tf(esc[pos->esc++]);
	}
}

/* Calls body with slot = page * SIFT_KERN_SLOTS + off - 1 for every bit of mask, in order */
#define FOREACH_SLOT(mask, npages, slot, body) \
	for (int w_ = 0; w_ < SIFT_KERN_WORDS(npages); w_++) \
		for (uint64 m_ = (mask)[w_]; m_; m_ &= m_ - 1) \
		{ \
			int			slot = w_ * 64 + __builtin_ctzll(m_); \
			body; \
		}

static void
knorm_scalar(float4 *knorm, const uint16 *const *dl, const uint64 *mask, int npages,
			 const SiftBm25 *bm)
{
	FOREACH_SLOT(mask, npages, s,
				 knorm[s] = sift_kern_knorm1(bm, dl[s / SIFT_KERN_SLOTS][s % SIFT_KERN_SLOTS]));
}

static int
bm25_accum_scalar(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
				  const float4 *knorm, float4 w)
{
	int			r = 0;

	FOREACH_SLOT(mask, npages, s,
				 scores[s] += sift_kern_bm25(w, tf ? tf[r] : 1, knorm[s]);
				 r++);
	return r;
}

static int
bm25_sel_scalar(float4 *scores, const uint64 *mask, const uint64 *cand, int npages, const uint8 *tf,
				const float4 *knorm, float4 w)
{
	int			r = 0;

	FOREACH_SLOT(mask, npages, s,
				 if (cand[s >> 6] >> (s & 63) & 1)
					 scores[s] += sift_kern_bm25(w, tf ? tf[r] : 1, knorm[s]);
				 r++);
	return r;
}

static int
prune_gt_scalar(uint64 *cand, const float4 *scores, int npages, float4 thr)
{
	int			n = 0;

	FOREACH_SLOT(cand, npages, s,
				 if (scores[s] > thr)
					 n++;
				 else
					 cand[s >> 6] &= ~(UINT64CONST(1) << (s & 63)));
	return n;
}

static void
max_merge_scalar(float4 *scores, const float4 *other, const uint64 *mask, int npages)
{
	FOREACH_SLOT(mask, npages, s,
				 scores[s] = scores[s] > other[s] ? scores[s] : other[s]);
}

static void
ub_accum_scalar(float4 *ub, const uint8 *bounds, float4 w, int n)
{
	for (int i = 0; i < n; i++)
		ub[i] += w * (float4) bounds[i];
}

static int
select_gt_scalar(uint16 *slot, float4 *vals, const float4 *scores, const uint64 *mask,
				 int npages, float4 theta)
{
	int			n = 0;

	FOREACH_SLOT(mask, npages, s,
				 if (scores[s] > theta)
				 {
					 slot[n] = s;
					 vals[n++] = scores[s];
				 });
	return n;
}

/* The even bits of x, packed into its low half */
static inline uint64
even_bits(uint64 x)
{
	x &= UINT64CONST(0x5555555555555555);
	x = (x | x >> 1) & UINT64CONST(0x3333333333333333);
	x = (x | x >> 2) & UINT64CONST(0x0F0F0F0F0F0F0F0F);
	x = (x | x >> 4) & UINT64CONST(0x00FF00FF00FF00FF);
	x = (x | x >> 8) & UINT64CONST(0x0000FFFF0000FFFF);
	return (x | x >> 16) & UINT64CONST(0x00000000FFFFFFFF);
}

void
sift_kern_vm_scalar(uint64 *pagemask, const uint8 *vmbytes)
{
	for (int i = 0; i < SIFT_PAGEMASK_WORDS; i++)
		pagemask[i] = even_bits(sift_kern_ld64(vmbytes + 16 * i)) |
			even_bits(sift_kern_ld64(vmbytes + 16 * i + 8)) << 32;
}

const SiftKernels sift_kernels_scalar = {
	.expand = sift_kern_expand_scalar,
	.tf_decode = tf_decode_scalar,
	.knorm = knorm_scalar,
	.bm25_accum = bm25_accum_scalar,
	.bm25_sel = bm25_sel_scalar,
	.prune_gt = prune_gt_scalar,
	.max_merge = max_merge_scalar,
	.ub_accum = ub_accum_scalar,
	.select_gt = select_gt_scalar,
	.vm_allvisible = sift_kern_vm_scalar,
};

const SiftKernels *
sift_kernels_for(SiftSimdLevel level)
{
	if (level > sift_cpu.detected)
		return NULL;
#if defined(__x86_64__)
	if (level == SIFT_SIMD_AVX512)
		return &sift_kernels_avx512;
	if (level == SIFT_SIMD_AVX2)
		return sift_cpu.flags & SIFT_CPU_BMI2_FAST ? &sift_kernels_avx2 : &sift_kernels_avx2_nopdep;
#endif
	return &sift_kernels_scalar;
}

static void
kernels_rebind(const SiftCpu *cpu)
{
	sift_kern = sift_kernels_for(cpu->active);
}

void
sift_kernels_init(void)
{
	sift_cpu_register(kernels_rebind);
}

void
sift_kern_bm25_init(SiftBm25 *bm, float4 k1, float4 b, float4 avgdl)
{
	bm->k1 = k1;
	bm->b = b;
	bm->avgdl = avgdl;
	bm->c0 = k1 * (1.0f - b);
	bm->c1 = (k1 * b) / avgdl;
}

float4
sift_kern_idf(double ndocs, double df)
{
	return (float4) log(1.0 + (ndocs - df + 0.5) / (df + 0.5));
}

float4
sift_kern_term_weight(float4 boost, float4 idf, float4 k1)
{
	return (boost * idf) * (k1 + 1.0f);
}

uint8
sift_kern_bound_u8(const SiftBm25 *ref, uint32 tf, uint32 dl)
{
	double		k = (double) ref->k1 * (1.0 - ref->b + (double) ref->b * dl / ref->avgdl);
	double		q = ceil(255.0 * tf / (tf + k));

	return q < 1 ? 1 : q > 255 ? 255 : (uint8) q;
}

float4
sift_kern_ub_weight(float4 w, double avgdl_now, double avgdl_ref, int nterms)
{
	double		c = Max(1.0, avgdl_now / avgdl_ref);
	double		slack = Max(SIFT_KERN_UB_SLACK, nterms * SIFT_KERN_UB_SLACK_TERM);
	double		d = w * c / 255.0 * (1.0 + slack);
	float4		f = (float4) d;

	return (double) f < d ? nextafterf(f, INFINITY) : f;
}

void
sift_kern_tf_encode(uint8 *l1, uint8 *nib, uint8 *esc, SiftTfPos *pos,
					const uint8 *tf, uint32 count)
{
	for (uint32 i = 0; i < count; i++, pos->rank++)
	{
		uint32		n;

		if (tf[i] <= 1)
			continue;
		l1[pos->rank >> 3] |= 1 << (pos->rank & 7);
		n = Min(tf[i] - 2, 15);
		nib[pos->nib >> 1] |= n << ((pos->nib & 1) * 4);
		pos->nib++;
		if (n == 15)
			esc[pos->esc++] = tf[i] - 17;
	}
}

void
sift_kern_topk_init(SiftTopK *t, SiftHit *buf, int64 cap, int64 k)
{
	t->hit = buf;
	t->cap = cap;
	t->k = k;
	t->n = 0;
}

float4
sift_kern_topk_theta(const SiftTopK *t)
{
	return t->n < t->k ? -INFINITY : nextafterf(t->hit[0].score, -INFINITY);
}

/* Restores the min-heap (worst at the root) below slot i of h[0..n) */
static void
sift_down(SiftHit *h, int64 n, int64 i)
{
	SiftHit		x = h[i];

	for (int64 c; (c = 2 * i + 1) < n; i = c)
	{
		if (c + 1 < n && sift_hit_better(h[c].score, h[c].key, &h[c + 1]))
			c++;
		if (!sift_hit_better(x.score, x.key, &h[c]))
			break;
		h[i] = h[c];
	}
	h[i] = x;
}

void
sift_kern_topk_push(SiftTopK *t, float4 s, SiftKey key)
{
	SiftHit    *h = t->hit;
	int64		i;

	if (t->n == t->k)
	{
		h[0] = (SiftHit) {.key = key,.score = s};
		sift_down(h, t->n, 0);
		return;
	}
	for (i = t->n++; i > 0 && sift_hit_better(h[(i - 1) / 2].score, h[(i - 1) / 2].key,
											  &(SiftHit) {.key = key,.score = s}); i = (i - 1) / 2)
		h[i] = h[(i - 1) / 2];
	h[i] = (SiftHit) {.key = key,.score = s};
}

int64
sift_kern_topk_sort(SiftTopK *t)
{
	for (int64 i = t->n - 1; i > 0; i--)
	{
		SiftHit		worst = t->hit[0];

		t->hit[0] = t->hit[i];
		t->hit[i] = worst;
		sift_down(t->hit, i, 0);
	}
	return t->n;
}
