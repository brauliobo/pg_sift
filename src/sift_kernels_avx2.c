/*
 * sift_kernels_avx2.c - AVX2 kernels. An 8-slot byte of a mask word is one
 * ymm of floats; only nonzero bytes are visited. Rank-order tf bytes are
 * placed into slot lanes with PSHUFB from a 256-entry expand table, and
 * selected lanes are packed with VPERMPS from a compress table. Slots
 * outside the mask are blended back unchanged.
 */
#include "sift_host.h"

#include "sift_kernels_int.h"

#if defined(__x86_64__)
#include <immintrin.h>

#define K2 static inline __attribute__((always_inline)) SIFT_TARGET_AVX2

#define R4(F, m)	F(m), F(m + 1), F(m + 2), F(m + 3)
#define R16(F, m)	R4(F, m), R4(F, m + 4), R4(F, m + 8), R4(F, m + 12)
#define R64(F, m)	R16(F, m), R16(F, m + 16), R16(F, m + 32), R16(F, m + 48)
#define R256(F)		R64(F, 0), R64(F, 64), R64(F, 128), R64(F, 192)
#define ROW8(F, m)	{F(m, 0), F(m, 1), F(m, 2), F(m, 3), F(m, 4), F(m, 5), F(m, 6), F(m, 7)}
#define BELOW(m, j)	__builtin_popcount((m) & ((1 << (j)) - 1))

/* Lane j of row m: rank of bit j among the set bits of m, 0x80 (PSHUFB zero) if clear */
#define XL(m, j)	((((m) >> (j)) & 1) ? BELOW(m, j) : 0x80)
#define XROW(m)		ROW8(XL, m)
const uint8 sift_kern_lut_expand[256][8] = {R256(XROW)};

/* Lane j of row m: position of the j-th set bit of m (0 past the last) */
#define CB(m, j, b)	((((m) >> (b)) & 1) && BELOW(m, b) == (j) ? (b) : 0)
#define CL(m, j)	(CB(m, j, 0) + CB(m, j, 1) + CB(m, j, 2) + CB(m, j, 3) + \
					 CB(m, j, 4) + CB(m, j, 5) + CB(m, j, 6) + CB(m, j, 7))
#define CROW(m)		ROW8(CL, m)
static const uint8 lut_compress[256][8] = {R256(CROW)};

/* Bitmap of the nonzero bytes of one page mask */
K2 uint64
bytes_nz(const uint64 *pm)
{
	const __m256i z = _mm256_setzero_si256();
	uint32		lo = _mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *) pm), z)),
				hi = _mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *) (pm + 4)), z));

	return ~((uint64) hi << 32 | lo);
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

/* All-ones float lanes for the set bits of k */
K2 __m256
lanes(uint32 k)
{
	const __m256i bit = _mm256_setr_epi32(1, 2, 4, 8, 16, 32, 64, 128);

	return _mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_and_si256(_mm256_set1_epi32(k), bit), bit));
}

K2 __m128i
lut(const uint8 (*t)[8], uint32 k)
{
	return _mm_loadl_epi64((const __m128i *) t[k]);
}

/* tf of the byte's lanes: the next popcount(k) bytes of tf in lane order, 0 elsewhere */
K2 __m256
byte_tf(const uint8 *tf, uint32 k)
{
	__m128i		t = _mm_shuffle_epi8(_mm_loadl_epi64((const __m128i *) tf), lut(sift_kern_lut_expand, k));

	return _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(t));
}

K2 int
bm25_accum_tmpl(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
				const float4 *knorm, float4 w, const bool tf1)
{
	const __m256 vw = _mm256_set1_ps(w),
				one = _mm256_set1_ps(1.0f);
	int			r = 0;

	FOREACH_BYTE(mask, npages, s, k,
				 __m256 lm = lanes(k);
				 __m256 t = tf1 ? one : _mm256_or_ps(_mm256_andnot_ps(lm, one), byte_tf(tf + r, k));
				 __m256 x = _mm256_div_ps(t, _mm256_add_ps(t, _mm256_and_ps(lm, _mm256_load_ps(knorm + s))));
				 __m256 sc = _mm256_load_ps(scores + s);
				 _mm256_store_ps(scores + s, _mm256_blendv_ps(sc, _mm256_add_ps(sc, _mm256_mul_ps(vw, x)), lm));
				 r += __builtin_popcount(k));
	return r;
}

