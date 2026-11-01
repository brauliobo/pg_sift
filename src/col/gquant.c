/*
 * col/gquant.c - the quantized keys of a compact chunk of a global order (col/gquant.h)
 */
#include "core/host.h"
#include "col/gquant.h"

#if defined(__x86_64__) || defined(SIFT_HAVE_NEON)
#define GQ_SIMD 1
#endif
#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(SIFT_HAVE_NEON)
#include <arm_neon.h>
#endif

#define GQ_WSIMD	14				/* the widest mini-block the vector kernels unpack: 8 values of w bits are in 16 bytes */
#define GQ_MAXW		32

static inline uint32
bitlen(uint64 v)
{
	return v ? 64 - __builtin_clzll(v) : 0;
}

/* The bytes and the shifts that take 8 values of w bits at a time out of a 16 byte window (4 bytes of the window for each, then the bits of the first byte they skip) */
#define GQ_I(w, j)	(((j) * (w)) >> 3)
#define GQ_V(w, j)	GQ_I(w, j), GQ_I(w, j) + 1, GQ_I(w, j) + 2, GQ_I(w, j) + 3
#define GQ_ROW(w)	{GQ_V(w, 0), GQ_V(w, 1), GQ_V(w, 2), GQ_V(w, 3), GQ_V(w, 4), GQ_V(w, 5), GQ_V(w, 6), GQ_V(w, 7)}
#define GQ_S(w)		{0, (w) & 7, ((w) * 2) & 7, ((w) * 3) & 7, ((w) * 4) & 7, ((w) * 5) & 7, ((w) * 6) & 7, ((w) * 7) & 7}
#define GQ_ALL(F)	F(0), F(1), F(2), F(3), F(4), F(5), F(6), F(7), F(8), F(9), F(10), F(11), F(12), F(13), F(14)

#if defined(GQ_SIMD)
static const uint8 gq_idx[GQ_WSIMD + 1][32] __attribute__((aligned(32))) = {GQ_ALL(GQ_ROW)};
static const uint32 gq_shift[GQ_WSIMD + 1][8] __attribute__((aligned(32))) = {GQ_ALL(GQ_S)};
#endif

/*
 * The shift makes about q buckets of the mean gap g = (last - first) / (m - 1) (0 when g / q is under 2). Each mini-block takes the width that makes it shortest, the larger
 * differences patched in. Integers only.
 */
