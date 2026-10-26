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

/*
 * The reductions over the rows of a mask (the sum, the largest absolute value and the minima and maxima): a level whose selected rows are a merging mask
 * register (AVX-512) defines AKV_OWN_REDUCE and keeps its own. The selected lanes of a vector are the bits of the mask word, turned into lane masks by a
 * compare against the weight of the lane; the unselected lanes take the identity of the reduction. Min and max are a compare and a blend, which the compilers
 * turn into the instruction of the level; a NaN is never the first operand, so it never enters a running value (what MINPD/MAXPD do).
 */
#ifndef AKV_OWN_REDUCE
#ifndef AKV_RFN
#define AKV_RFN(x)	AKV_FN(x)
#endif
#if AKV_VB == 16
#define AKV_W32 {1, 2, 4, 8}
#define AKV_W64 {1, 2}
#elif AKV_VB == 32
#define AKV_W32 {1, 2, 4, 8, 16, 32, 64, 128}
#define AKV_W64 {1, 2, 4, 8}
#else
#define AKV_W32 {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768}
#define AKV_W64 {1, 2, 4, 8, 16, 32, 64, 128}
#endif

#define AKV_INL		static inline __attribute__((always_inline))
#define AKV_SEL(mask, i, n, L)	((mask) ? (uint32) ((mask)[(i) >> 6] >> ((i) & 63)) & (uint32) AGK_LOW(L) : (uint32) AGK_LANES((n) - (i), L))
#define AKV_LDT(T, p)	(*(const T *) (p))
/* the lanes of k's bits as masks: a level with a cheaper way than the compare against the weights (a table, a mask register) defines AKV_LANES64 */
#ifndef AKV_LANES64
#define AKV_LANES64(k)	((akv_s) ((((akv_s) {} + (k)) & akv_w64) == akv_w64))
#endif
#ifndef AKV_LANES32
#define AKV_LANES32(k)	((akv_i32) ((((akv_i32) {} + (int32) (k)) & akv_w32) == akv_w32))
#endif
/* m ? a : b per lane, m all ones or zero: a level with a select instruction defines AKV_BLEND */
#ifndef AKV_BLEND
#define AKV_BLEND(T, m, a, b)	((T) (((akv_s) (m) & (akv_s) (a)) | (~(akv_s) (m) & (akv_s) (b))))
#endif
/* min / max, a NaN first operand yields the second: the compilers do not form the instruction of the level out of the blend, a level defines AKV_MIN_<T> / AKV_MAX_<T> for the types it has one for (akv_i32, akv_f64) */
#define AKV_MIN(T, a, b)	AKV_MIN_##T(a, b)
#define AKV_MAX(T, a, b)	AKV_MAX_##T(a, b)
#ifndef AKV_MIN_akv_i32
#define AKV_MIN_akv_i32(a, b)	AKV_BLEND(akv_i32, (a) < (b), a, b)
#define AKV_MAX_akv_i32(a, b)	AKV_BLEND(akv_i32, (a) > (b), a, b)
#endif
#ifndef AKV_MIN_akv_f64
#define AKV_MIN_akv_f64(a, b)	AKV_BLEND(akv_f64, (a) < (b), a, b)
#define AKV_MAX_akv_f64(a, b)	AKV_BLEND(akv_f64, (a) > (b), a, b)
#endif

static const akv_i32 akv_w32 = AKV_W32;
static const akv_s akv_w64 = AKV_W64;

#ifndef AKV_NO_SUM_I32
/* The sum of int32 rows as int64: every row is biased by 2^31 into an unsigned 32 bit lane, whose even and odd halves add as 64 bit lanes; the bias comes off at the end */
AKV_INL int64
AKV_RFN(sum_i32_t)(const int32 *v, const uint64 *mask, int n, const bool masked)
{
	const akv_i32 sign = (akv_i32) {} + INT32_MIN;
	akv_u		acc = {};
	uint64		cnt = 0,
				s = 0;

	for (int i = 0; i < n; i += AKV_L32)
	{
		const uint32 b = AKV_SEL(masked ? mask : NULL, i, n, AKV_L32);
		akv_i32		x = AKV_LDT(akv_i32, v + i) ^ sign;
		akv_u		u;

		if (masked || i + AKV_L32 > n)
			x &= AKV_LANES32(b);
		u = (akv_u) x;
		acc += (u & 0xFFFFFFFF) + (u >> 32);
		cnt += __builtin_popcount(b);
	}
	for (int k = 0; k < AKV_L; k++)
		s += acc[k];
	return (int64) (s - (cnt << 31));
}

