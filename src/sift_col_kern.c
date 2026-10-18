/*
 * sift_col_kern.c - scalar column kernels and
 * the dispatch. Baseline x86-64 code: bits are counted with SWAR, no vector instruction.
 */
#include "sift_host.h"

#include "sift_col_kern_int.h"
#include "sift_popcount.h"

const SiftColKernels *sift_colk = &sift_col_kernels_scalar;

/* Word w of the bit vector of n bits whose bit i is (test), for i < n */
#define WORD_OF(n, w, test) \
	({ \
		uint64		m_ = 0; \
		for (uint32 i = (w) * 64, e_ = Min(n, i + 64); i < e_; i++) \
			m_ |= (uint64) ((test) != 0) << (i & 63); \
		m_; \
	})

/* Bit vector of n bits written as whole words, zero past n */
#define BITS_OF(n, bits, test) \
	do { \
		for (uint32 w_ = 0; w_ < SIFT_COLK_WORDS(n); w_++) \
			(bits)[w_] = WORD_OF(n, w_, test); \
		sift_colk_zero_pad(bits, SIFT_COLK_WORDS(n), n); \
	} while (0)

#define RANGE_SCALAR(name, T) \
static void \
name(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits) \
{ \
	const T    *p = a; \
	const T		l = (T) lo, \
				s = (T) span; \
	BITS_OF(n, bits, (T) (p[i] - l) <= s); \
}

RANGE_SCALAR(range_u8_scalar, uint8)
RANGE_SCALAR(range_u16_scalar, uint16)
RANGE_SCALAR(range_u32_scalar, uint32)
RANGE_SCALAR(range_u64_scalar, uint64)

#define LUT_SCALAR(name, T) \
static void \
name(const void *a, uint32 n, const uint64 *lutbits, uint64 *bits) \
{ \
	const T    *p = a; \
	BITS_OF(n, bits, (lutbits[p[i] >> 6] >> (p[i] & 63)) & 1); \
}

LUT_SCALAR(lut_u8_scalar, uint8)
LUT_SCALAR(lut_u16_scalar, uint16)
LUT_SCALAR(lut_u32_scalar, uint32)

SIFT_COLK_BITS_OPS(scalar, sift_kern_scalar)

static uint64
bits_and_count_scalar(const uint64 *a, const uint64 *b, uint32 nwords)
{
	uint64		c = 0;

	for (uint32 i = 0; i < nwords; i++)
		c += sift_pc64(a[i] & b[i]);
	return c;
}

static uint32
bits_to_idx_scalar(const uint64 *bits, uint32 nwords, uint32 base, uint32 *idx)
{
	uint32		n = 0;

	for (uint32 w = 0; w < nwords; w++)
		for (uint64 m = bits[w]; m; m &= m - 1)
			idx[n++] = base + w * 64 + __builtin_ctzll(m);
	return n;
}

static void
zone_range_scalar(const int64 *mn, const int64 *mx, uint32 n, int64 lo, int64 hi, uint64 *keep)
{
	BITS_OF(n, keep, !(mx[i] < lo || mn[i] > hi));
}

/* keep &= the bit vector of the test; the bits from n on are left as they are */
#define KEEP_AND(n, keep, test) \
	do { \
		for (uint32 w_ = 0; w_ < SIFT_COLK_WORDS(n); w_++) \
			(keep)[w_] &= WORD_OF(n, w_, test) | ~sift_colk_tail(n, w_); \
	} while (0)

static void
zone_set_scalar(const uint64 (*set)[4], uint32 n, const uint64 *probe, uint64 *keep)
{
	KEEP_AND(n, keep, (set[i][0] & probe[0]) | (set[i][1] & probe[1]) | (set[i][2] & probe[2]) | (set[i][3] & probe[3]));
}

static void
zone_notnull_scalar(const uint32 *nnull, const uint32 *nvals, uint32 n, uint64 *keep)
{
	KEEP_AND(n, keep, nnull[i] != nvals[i]);
}

