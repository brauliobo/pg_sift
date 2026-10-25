/*
 * agg/kern_vec.h - the vector kernels of the aggregation operators, written once with GCC/Clang vector extensions and
 * compiled by each level file at its own width (AKV_VB bytes per vector: 32 for AVX2, 64 for AVX-512, 16 for NEON) under the level's target
 * options. Included by agg/kern_avx2.c, agg/kern_avx512.c and agg/kern_neon.c after they define AKV_VB and AKV_FN(x). The integer arithmetic of the numeric programs
 * (agg/vnum.c) wraps (unsigned lanes): the caller has proved that no value leaves int64.
 */

typedef uint64 akv_u __attribute__((vector_size(AKV_VB), aligned(8), may_alias));
typedef int64 akv_s __attribute__((vector_size(AKV_VB), aligned(8), may_alias));

typedef int32 akv_i32 __attribute__((vector_size(AKV_VB), aligned(4), may_alias));
typedef double akv_f64 __attribute__((vector_size(AKV_VB), aligned(8), may_alias));

#define AKV_L		(AKV_VB / 8)
#define AKV_L32		(AKV_VB / 4)
#define AKV_LD(p)	(*(const akv_u *) (p))
#define AKV_ST(p, x) (*(akv_u *) (p) = (x))

#ifndef AKV_NO_ABSMAX
static uint64
AKV_FN(absmax_i64)(const int64 *v, int n)
{
	akv_s		mxp = {0},
				mxn = {0},
				neg_seen = {0};
	uint64		m = 0;
	int			i = 0;

	/* the largest |v| as the larger of the largest non-negative v and 1 + the largest ~v of a negative one (signed lanes, so INT64_MIN gives 2^63) */
	for (; i + AKV_L <= n; i += AKV_L)
	{
		akv_s		x = *(const akv_s *) (v + i),
					sg = x >> 63,
					pos = x & ~sg,
					neg = ~x & sg,
					gp = pos > mxp,
					gn = neg > mxn;

		mxp = (pos & gp) | (mxp & ~gp);
		mxn = (neg & gn) | (mxn & ~gn);
		neg_seen |= sg;
	}
	for (int k = 0; k < AKV_L; k++)
	{
		m = Max(m, (uint64) mxp[k]);
		if (neg_seen[k])
			m = Max(m, (uint64) mxn[k] + 1);
	}
	for (; i < n; i++)
		m = Max(m, v[i] < 0 ? -(uint64) v[i] : (uint64) v[i]);
	return m;
}
#endif

/*
 * The one operation the vector extensions cannot express: the product of the low signed 32 bits of each lane as a full 64-bit lane (vpmuldq, vmull_s32).
 * A level file defines AKV_WIDEN(a, b) for its ISA before including this file; x86-64 has no 64-bit vector multiply before AVX-512DQ and NEON never,
 * which is why the numeric programs prove their operands fit int32 (agg/vnum.c) and use this instead.
 */
#define AKV_TERM(a, k)	((akv_u) AKV_WIDEN((a), (akv_u) {} + (uint64) (int64) (k)))

/* dst = a * ka + b * kb: a term with |k| == 1 is a plain add or subtract, any other multiplies the low 32 bits of the operand */
#define AKV_LIN_LOOP(TA, TB) \
	for (; i + AKV_L <= n; i += AKV_L) \
	{ \
		akv_u		x = *(const akv_u *) (a + i), \
					y = *(const akv_u *) (b + i); \
		*(akv_u *) (dst + i) = (TA) + (TB); \
	}

static void
AKV_FN(lin_i64)(int64 *dst, const int64 *a, int32 ka, const int64 *b, int32 kb, int n)
{
	int			i = 0;

	if (ka == 1 && kb == 1)
		AKV_LIN_LOOP(x, y)
	else if (ka == 1 && kb == -1)
		AKV_LIN_LOOP(x, -y)
	else if (ka == 1)
		AKV_LIN_LOOP(x, AKV_TERM(y, kb))
	else if (kb == 1)
		AKV_LIN_LOOP(ka == -1 ? -x : AKV_TERM(x, ka), y)
	else if (kb == -1)
		AKV_LIN_LOOP(ka == -1 ? -x : AKV_TERM(x, ka), -y)
	else
		/* two multiplies, or a negated first term: the kind of the term is loop invariant */
		AKV_LIN_LOOP(ka == -1 ? -x : AKV_TERM(x, ka), kb == -1 ? -y : AKV_TERM(y, kb))
	for (; i < n; i++)
		dst[i] = (int64) ((uint64) agk_lin_term(a[i], ka) + (uint64) agk_lin_term(b[i], kb));
}

static void
AKV_FN(mulw_i64)(int64 *dst, const int64 *a, const int64 *b, int n)
{
	int			i = 0;

	for (; i + AKV_L <= n; i += AKV_L)
		*(akv_s *) (dst + i) = (akv_s) AKV_WIDEN(*(const akv_s *) (a + i), *(const akv_s *) (b + i));
	for (; i < n; i++)
		dst[i] = (int64) (int32) a[i] * (int64) (int32) b[i];
}

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