static int64
AKV_RFN(sum_i32)(const int32 *v, const uint64 *mask, int n)
{
	return mask ? AKV_RFN(sum_i32_t)(v, mask, n, true) : AKV_RFN(sum_i32_t)(v, mask, n, false);
}
#endif

#ifndef AKV_NO_SUM_I64
/* The sum of int64 rows modulo 2^64; the rows past the last whole vector are added one by one */
AKV_INL int64
AKV_RFN(sum_i64_t)(const int64 *v, const uint64 *mask, int n, const bool masked)
{
	akv_u		acc = {};
	uint64		s = 0;
	int			i = 0;

	for (; i + AKV_L <= n; i += AKV_L)
	{
		akv_u		x = AKV_LD(v + i);

		if (masked)
			x &= (akv_u) AKV_LANES64(AKV_SEL(mask, i, n, AKV_L));
		acc += x;
	}
	for (int k = 0; k < AKV_L; k++)
		s += acc[k];
	for (; i < n; i++)
		if (!masked || (mask[i >> 6] >> (i & 63) & 1))
			s += (uint64) v[i];
	return (int64) s;
}

static int64
AKV_RFN(sum_i64)(const int64 *v, const uint64 *mask, int n)
{
	return mask ? AKV_RFN(sum_i64_t)(v, mask, n, true) : AKV_RFN(sum_i64_t)(v, mask, n, false);
}
#endif

/* Unselected lanes are +0.0; a lane is finite when |x| <= DBL_MAX (false for NaN and Inf) */
#define AKV_ABS_STEP(m, b, p) \
	do { \
		const akv_f64 x_ = (akv_f64) ((AKV_LDT(akv_s, p) & AKV_LANES64(b)) & habs); \
		fin &= (akv_s) (x_ <= big); \
		(m) = AKV_MAX(akv_f64, m, x_); \
	} while (0)

AKV_INL bool
AKV_RFN(absmax_f64_t)(const double *v, const uint64 *mask, int n, double *mx, const bool masked)
{
	const akv_s habs = (akv_s) {} + INT64_MAX;
	const akv_f64 big = (akv_f64) {} + DBL_MAX;
	akv_f64		m[4] = {{}, {}, {}, {}};
	akv_s		fin = (akv_s) {} - 1;
	double		r = 0;
	int			i = 0;

	for (; i + 4 * AKV_L <= n; i += 4 * AKV_L)
	{
		const uint32 bb = AKV_SEL(masked ? mask : NULL, i, n, 4 * AKV_L);

		AGK_UNROLL(4)
		for (int g = 0; g < 4; g++)
			AKV_ABS_STEP(m[g], (bb >> (g * AKV_L)) & (uint32) AGK_LOW(AKV_L), v + i + g * AKV_L);
	}
	for (; i < n; i += AKV_L)
		AKV_ABS_STEP(m[0], AKV_SEL(masked ? mask : NULL, i, n, AKV_L), v + i);
	for (int k = 0; k < AKV_L; k++)
		if (!fin[k])
			return false;
	for (int g = 1; g < 4; g++)
		m[0] = AKV_MAX(akv_f64, m[0], m[g]);
	for (int k = 0; k < AKV_L; k++)
		r = Max(r, m[0][k]);
	*mx = r;
	return true;
}

static bool
AKV_RFN(absmax_f64)(const double *v, const uint64 *mask, int n, double *mx)
{
	return mask ? AKV_RFN(absmax_f64_t)(v, mask, n, mx, true) : AKV_RFN(absmax_f64_t)(v, mask, n, mx, false);
}

AKV_INL bool
AKV_RFN(minmax_i32_t)(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, int n, const bool masked)
{
	const akv_i32 imax = (akv_i32) {} + INT32_MAX,
				imin = (akv_i32) {} + INT32_MIN;
	akv_i32		lo = imax,
				hi = imin;
	uint32		any = 0;
	int			i = 0;

	for (; !masked && i + AKV_L32 <= n; i += AKV_L32)
	{
		const akv_i32 x = AKV_LDT(akv_i32, v + i);

		lo = AKV_MIN(akv_i32, x, lo);
		hi = AKV_MAX(akv_i32, x, hi);
	}
	for (; i < n; i += AKV_L32)
	{
		const uint32 b = AKV_SEL(masked ? mask : NULL, i, n, AKV_L32);
		const akv_i32 x = AKV_LDT(akv_i32, v + i),
					m = AKV_LANES32(b);

		any |= b;
		lo = AKV_MIN(akv_i32, AKV_BLEND(akv_i32, m, x, imax), lo);
		hi = AKV_MAX(akv_i32, AKV_BLEND(akv_i32, m, x, imin), hi);
	}
	if (!masked ? n == 0 : !any)
		return false;
	*mn = lo[0];
	*mx = hi[0];
	for (int k = 1; k < AKV_L32; k++)
	{
		*mn = Min(*mn, lo[k]);
		*mx = Max(*mx, hi[k]);
	}
	return true;
}

