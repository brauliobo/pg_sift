/*
 * col/kern_int.h - shared between the per-level column kernel files.
 */
#ifndef SIFT_COL_KERN_INT_H
#define SIFT_COL_KERN_INT_H

#include "col/kern.h"
#include "bm25/bitmap_kern.h"
#include "bm25/kernels_int.h"

extern const SiftColKernels sift_col_kernels_scalar;
extern const SiftColKernels sift_col_kernels_avx2;
extern const SiftColKernels sift_col_kernels_avx512;

#if defined(SIFT_HAVE_NEON)
/* The NEON kernels (kern_neon.c); the NEON table is in kern.c, next to the scalar kernels it keeps for the other slots */
extern void sift_colk_range_u8_neon(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits);
extern void sift_colk_range_u16_neon(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits);
extern void sift_colk_range_u32_neon(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits);
extern void sift_colk_range_u64_neon(const void *a, uint32 n, uint64 lo, uint64 span, uint64 *bits);
extern uint32 sift_colk_bits_to_idx_neon(const uint64 *bits, uint32 nwords, uint32 base, uint32 *idx);
extern void sift_colk_unpack_neon(const uint8 *src, uint32 bits, uint32 n, uint32 *dst);
extern void sift_colk_lut_u8_neon(const void *a, uint32 n, const uint64 *lutbits, uint64 *bits);
extern void sift_colk_zone_range_neon(const int64 *mn, const int64 *mx, uint32 n, int64 lo, int64 hi, uint64 *keep);
extern void sift_colk_zone_notnull_neon(const uint32 *nnull, const uint32 *nvals, uint32 n, uint64 *keep);
extern void sift_colk_bits_to_flags_neon(const uint64 *bits, uint32 n, uint8 *flags);
extern uint64 sift_colk_bits_and_count_neon(const uint64 *a, const uint64 *b, uint32 nwords);
extern void sift_colk_for_i64_u8_neon(const void *a, uint32 n, int64 base, int64 *dst);
extern void sift_colk_for_i64_u16_neon(const void *a, uint32 n, int64 base, int64 *dst);
extern void sift_colk_for_i64_u32_neon(const void *a, uint32 n, int64 base, int64 *dst);
extern void sift_colk_for_i64_u64_neon(const void *a, uint32 n, int64 base, int64 *dst);
extern void sift_colk_f8_from_keys_neon(int64 *a, uint32 n);
extern void sift_colk_zone_set_neon(const uint64 (*set)[4], uint32 n, const uint64 *probe, uint64 *keep);
extern void sift_colk_expand_parents_neon(const uint32 *end, uint32 np, uint32 *parent);
#endif

/* Fully unrolled loop of a constant trip count (a vector step per iteration), whichever compiler builds the file */
#if defined(__clang__)
#define SIFT_COLK_UNROLL(n)	_Pragma("clang loop unroll(full)")
#else
#define SIFT_COLK_UNROLL(n)	SIFT_COLK_PRAGMA(GCC unroll n)
#define SIFT_COLK_PRAGMA(x)	_Pragma(#x)
#endif

/* bits_to_idx: a word with at most this many bits is walked bit by bit, the vector expansion costs more */
#define SIFT_COLK_SEL_SPARSE_AVX2	8
#define SIFT_COLK_SEL_SPARSE_AVX512	2
#define SIFT_COLK_SEL_SPARSE_NEON	8

/* Words that hold n bits, and the mask of the bits of word w that are below n */
#define SIFT_COLK_WORDS(n)		(((uint32) (n) + 63) / 64)

static inline uint64
sift_colk_tail(uint32 n, uint32 w)
{
	uint32		r = n - w * 64;

	return r >= 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << r) - 1;
}

/*
 * Fills the words of `bits` that hold n bits from word(p, ...), the 64 bits of the 64 elements of type T at p: full words straight from the
 * array, the last partial word from a zero padded copy of its elements (so no read goes past the array), bits past n zero
 */
