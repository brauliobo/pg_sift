/*
 * agg/kern_vec.h - the vector kernels of the aggregation operators, written once with GCC/Clang vector extensions and
 * compiled by each level file at its own width (AKV_VB bytes per vector: 32 for AVX2, 64 for AVX-512, 16 for NEON) under the level's target
 * options. Included by agg/kern_avx2.c, agg/kern_avx512.c and agg/kern_neon.c after they define AKV_VB and AKV_FN(x).
 */

typedef uint64 akv_u __attribute__((vector_size(AKV_VB), aligned(8), may_alias));
typedef int64 akv_s __attribute__((vector_size(AKV_VB), aligned(8), may_alias));

typedef int32 akv_i32 __attribute__((vector_size(AKV_VB), aligned(4), may_alias));
typedef double akv_f64 __attribute__((vector_size(AKV_VB), aligned(8), may_alias));

#define AKV_L		(AKV_VB / 8)
#define AKV_L32		(AKV_VB / 4)
#define AKV_LD(p)	(*(const akv_u *) (p))
#define AKV_ST(p, x) (*(akv_u *) (p) = (x))

/*
 * Two columns compared into a bitmap (the cmp2_* slots). The six operators are two primitives and a complement: a < b, a = b, and their exact negations over the
 * total order (a <= b is not b < a, a <> b is not a = b), so a level needs one loop per primitive and the operand swap is a call with the pointers exchanged.
 * Every 64 rows are one word of the bitmap; the tail word reads at most one vector past n (the padding of the arrays) and its bits >= n are cleared. A level file defines
 * AKV_BITS32(m) and AKV_BITS64(m): the lane masks of an int32 / int64 compare as the bits of its lanes.
 *
 * Doubles follow PostgreSQL's order: a NaN equals a NaN and is greater than every number, -0 equals 0 (a < b, with b a NaN and a not, is true).
 */
#define AKV_CMP2_LOOP(NAME, T, VT, LANES, BITS, LT, EQ) \
	static inline __attribute__((always_inline)) void \
	NAME##_t(uint64 *out, const T *a, const T *b, int n, const bool eq, const uint64 neg) \
	{ \
		int			w = 0; \
		for (; (w + 1) * 64 <= n; w++) \
		{ \
			uint64		bits = 0; \
			for (int j = 0; j < 64; j += LANES) \
			{ \
				const VT	x = *(const VT *) (a + w * 64 + j), \
							y = *(const VT *) (b + w * 64 + j); \
				bits |= (uint64) BITS(eq ? (EQ) : (LT)) << j; \
			} \
			out[w] = bits ^ neg; \
		} \
		if (n & 63) \
		{ \
			uint64		bits = 0; \
			for (int j = 0; j < (n & 63); j += LANES) \
			{ \
				const VT	x = *(const VT *) (a + w * 64 + j), \
							y = *(const VT *) (b + w * 64 + j); \
				bits |= (uint64) BITS(eq ? (EQ) : (LT)) << j; \
			} \
			out[w] = (bits ^ neg) & AGK_LOW(n & 63); \
		} \
	} \
	static void \
	AKV_FN(NAME)(uint64 *out, const T *a, const T *b, int n, SiftCmpOp op) \
	{ \
		switch (op) \
		{ \
			case SCMP_LT: NAME##_t(out, a, b, n, false, 0); break; \
			case SCMP_GT: NAME##_t(out, b, a, n, false, 0); break; \
			case SCMP_LE: NAME##_t(out, b, a, n, false, ~UINT64CONST(0)); break; \
			case SCMP_GE: NAME##_t(out, a, b, n, false, ~UINT64CONST(0)); break; \
			case SCMP_EQ: NAME##_t(out, a, b, n, true, 0); break; \
			case SCMP_NE: NAME##_t(out, a, b, n, true, ~UINT64CONST(0)); break; \
		} \
	}

#define AKV_NAN_LT(x, y)	(((x) < (y)) | (((y) != (y)) & ((x) == (x))))
#define AKV_NAN_EQ(x, y)	(((x) == (y)) | (((x) != (x)) & ((y) != (y))))

AKV_CMP2_LOOP(cmp2_i32, int32, akv_i32, AKV_L32, AKV_BITS32, x < y, x == y)
#ifndef AKV_NO_CMP2_I64
AKV_CMP2_LOOP(cmp2_i64, int64, akv_s, AKV_L, AKV_BITS64, x < y, x == y)
#endif
#ifndef AKV_NO_CMP2_F64
AKV_CMP2_LOOP(cmp2_f64, double, akv_f64, AKV_L, AKV_BITS64, AKV_NAN_LT(x, y), AKV_NAN_EQ(x, y))
#endif