static bool
AKV_RFN(minmax_i32)(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, int n)
{
	return mask ? AKV_RFN(minmax_i32_t)(mn, mx, v, mask, n, true) : AKV_RFN(minmax_i32_t)(mn, mx, v, mask, n, false);
}

#ifndef AKV_NO_MINMAX_F64
/*
 * Doubles: the minimum is NaN only when every selected row is (the running minimum then never left +Inf and the maximum never left -Inf: lo <= hi tells
 * that a non-NaN row was seen), the maximum when any is; NaN rows are noted on the side. An unselected lane is made a NaN, which the running values ignore.
 */
AKV_INL bool
AKV_RFN(minmax_f64_t)(double *mn, double *mx, const double *v, const uint64 *mask, int n, const bool masked)
{
	const akv_f64 pinf = (akv_f64) {} + INFINITY,
				ninf = (akv_f64) {} - INFINITY;
	akv_f64		lo[4],
				hi[4];
	akv_s		anynan[4] = {{}, {}, {}, {}};
	uint32		any = 0;
	int			i = 0;

	for (int g = 0; g < 4; g++)
		lo[g] = pinf, hi[g] = ninf;
	for (; i + 4 * AKV_L <= n; i += 4 * AKV_L)
	{
		const uint32 bb = masked ? AKV_SEL(mask, i, n, 4 * AKV_L) : 0;

		AGK_UNROLL(4)
		for (int g = 0; g < 4; g++)
		{
			const uint32 b = (bb >> (g * AKV_L)) & (uint32) AGK_LOW(AKV_L);
			const akv_f64 x = AKV_LDT(akv_f64, v + i + g * AKV_L);
			akv_f64		xs;
			akv_s		m = (akv_s) {} - 1;

			if (masked)
				m = AKV_LANES64(b);
			any |= b;
			anynan[g] |= (akv_s) (x != x) & m;
			xs = (akv_f64) ((akv_s) x | ~m);
			lo[g] = AKV_MIN(akv_f64, xs, lo[g]);
			hi[g] = AKV_MAX(akv_f64, xs, hi[g]);
		}
	}
	for (; i < n; i += AKV_L)
	{
		const uint32 b = AKV_SEL(masked ? mask : NULL, i, n, AKV_L);
		const akv_f64 x = AKV_LDT(akv_f64, v + i);
		const akv_s m = AKV_LANES64(b);
		akv_f64		xs;

		any |= b;
		anynan[0] |= (akv_s) (x != x) & m;
		xs = (akv_f64) ((akv_s) x | ~m);
		lo[0] = AKV_MIN(akv_f64, xs, lo[0]);
		hi[0] = AKV_MAX(akv_f64, xs, hi[0]);
	}
	if (!masked ? n == 0 : !any)
		return false;
	for (int g = 1; g < 4; g++)
	{
		lo[0] = AKV_MIN(akv_f64, lo[g], lo[0]);
		hi[0] = AKV_MAX(akv_f64, hi[g], hi[0]);
	}
	{
		double		lov = lo[0][0],
					hiv = hi[0][0];
		bool		nan = false;

		for (int k = 1; k < AKV_L; k++)
		{
			lov = Min(lov, lo[0][k]);
			hiv = Max(hiv, hi[0][k]);
		}
		for (int g = 0; g < 4; g++)
			for (int k = 0; k < AKV_L; k++)
				nan |= anynan[g][k] != 0;
		*mn = agk_canon_minmax(lov <= hiv ? lov : __builtin_nan(""));
		*mx = agk_canon_minmax(nan ? __builtin_nan("") : hiv);
	}
	return true;
}

static bool
AKV_RFN(minmax_f64)(double *mn, double *mx, const double *v, const uint64 *mask, int n)
{
	return mask ? AKV_RFN(minmax_f64_t)(mn, mx, v, mask, n, true) : AKV_RFN(minmax_f64_t)(mn, mx, v, mask, n, false);
}
#endif

#endif							/* AKV_OWN_REDUCE */