#define SIFT_COLK_WORDS_OF(T, a, n, bits, word, ...) \
	do { \
		const T    *p_ = (a); \
		uint32		i_ = 0; \
		for (; i_ + 64 <= (n); i_ += 64) \
			(bits)[i_ / 64] = word(p_ + i_, __VA_ARGS__); \
		if (i_ < (n)) \
		{ \
			T			tmp_[64]; \
			uint32		r_ = (n) - i_; \
			memcpy(tmp_, p_ + i_, r_ * sizeof(T)); \
			memset(tmp_ + r_, 0, (64 - r_) * sizeof(T)); \
			(bits)[i_ / 64] = word(tmp_, __VA_ARGS__) & sift_colk_tail(n, i_ / 64); \
		} \
	} while (0)

/* ... and the words up to the padded size zero */
#define SIFT_COLK_BITS_OF(T, a, n, bits, word, ...) \
	do { \
		SIFT_COLK_WORDS_OF(T, a, n, bits, word, __VA_ARGS__); \
		sift_colk_zero_pad(bits, SIFT_COLK_WORDS(n), n); \
	} while (0)

/* Zeroes the words of a bit vector of n bits from word `from` up to its padded size */
static inline void
sift_colk_zero_pad(uint64 *bits, uint32 from, uint32 n)
{
	for (uint64 w = from; w < SIFT_COL_NWORDS(n); w++)
		bits[w] = 0;
}

/*
 * bits_count of a level is the popcount kernel of the bitmap module's table of the same level (bm25/bitmap_kern.h; AVX2 and AVX-512
 * have vector ones), the scalar bits_and / bits_or / bits_andnot its vop: static wrappers for the table initializers
 */
#define SIFT_COLK_BITS_COUNT(sfx, tab) \
	static uint64 bits_count_##sfx(const uint64 *a, uint32 nwords) { return (tab).popcount((const uint8_t *) a, (size_t) nwords * 8); }

#define SIFT_COLK_BITS_OPS(sfx, tab) \
	static void bits_and_##sfx(uint64 *dst, const uint64 *src, uint32 nwords) { (tab).vop(dst, dst, src, nwords, SIFT_VOP_AND); } \
	static void bits_or_##sfx(uint64 *dst, const uint64 *src, uint32 nwords) { (tab).vop(dst, dst, src, nwords, SIFT_VOP_OR); } \
	static void bits_andnot_##sfx(uint64 *dst, const uint64 *src, uint32 nwords) { (tab).vop(dst, dst, src, nwords, SIFT_VOP_ANDNOT); } \
	SIFT_COLK_BITS_COUNT(sfx, tab)

/* The positions of the set bits of a byte, lane j of row b (0 past the last): the AVX2 and NEON expansion of bits_to_idx */
extern const uint8 sift_colk_lut_pos[256][8];

/* The scalar kernels the AVX2 kernels finish with (the tail of an array) */
extern void sift_colk_unpack_scalar(const uint8 *src, uint32 bits, uint32 n, uint32 *dst);
extern void sift_colk_for_i64_u8_scalar(const void *a, uint32 n, int64 base, int64 *dst);
extern void sift_colk_for_i64_u16_scalar(const void *a, uint32 n, int64 base, int64 *dst);
extern void sift_colk_for_i64_u32_scalar(const void *a, uint32 n, int64 base, int64 *dst);
extern void sift_colk_for_i64_u64_scalar(const void *a, uint32 n, int64 base, int64 *dst);
extern void sift_colk_for_sel_u8_scalar(const void *a, const uint32 *idx, uint32 n, int64 base, int64 *dst);
extern void sift_colk_for_sel_u16_scalar(const void *a, const uint32 *idx, uint32 n, int64 base, int64 *dst);
extern void sift_colk_for_sel_u32_scalar(const void *a, const uint32 *idx, uint32 n, int64 base, int64 *dst);
extern void sift_colk_for_sel_u64_scalar(const void *a, const uint32 *idx, uint32 n, int64 base, int64 *dst);
extern void sift_colk_gather_u32_scalar(const uint32 *a, const uint32 *idx, uint32 n, uint32 *dst);
extern void sift_colk_f8_from_keys_scalar(int64 *a, uint32 n);
extern void sift_colk_bits_to_flags_scalar(const uint64 *bits, uint32 n, uint8 *flags);

#endif							/* SIFT_COL_KERN_INT_H */
