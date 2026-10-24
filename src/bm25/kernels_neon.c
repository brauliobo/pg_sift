/*
 * bm25/kernels_neon.c - AArch64 Advanced SIMD kernels. An 8-slot byte of a mask word is two registers of floats; only nonzero bytes
 * are visited. Rank-order tf bytes are placed into slot lanes with TBL from the 256-entry expand table (an index of 0x80 gives 0),
 * and slots outside the mask are blended back unchanged. Slots without a NEON kernel are the scalar functions: expand, tf_decode, select_gt and vm_allvisible
 * (a deposit, a variable length decode, a compaction and a bit gather, none of which NEON has an instruction for), ub_accum,
 * which the compiler vectorizes as well as the intrinsics do, and bm25_sel, which the intrinsics lose on (docs/KERNELS.md).
 */
#include "core/host.h"

#include "bm25/kernels_int.h"
#include "core/neon.h"

#if defined(SIFT_HAVE_NEON)

/* Bitmap of the nonzero bytes of one page mask */
static inline uint64
bytes_nz(const uint64 *pm)
{
	const uint8 *b = (const uint8 *) pm;

	return ~sift_neon_movemask64(vceqzq_u8(vld1q_u8(b)), vceqzq_u8(vld1q_u8(b + 16)), vceqzq_u8(vld1q_u8(b + 32)), vceqzq_u8(vld1q_u8(b + 48)));
}

/* Visits the nonzero 8-slot bytes of mask: s = first slot, k = byte */
#define FOREACH_BYTE(mask, npages, s, k, body) \
	for (int p_ = 0; p_ < (npages); p_++) \
		for (uint64 nz_ = bytes_nz((mask) + p_ * SIFT_OFFMASK_WORDS); nz_; nz_ &= nz_ - 1) \
		{ \
			int			s = p_ * SIFT_KERN_SLOTS + __builtin_ctzll(nz_) * 8; \
			uint32		k = ((mask)[s >> 6] >> (s & 63)) & 0xFF; \
			body; \
		}

/* All-ones float lanes for the set bits of a nibble */
static inline uint32x4_t
lanes4(uint32 k)
{
	static const uint32 bit[4] = {1, 2, 4, 8};

	return vtstq_u32(vdupq_n_u32(k), vld1q_u32(bit));
}

/* tf of the byte's lanes as floats: the next popcount(k) bytes of tf in lane order, 0 elsewhere */
static inline void
byte_tf(const uint8 *tf, uint32 k, float32x4_t *t0, float32x4_t *t1)
{
	const uint16x8_t t = vmovl_u8(vqtbl1_u8(vcombine_u8(vld1_u8(tf), vdup_n_u8(0)), vld1_u8(sift_kern_lut_expand[k])));

	*t0 = vcvtq_f32_u32(vmovl_u16(vget_low_u16(t)));
	*t1 = vcvtq_f32_u32(vmovl_high_u16(t));
}

/* scores[0 .. 4) += w * (t / (t + knorm)) in the lanes of l; t is 1 in the others */
static inline void
accum4(float4 *scores, const float4 *knorm, float32x4_t w, float32x4_t t, uint32 k)
{
	const uint32x4_t l = lanes4(k);
	const float32x4_t s = vld1q_f32(scores),
				u = vbslq_f32(l, t, vdupq_n_f32(1.0f)),
				n = vreinterpretq_f32_u32(vandq_u32(l, vreinterpretq_u32_f32(vld1q_f32(knorm))));

	vst1q_f32(scores, vbslq_f32(l, vaddq_f32(s, vmulq_f32(w, vdivq_f32(u, vaddq_f32(u, n)))), s));
}

/* A byte with this many slots or fewer costs less as the scalar sequence (a four lane division is not four times a scalar one) */
#define SIFT_KERN_NEON_SPARSE	2

static inline void
accum8(float4 *scores, const float4 *knorm, float4 w, const uint8 *tf, uint32 k)
{
	if (__builtin_popcount(k) <= SIFT_KERN_NEON_SPARSE)
	{
		for (uint32 m = k, j = 0; m; m &= m - 1, j++)
		{
			const int	b = __builtin_ctz(m);

			scores[b] += sift_kern_bm25(w, tf ? tf[j] : 1, knorm[b]);
		}
		return;
	}
	{
		float32x4_t t0 = vdupq_n_f32(1.0f);
		float32x4_t t1 = t0;
		const float32x4_t vw = vdupq_n_f32(w);

		if (tf)
			byte_tf(tf, k, &t0, &t1);
		if (k & 15)
			accum4(scores, knorm, vw, t0, k & 15);
		if (k >> 4)
			accum4(scores + 4, knorm + 4, vw, t1, k >> 4);
	}
}