uint32
sift_gq_encode(const int64 *key, uint32 m, uint32 q, StringInfo out)
{
	uint64		first = (uint64) key[0],
				span = m > 1 ? (uint64) key[m - 1] - first : 0,
				tgt = m > 1 ? span / (m - 1) / q : 0,
				qprev = 0;
	uint32		sh = 0,
				nmb = (m + SIFT_GQ_MB - 1) / SIFT_GQ_MB,
				len = 1;
	uint8	   *buf = palloc0(1 + (Size) nmb * (1 + 4 * GQ_MAXW + 2 + SIFT_GQ_MB + 4 * SIFT_GQ_MB) + 16);

	if (tgt >= 2)
		sh = 63 - __builtin_clzll(tgt);
	buf[0] = (uint8) sh;
	for (uint32 j = 0; j < nmb; j++)
	{
		uint32		d[SIFT_GQ_MB],
					cnt[GQ_MAXW + 2] = {0},
					mx = 0,
					w = 0,
					best = UINT32_MAX,
					n = 0,
					e;
		uint8		pos[SIFT_GQ_MB];

		for (uint32 i = 0; i < SIFT_GQ_MB; i++)
		{
			uint32		at = j * SIFT_GQ_MB + i;
			uint64		qi = at < m ? ((uint64) key[at] - first) >> sh : qprev;

			d[i] = (uint32) (qi - qprev);
			qprev = qi;
			cnt[bitlen(d[i])]++;
			mx = Max(mx, bitlen(d[i]));
		}
		/* the bits of a width: its packed values, and the patches (a position byte, 2 header bytes, and the bits above the width) of those that do not fit */
		for (uint32 c = 0, over = SIFT_GQ_MB; c <= mx; c++)
		{
			uint32		cost;

			over -= cnt[c];
			cost = 8 * (4 * c) + (over > 0 ? 8 * (2 + over) + over * (mx - c) : 0);
			if (cost < best)
			{
				best = cost;
				w = c;
			}
		}
		for (uint32 i = 0; i < SIFT_GQ_MB; i++)
			if (bitlen(d[i]) > w)
				pos[n++] = (uint8) i;
		e = mx - w;
		buf[len++] = (uint8) (w | (n > 0 ? 0x80 : 0));
		for (uint32 i = 0; i < SIFT_GQ_MB; i++)
			for (uint32 b = 0; b < w; b++)
				if ((d[i] >> b) & 1)
					buf[len + ((i * w + b) >> 3)] |= (uint8) (1 << ((i * w + b) & 7));
		len += 4 * w;
		if (n > 0)
		{
			buf[len++] = (uint8) n;
			buf[len++] = (uint8) e;
			memcpy(buf + len, pos, n);
			len += n;
			for (uint32 k = 0; k < n; k++)
				for (uint32 b = 0; b < e; b++)
					if (((d[pos[k]] >> w) >> b) & 1)
						buf[len + ((k * e + b) >> 3)] |= (uint8) (1 << ((k * e + b) & 7));
			len += (n * e + 7) >> 3;
		}
	}
	appendBinaryStringInfo(out, (const char *) buf, len);
	pfree(buf);
	return len;
}

/* w bits (w <= 32) at bit pos of the stream b, LSB first; b has 8 readable bytes past pos / 8 */
static inline uint32
gq_bits(const uint8 *b, uint32 pos, uint32 w)
{
	uint64		x;

	memcpy(&x, b + (pos >> 3), 8);
	return (uint32) ((x >> (pos & 7)) & (((uint64) 1 << w) - 1));
}

/*
 * The header of the mini-block at p of a blob of len bytes: its width and what the patches hold, and the bytes of the mini-block; false when it does not fit. The patches are added to d
 * (the 32 differences, unpacked by the caller) by gq_patch.
 */
static inline bool
gq_mb(const uint8 *b, uint32 len, uint32 p, uint32 *w, uint32 *used)
{
	uint32		n,
				e;

	if (p >= len)
		return false;
	*w = b[p] & 0x7f;
	if (*w > GQ_MAXW)
		return false;
	*used = 1 + 4 * *w;
	if (b[p] & 0x80)
	{
		if (*used + 2 > len - p)
			return false;
		n = b[p + *used];
		e = b[p + *used + 1];
		if (n == 0 || n > SIFT_GQ_MB || e > GQ_MAXW)
			return false;
		*used += 2 + n + ((n * e + 7) >> 3);
	}
	return *used <= len - p;
}

static inline bool
gq_patch(const uint8 *b, uint32 p, uint32 w, uint32 *d)
{
	const uint8 *s = b + p + 1 + 4 * w;
	uint32		n = s[0],
				e = s[1];

	for (uint32 k = 0; k < n; k++)
	{
		if (s[2 + k] >= SIFT_GQ_MB)
			return false;
		d[s[2 + k]] |= w < 32 ? gq_bits(s + 2 + n, k * e, e) << w : 0;
	}
	return true;
}

static inline void
gq_unpack(const uint8 *pl, uint32 w, uint32 *d)
{
	for (uint32 i = 0; i < SIFT_GQ_MB; i++)
		d[i] = w == 0 ? 0 : gq_bits(pl, i * w, w);
}