static SIFT_TARGET_AVX2 int
bm25_accum_avx2(float4 *scores, const uint64 *mask, int npages, const uint8 *tf,
				const float4 *knorm, float4 w)
{
	return tf ? bm25_accum_tmpl(scores, mask, npages, tf, knorm, w, false) :
		bm25_accum_tmpl(scores, mask, npages, tf, knorm, w, true);
}

/* The lanes of the byte at slot s that cand keeps */
K2 uint32
byte_cand(const uint64 *cand, int s, uint32 k)
{
	return k & (cand[s >> 6] >> (s & 63));
}

static SIFT_TARGET_AVX2 int
bm25_sel_avx2(float4 *scores, const uint64 *mask, const uint64 *cand, int npages, const uint8 *tf,
			  const float4 *knorm, float4 w)
{
	const __m256 vw = _mm256_set1_ps(w),
				one = _mm256_set1_ps(1.0f);
	int			r = 0;

	FOREACH_BYTE(mask, npages, s, k,
				 uint32 kc = byte_cand(cand, s, k);
				 if (kc)
				 {
					 __m256 lm = lanes(k);
					 __m256 lc = lanes(kc);
					 __m256 t =tf ? _mm256_or_ps(_mm256_andnot_ps(lm, one), byte_tf(tf + r, k)) : one;
					 __m256 x = _mm256_div_ps(t, _mm256_add_ps(t, _mm256_and_ps(lc, _mm256_load_ps(knorm + s))));
					 __m256 sc = _mm256_load_ps(scores + s);

					 _mm256_store_ps(scores + s, _mm256_blendv_ps(sc, _mm256_add_ps(sc, _mm256_mul_ps(vw, x)), lc));
				 }
				 r += __builtin_popcount(k));
	return r;
}

static SIFT_TARGET_AVX2 int
prune_gt_avx2(uint64 *cand, const float4 *scores, int npages, float4 thr)
{
	const __m256 th = _mm256_set1_ps(thr);
	int			n = 0;

	FOREACH_BYTE(cand, npages, s, k,
				 uint32 g = _mm256_movemask_ps(_mm256_cmp_ps(_mm256_load_ps(scores + s), th, _CMP_GT_OQ)) & k;

				 cand[s >> 6] ^= (uint64) (k ^ g) << (s & 63);
				 n += __builtin_popcount(g));
	return n;
}

/* PSHUFB control moving u16 lanes down by row index: lane i takes lane i + sh, zero past 7 */
#define SH(sh, j)	(((j) / 2 + (sh) < 8) ? (j) + 2 * (sh) : 0x80)
#define SHROW(sh)	{SH(sh, 0), SH(sh, 1), SH(sh, 2), SH(sh, 3), SH(sh, 4), SH(sh, 5), SH(sh, 6), SH(sh, 7), \
					 SH(sh, 8), SH(sh, 9), SH(sh, 10), SH(sh, 11), SH(sh, 12), SH(sh, 13), SH(sh, 14), SH(sh, 15)}
static const uint8 lut_shift16[8][16] = {SHROW(0), SHROW(1), SHROW(2), SHROW(3), SHROW(4), SHROW(5), SHROW(6), SHROW(7)};

/* row[b .. b + 8) where entries past last (the highest offset in use) may be garbage */
K2 __m128i
dl8(const uint16 *row, int b, uint32 k, int last)
{
	uint16		tmp[8] = {0};

	if (b + 7 <= last)
		return _mm_loadu_si128((const __m128i *) (row + b));
	if (last >= 7)
		return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *) (row + last - 7)),
								_mm_loadu_si128((const __m128i *) lut_shift16[b + 7 - last]));
	for (; k; k &= k - 1)
		tmp[__builtin_ctz(k)] = row[b + __builtin_ctz(k)];
	return _mm_loadu_si128((const __m128i *) tmp);
}