static int
bm25_accum_neon(float4 *scores, const uint64 *mask, int npages, const uint8 *tf, const float4 *knorm, float4 w)
{
	int			r = 0;

	FOREACH_BYTE(mask, npages, s, k,
				 accum8(scores + s, knorm + s, w, tf ? tf + r : NULL, k);
				 r += __builtin_popcount(k));
	return r;
}

/* The 8 lanes of two masks as the bits of a byte */
static inline uint32
lane_bits(uint32x4_t m0, uint32x4_t m1)
{
	static const uint8 weight[8] = {1, 2, 4, 8, 16, 32, 64, 128};

	return vaddv_u8(vand_u8(vmovn_u16(vuzp1q_u16(vreinterpretq_u16_u32(m0), vreinterpretq_u16_u32(m1))), vld1_u8(weight)));
}

static int
prune_gt_neon(uint64 *cand, const float4 *scores, int npages, float4 thr)
{
	const float32x4_t th = vdupq_n_f32(thr);
	int			n = 0;

	FOREACH_BYTE(cand, npages, s, k,
				 const uint32 g = lane_bits(vcgtq_f32(vld1q_f32(scores + s), th), vcgtq_f32(vld1q_f32(scores + s + 4), th)) & k;

				 cand[s >> 6] ^= (uint64) (k ^ g) << (s & 63);
				 n += __builtin_popcount(g));
	return n;
}

static void
max_merge_neon(float4 *scores, const float4 *other, const uint64 *mask, int npages)
{
	FOREACH_BYTE(mask, npages, s, k,
				 const float32x4_t a0 = vld1q_f32(scores + s);
				 const float32x4_t a1 = vld1q_f32(scores + s + 4);
				 const float32x4_t b0 = vld1q_f32(other + s);
				 const float32x4_t b1 = vld1q_f32(other + s + 4);

				 vst1q_f32(scores + s, vbslq_f32(lanes4(k & 15), vbslq_f32(vcgtq_f32(a0, b0), a0, b0), a0));
				 vst1q_f32(scores + s + 4, vbslq_f32(lanes4(k >> 4), vbslq_f32(vcgtq_f32(a1, b1), a1, b1), a1)));
}

static void
knorm_neon(float4 *knorm, const uint16 *const *dl, const uint64 *mask, int npages, const SiftBm25 *bm)
{
	const float32x4_t c0 = vdupq_n_f32(bm->c0),
				c1 = vdupq_n_f32(bm->c1);

	for (int p = 0; p < npages; p++)
	{
		const uint8 *pb = (const uint8 *) (mask + p * SIFT_OFFMASK_WORDS);
		float4	   *kp = knorm + p * SIFT_KERN_SLOTS;
		uint64		nz = bytes_nz(mask + p * SIFT_OFFMASK_WORDS);
		int			hb,
					last;

		if (!nz)
			continue;
		/* the row ends at its highest offset in use: nothing past it is read */
		hb = 63 - __builtin_clzll(nz);
		last = hb * 8 + 31 - __builtin_clz(pb[hb]);
		for (; nz; nz &= nz - 1)
		{
			const int	s = __builtin_ctzll(nz) * 8;
			uint32		k = pb[s / 8];
			uint16x8_t	d;

			if (s + 7 <= last)
				d = vld1q_u16(dl[p] + s);
			else
			{
				uint16		tmp[8] = {0};

				for (uint32 m = k; m; m &= m - 1)
					tmp[__builtin_ctz(m)] = dl[p][s + __builtin_ctz(m)];
				d = vld1q_u16(tmp);
			}
			vst1q_f32(kp + s, vbslq_f32(lanes4(k & 15), vaddq_f32(c0, vmulq_f32(c1, vcvtq_f32_u32(vmovl_u16(vget_low_u16(d))))), vld1q_f32(kp + s)));
			vst1q_f32(kp + s + 4, vbslq_f32(lanes4(k >> 4), vaddq_f32(c0, vmulq_f32(c1, vcvtq_f32_u32(vmovl_high_u16(d)))), vld1q_f32(kp + s + 4)));
		}
	}
}

const SiftKernels sift_kernels_neon = {
	.expand = sift_kern_expand_scalar,
	.tf_decode = sift_kern_tf_decode_scalar,
	.knorm = knorm_neon,
	.bm25_accum = bm25_accum_neon,
	.bm25_sel = sift_kern_bm25_sel_scalar,
	.prune_gt = prune_gt_neon,
	.max_merge = max_merge_neon,
	.ub_accum = sift_kern_ub_accum_scalar,
	.select_gt = sift_kern_select_gt_scalar,
	.vm_allvisible = sift_kern_vm_scalar,
};

#endif