static bool
gq_decode_scalar(const uint8 *b, uint32 len, uint32 m, int64 first, int64 *kmin, uint32 *sh_out)
{
	uint32		nmb = (m + SIFT_GQ_MB - 1) / SIFT_GQ_MB,
				p = 1,
				sh,
				q = 0;

	if (len < 1 || b[0] > 63)
		return false;
	sh = b[0];
	for (uint32 j = 0; j < nmb; j++)
	{
		uint32		w,
					used,
					d[SIFT_GQ_MB];

		if (!gq_mb(b, len, p, &w, &used))
			return false;
		gq_unpack(b + p + 1, w, d);
		if ((b[p] & 0x80) && !gq_patch(b, p, w, d))
			return false;
		for (uint32 i = 0; i < SIFT_GQ_MB; i++)
		{
			q += d[i];
			kmin[j * SIFT_GQ_MB + i] = (int64) ((uint64) first + ((uint64) q << sh));
		}
		p += used;
	}
	*sh_out = sh;
	return true;
}

#if defined(__x86_64__)
SIFT_TARGET_AVX2
static bool
gq_decode_avx2(const uint8 *b, uint32 len, uint32 m, int64 first, int64 *kmin, uint32 *sh_out)
{
	uint32		nmb = (m + SIFT_GQ_MB - 1) / SIFT_GQ_MB,
				p = 1,
				sh;
	__m256i		vfirst = _mm256_set1_epi64x(first),
				carry = _mm256_setzero_si256(),
				last = _mm256_set1_epi32(7);
	__m128i		shc;
	uint32		d[SIFT_GQ_MB] __attribute__((aligned(32)));

	if (len < 1 || b[0] > 63)
		return false;
	sh = b[0];
	shc = _mm_cvtsi32_si128((int) sh);
	for (uint32 j = 0; j < nmb; j++)
	{
		uint32		w,
					used;

		if (!gq_mb(b, len, p, &w, &used))
			return false;
		if (w > GQ_WSIMD)
			gq_unpack(b + p + 1, w, d);
		else if (w == 0)
			memset(d, 0, sizeof(d));
		else
		{
			__m256i		msk = _mm256_set1_epi32((int) (((uint32) 1 << w) - 1)),
						sf = _mm256_load_si256((const __m256i *) gq_idx[w]),
						sv = _mm256_load_si256((const __m256i *) gq_shift[w]);

			for (uint32 g = 0; g < 4; g++)
			{
				__m256i		x = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *) (b + p + 1 + g * w)));

				x = _mm256_and_si256(_mm256_srlv_epi32(_mm256_shuffle_epi8(x, sf), sv), msk);
				_mm256_store_si256((__m256i *) (d + g * 8), x);
			}
		}
		if ((b[p] & 0x80) && !gq_patch(b, p, w, d))
			return false;
		for (uint32 g = 0; g < 4; g++)
		{
			__m256i		x = _mm256_load_si256((const __m256i *) (d + g * 8)),
						t,
						lo,
						hi;

			/* the running sum of 8 sums: of each half, then the first half's total into the second, then everything before */
			x = _mm256_add_epi32(x, _mm256_slli_si256(x, 4));
			x = _mm256_add_epi32(x, _mm256_slli_si256(x, 8));
			t = _mm256_shuffle_epi32(x, 0xFF);
			x = _mm256_add_epi32(x, _mm256_permute2x128_si256(t, t, 0x08));
			x = _mm256_add_epi32(x, carry);
			carry = _mm256_permutevar8x32_epi32(x, last);
			lo = _mm256_add_epi64(_mm256_sll_epi64(_mm256_cvtepu32_epi64(_mm256_castsi256_si128(x)), shc), vfirst);
			hi = _mm256_add_epi64(_mm256_sll_epi64(_mm256_cvtepu32_epi64(_mm256_extracti128_si256(x, 1)), shc), vfirst);
			_mm256_storeu_si256((__m256i *) (kmin + j * SIFT_GQ_MB + g * 8), lo);
			_mm256_storeu_si256((__m256i *) (kmin + j * SIFT_GQ_MB + g * 8 + 4), hi);
		}
		p += used;
	}
	*sh_out = sh;
	return true;
}
#endif							/* __x86_64__ */