static SIFT_TARGET_AVX2 void
knorm_avx2(float4 *knorm, const uint16 *const *dl, const uint64 *mask, int npages,
		   const SiftBm25 *bm)
{
	const __m256 c0 = _mm256_set1_ps(bm->c0),
				c1 = _mm256_set1_ps(bm->c1);

	for (int p = 0; p < npages; p++)
	{
		const uint8 *pb = (const uint8 *) (mask + p * SIFT_OFFMASK_WORDS);
		float4	   *kp = knorm + p * SIFT_KERN_SLOTS;
		uint64		nz = bytes_nz(mask + p * SIFT_OFFMASK_WORDS);
		int			hb,
					last;

		if (!nz)
			continue;
		hb = 63 - __builtin_clzll(nz);
		last = hb * 8 + 31 - __builtin_clz(pb[hb]);
		for (; nz; nz &= nz - 1)
		{
			int			s = __builtin_ctzll(nz) * 8;
			uint32		k = pb[s / 8];
			__m256		f = _mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(dl8(dl[p], s, k, last)));

			_mm256_store_ps(kp + s, _mm256_blendv_ps(_mm256_load_ps(kp + s), _mm256_add_ps(c0, _mm256_mul_ps(c1, f)), lanes(k)));
		}
	}
}

static SIFT_TARGET_AVX2 void
max_merge_avx2(float4 *scores, const float4 *other, const uint64 *mask, int npages)
{
	FOREACH_BYTE(mask, npages, s, k,
				 __m256 sc = _mm256_load_ps(scores + s);
				 _mm256_store_ps(scores + s, _mm256_blendv_ps(sc, _mm256_max_ps(sc, _mm256_load_ps(other + s)), lanes(k))));
}

static SIFT_TARGET_AVX2 void
ub_accum_avx2(float4 *ub, const uint8 *bounds, float4 w, int n)
{
	const __m256 vw = _mm256_set1_ps(w);
	int			i = 0;

	for (; i + 8 <= n; i += 8)
	{
		__m256		b = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *) (bounds + i))));

		_mm256_storeu_ps(ub + i, _mm256_add_ps(_mm256_loadu_ps(ub + i), _mm256_mul_ps(vw, b)));
	}
	for (; i < n; i++)
		ub[i] += w * (float4) bounds[i];
}

static SIFT_TARGET_AVX2 int
select_gt_avx2(uint16 *slot, float4 *vals, const float4 *scores, const uint64 *mask,
			   int npages, float4 theta)
{
	const __m256 th = _mm256_set1_ps(theta);
	int			n = 0;

	FOREACH_BYTE(mask, npages, s, k,
				 __m256 v = _mm256_load_ps(scores + s);
				 uint32 g = _mm256_movemask_ps(_mm256_cmp_ps(v, th, _CMP_GT_OQ)) & k;
				 if (g)
				 {
					 __m128i ci = lut(lut_compress, g);

					 _mm256_storeu_ps(vals + n, _mm256_permutevar8x32_ps(v, _mm256_cvtepu8_epi32(ci)));
					 _mm_storeu_si128((__m128i *) (slot + n), _mm_add_epi16(_mm_cvtepu8_epi16(ci), _mm_set1_epi16(s)));
					 n += __builtin_popcount(g);
				 });
	return n;
}