static void
unpack_scalar(const uint8 *src, uint32 bits, uint32 n, uint32 *dst)
{
	const uint32 mask = (1U << bits) - 1;

	for (uint32 i = 0; i < n; i++)
	{
		uint64		pos = (uint64) i * bits;

		dst[i] = (uint32) (sift_kern_ld64(src + (pos >> 3)) >> (pos & 7)) & mask;
	}
}

#define FOR_I64_SCALAR(name, T) \
void \
name(const void *a, uint32 n, int64 base, int64 *dst) \
{ \
	const T    *p = a; \
	for (uint32 i = 0; i < n; i++) \
		dst[i] = (int64) ((uint64) base + (uint64) p[i]); \
}

FOR_I64_SCALAR(sift_colk_for_i64_u8_scalar, uint8)
FOR_I64_SCALAR(sift_colk_for_i64_u16_scalar, uint16)
FOR_I64_SCALAR(sift_colk_for_i64_u32_scalar, uint32)
FOR_I64_SCALAR(sift_colk_for_i64_u64_scalar, uint64)

#define FOR_SEL_SCALAR(name, T) \
void \
name(const void *a, const uint32 *idx, uint32 n, int64 base, int64 *dst) \
{ \
	const T    *p = a; \
	for (uint32 k = 0; k < n; k++) \
		dst[k] = (int64) ((uint64) base + (uint64) p[idx[k]]); \
}

FOR_SEL_SCALAR(sift_colk_for_sel_u8_scalar, uint8)
FOR_SEL_SCALAR(sift_colk_for_sel_u16_scalar, uint16)
FOR_SEL_SCALAR(sift_colk_for_sel_u32_scalar, uint32)
FOR_SEL_SCALAR(sift_colk_for_sel_u64_scalar, uint64)

void
sift_colk_gather_u32_scalar(const uint32 *a, const uint32 *idx, uint32 n, uint32 *dst)
{
	for (uint32 k = 0; k < n; k++)
		dst[k] = a[idx[k]];
}

static void
expand_parents_scalar(const uint32 *end, uint32 np, uint32 *parent)
{
	uint32		i = 0;

	for (uint32 p = 0; p < np; p++)
		for (; i < end[p]; i++)
			parent[i] = p;
}

const SiftColKernels sift_col_kernels_scalar = {
	.range = {range_u8_scalar, range_u16_scalar, range_u32_scalar, range_u64_scalar},
	.lut = {lut_u8_scalar, lut_u16_scalar, lut_u32_scalar},
	.bits_and = bits_and_scalar,
	.bits_or = bits_or_scalar,
	.bits_andnot = bits_andnot_scalar,
	.bits_count = bits_count_scalar,
	.bits_and_count = bits_and_count_scalar,
	.bits_to_idx = bits_to_idx_scalar,
	.zone_range = zone_range_scalar,
	.zone_set = zone_set_scalar,
	.zone_notnull = zone_notnull_scalar,
	.unpack = unpack_scalar,
	.for_i64 = {sift_colk_for_i64_u8_scalar, sift_colk_for_i64_u16_scalar, sift_colk_for_i64_u32_scalar, sift_colk_for_i64_u64_scalar},
	.for_i64_sel = {sift_colk_for_sel_u8_scalar, sift_colk_for_sel_u16_scalar, sift_colk_for_sel_u32_scalar, sift_colk_for_sel_u64_scalar},
	.gather_u32 = sift_colk_gather_u32_scalar,
	.expand_parents = expand_parents_scalar,
};

const SiftColKernels *
sift_col_kernels_for(SiftSimdLevel level)
{
	level = Min(level, sift_cpu.detected);
#if defined(__x86_64__)
	if (level == SIFT_SIMD_AVX512)
		return &sift_col_kernels_avx512;
	if (level == SIFT_SIMD_AVX2)
		return &sift_col_kernels_avx2;
#endif
	return &sift_col_kernels_scalar;
}

static void
col_kernels_rebind(const SiftCpu *cpu)
{
	sift_colk = sift_col_kernels_for(cpu->active);
}

void
sift_col_kernels_init(void)
{
	sift_cpu_register(col_kernels_rebind);
}