#if defined(SIFT_HAVE_NEON)
static bool
gq_decode_neon(const uint8 *b, uint32 len, uint32 m, int64 first, int64 *kmin, uint32 *sh_out)
{
	uint32		nmb = (m + SIFT_GQ_MB - 1) / SIFT_GQ_MB,
				p = 1,
				sh;
	uint64x2_t	vfirst = vdupq_n_u64((uint64) first);
	int64x2_t	shc;
	uint32x4_t	carry = vdupq_n_u32(0),
				zero = vdupq_n_u32(0);
	uint32		d[SIFT_GQ_MB];

	if (len < 1 || b[0] > 63)
		return false;
	sh = b[0];
	shc = vdupq_n_s64((int64) sh);
	for (uint32 j = 0; j < nmb; j++)
	{
		uint32		w,
					used;

		if (!gq_mb(b, len, p, &w, &used))
			return false;
		if (w > GQ_WSIMD)
			gq_unpack(b + p + 1, w, d);
		else if (w == 0)
			memset(d, 0, sizeof(d));
		else
		{
			uint32x4_t	msk = vdupq_n_u32(((uint32) 1 << w) - 1);

			for (uint32 g = 0; g < 4; g++)
			{
				uint8x16_t	x = vld1q_u8(b + p + 1 + g * w);

				for (uint32 h = 0; h < 2; h++)
				{
					uint32x4_t	v = vreinterpretq_u32_u8(vqtbl1q_u8(x, vld1q_u8(gq_idx[w] + 16 * h)));

					v = vandq_u32(vshlq_u32(v, vnegq_s32(vreinterpretq_s32_u32(vld1q_u32(gq_shift[w] + 4 * h)))), msk);
					vst1q_u32(d + g * 8 + 4 * h, v);
				}
			}
		}
		if ((b[p] & 0x80) && !gq_patch(b, p, w, d))
			return false;
		for (uint32 g = 0; g < 8; g++)
		{
			uint32x4_t	x = vld1q_u32(d + g * 4);

			x = vaddq_u32(x, vextq_u32(zero, x, 3));
			x = vaddq_u32(x, vextq_u32(zero, x, 2));
			x = vaddq_u32(x, carry);
			carry = vdupq_laneq_u32(x, 3);
			vst1q_s64(kmin + j * SIFT_GQ_MB + g * 4, vreinterpretq_s64_u64(vaddq_u64(vshlq_u64(vmovl_u32(vget_low_u32(x)), shc), vfirst)));
			vst1q_s64(kmin + j * SIFT_GQ_MB + g * 4 + 2, vreinterpretq_s64_u64(vaddq_u64(vshlq_u64(vmovl_u32(vget_high_u32(x)), shc), vfirst)));
		}
		p += used;
	}
	*sh_out = sh;
	return true;
}
#endif							/* SIFT_HAVE_NEON */

SiftGqDecode
sift_gq_decode_for(SiftSimdLevel level)
{
	level = Min(level, sift_cpu.detected);
#if defined(__x86_64__)
	if (level >= SIFT_SIMD_AVX2)
		return gq_decode_avx2;
#elif defined(SIFT_HAVE_NEON)
	if (level >= SIFT_SIMD_NEON)
		return gq_decode_neon;
#endif
	return gq_decode_scalar;
}

bool
sift_gq_decode(const uint8 *b, uint32 len, uint32 m, int64 first, int64 *kmin, uint32 *sh)
{
	return sift_gq_decode_for(sift_cpu.active) (b, len, m, first, kmin, sh);
}

int64
sift_gq_kmax(int64 kmin, uint32 sh)
{
	uint64		w = sh >= 63 ? (uint64) PG_INT64_MAX : ((uint64) 1 << sh) - 1,
				u = (uint64) kmin + ((uint64) 1 << 63);

	if (w > UINT64_MAX - u)
		w = UINT64_MAX - u;
	return (int64) (u + w - ((uint64) 1 << 63));
}