/* The next 64 nibbles from nibble index nb, each plus 2, one per byte, into out[64] */
K2 void
nibbles64(uint8 *out, const uint8 *nib, uint32 nb)
{
	const uint8 *p = nib + (nb >> 1);
	const __m256i lo = _mm256_set1_epi16(0x0F0F),
				two = _mm256_set1_epi8(2);
	__m256i		x = _mm256_loadu_si256((const __m256i *) p);
	__m256i		w0,
				w1;

	if (nb & 1)
		x = _mm256_or_si256(_mm256_srli_epi64(x, 4),
							_mm256_slli_epi64(_mm256_loadu_si256((const __m256i *) (p + 8)), 60));
	w0 = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(x));
	w1 = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(x, 1));
	_mm256_storeu_si256((__m256i *) out, _mm256_add_epi8(_mm256_and_si256(_mm256_or_si256(w0, _mm256_slli_epi16(w0, 4)), lo), two));
	_mm256_storeu_si256((__m256i *) (out + 32), _mm256_add_epi8(_mm256_and_si256(_mm256_or_si256(w1, _mm256_slli_epi16(w1, 4)), lo), two));
}

static SIFT_TARGET_AVX2 void
tf_decode_avx2(uint8 *tf, const uint8 *l1, const uint8 *nib, const uint8 *esc,
			   SiftTfPos *pos, uint32 count)
{
	const __m128i one = _mm_set1_epi8(1),
				x17 = _mm_set1_epi8(17);
	uint8		nibs[64 + 16];

	for (uint32 i = 0; i < count; i += 64)
	{
		uint64		bits = _bzhi_u64(sift_kern_bits64(l1, (uint64) pos->rank + i), Min(count - i, 64));
		uint32		k = 0;

		if (!bits)
		{
			_mm256_storeu_si256((__m256i *) (tf + i), _mm256_set1_epi8(1));
			_mm256_storeu_si256((__m256i *) (tf + i + 32), _mm256_set1_epi8(1));
			continue;
		}
		nibbles64(nibs, nib, pos->nib);
		pos->nib += _mm_popcnt_u64(bits);
		for (int j = 0; j < 64; j += 8)
		{
			uint32		b = (bits >> j) & 0xFF;
			__m128i		v = _mm_max_epu8(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i *) (nibs + k)), lut(sift_kern_lut_expand, b)), one);

			_mm_storel_epi64((__m128i *) (tf + i + j), v);
			k += __builtin_popcount(b);
			for (uint32 em = _mm_movemask_epi8(_mm_cmpeq_epi8(v, x17)) & b; em; em &= em - 1)
				tf[i + j + __builtin_ctz(em)] = sift_kern_esc_tf(esc[pos->esc++]);
		}
	}
	pos->rank += count;
}

SIFT_TARGET_AVX2 int
sift_kern_expand_pdep(uint64 *out, const uint64 *base, const uint64 *rankbits, int nwords)
{
	uint32		pos = 0;

	for (int i = 0; i < nwords; i++)
	{
		int			n = _mm_popcnt_u64(base[i]);

		out[i] = _pdep_u64(sift_kern_rank_bits(rankbits, pos, n), base[i]);
		pos += n;
	}
	return pos;
}

SIFT_TARGET_AVX2 void
sift_kern_vm_pext(uint64 *pagemask, const uint8 *vmbytes)
{
	const uint64 even = UINT64CONST(0x5555555555555555);

	for (int i = 0; i < SIFT_PAGEMASK_WORDS; i++)
		pagemask[i] = _pext_u64(sift_kern_ld64(vmbytes + 16 * i), even) |
			_pext_u64(sift_kern_ld64(vmbytes + 16 * i + 8), even) << 32;
}

#define AVX2_TABLE(expand_fn, vm_fn) { \
	.expand = expand_fn, \
	.tf_decode = tf_decode_avx2, \
	.knorm = knorm_avx2, \
	.bm25_accum = bm25_accum_avx2, \
	.bm25_sel = bm25_sel_avx2, \
	.prune_gt = prune_gt_avx2, \
	.max_merge = max_merge_avx2, \
	.ub_accum = ub_accum_avx2, \
	.select_gt = select_gt_avx2, \
	.vm_allvisible = vm_fn, \
}

const SiftKernels sift_kernels_avx2 = AVX2_TABLE(sift_kern_expand_pdep, sift_kern_vm_pext);
const SiftKernels sift_kernels_avx2_nopdep = AVX2_TABLE(sift_kern_expand_scalar, sift_kern_vm_scalar);

#endif
