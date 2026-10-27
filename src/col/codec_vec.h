/*
 * col/codec_vec.h - the SIMD kernels of the chunk codec, written once with GCC/Clang vector extensions and compiled by each level file at its
 * own width (CC_VB bytes per vector: 16 for NEON, 32 for AVX2, 64 for AVX-512) under the level's target options. Included by col/codec_avx2.c,
 * col/codec_avx512.c and col/codec_neon.c after they define CC_VB, CC_FN(x) and CC_MOVEMASK(m), the one operation the extensions lack: the sign
 * bit of every lane of a u32 compare result (all ones or zero per lane) as the bits of a word, lane k to bit k (CC_ROWMASK, below, may replace it).
 * Bit-identical to the scalar table in col/codec_kern.c; the tails run the scalar kernels, and slots past the last value of rle_expand32 are
 * unspecified (the caller allocates SIFT_COL_PAD).
 */

typedef uint32 cc_vu32 __attribute__((vector_size(CC_VB), aligned(4), may_alias));
typedef int32 cc_vi32 __attribute__((vector_size(CC_VB), aligned(4), may_alias));
typedef float cc_vf32 __attribute__((vector_size(CC_VB), aligned(4), may_alias));
typedef int64 cc_vi64 __attribute__((vector_size(CC_VB), aligned(8), may_alias));
typedef uint64 cc_vu64 __attribute__((vector_size(CC_VB), aligned(8), may_alias));
typedef double cc_vf64 __attribute__((vector_size(CC_VB), aligned(8), may_alias));
/* one u32 or float per 64-bit lane */
typedef int32 cc_hi32 __attribute__((vector_size(CC_VB / 2), aligned(4), may_alias));
typedef uint32 cc_hu32 __attribute__((vector_size(CC_VB / 2), aligned(4), may_alias));
typedef float cc_hf32 __attribute__((vector_size(CC_VB / 2), aligned(4), may_alias));

#define CC_LANES	(CC_VB / 4)		/* u32 lanes per vector */
#define CC_LANES64	(CC_VB / 8)
#define CC_NV		(32 / CC_LANES)	/* vectors per row of 32 values */
#define CC_INLINE	static inline __attribute__((always_inline))
/* the loops over the vectors of a row (at most 8) keep the row in registers only when unrolled, which -O2 does not */
#define CC_UNROLL	_Pragma("GCC unroll 8")

/*
 * A row of vectors on the stack. The vector type is aligned(4) for the loads through unaligned pointers, but GCC still assumes the stack array
 * is CC_VB aligned and emits aligned moves for it, which the stack frame of an ASan build (GCC 13) does not honor: the alignment is explicit.
 */
typedef cc_vu32 cc_row_t[CC_NV] __attribute__((aligned(CC_VB)));

/* Row r of a block: the 32 values (CC_NV vectors) at bit r * W of the lane streams, plus add (wrapping), into out[] */
CC_INLINE void
CC_FN(row)(const uint32 *a, uint32 s, uint32 W, uint32 mask, uint32 add, cc_vu32 *out)
{
	CC_UNROLL
	for (int q = 0; q < CC_NV; q++)
	{
		cc_vu32		x = *(const cc_vu32 *) (a + q * CC_LANES) >> s;

		if (s + W > 32)
			x |= *(const cc_vu32 *) (a + 32 + q * CC_LANES) << (32 - s);
		out[q] = (x & mask) + add;
	}
}

static inline void
CC_FN(unpack32)(const uint32 *src, uint32 W, uint32 rows, uint32 base, uint32 *dst)
{
	uint32		mask = W >= 32 ? ~0u : (1u << W) - 1;

	if (W == 0)
	{
		for (uint64 i = 0; i < (uint64) rows * 32; i++)
			dst[i] = base;
		return;
	}
	while (rows)
	{
		uint32		R = rows > 32 ? 32 : rows;

		for (uint32 r = 0; r < R; r++)
		{
			uint32		bp = r * W;
			cc_row_t	v;

			CC_FN(row)(src + (size_t) (bp >> 5) * 32, bp & 31, W, mask, base, v);
			CC_UNROLL
			for (int q = 0; q < CC_NV; q++)
				*(cc_vu32 *) (dst + r * 32 + q * CC_LANES) = v[q];
		}
		src += R == 32 ? (size_t) 32 * W : (size_t) 32 * ((R * W + 31) / 32);
		dst += R * 32;
		rows -= R;
	}
}

#ifndef CC_ROWMASK
/* The bit of each value of a row that is <= span, unsigned: a level whose compare gives bits or folds a row better than one movemask per vector defines CC_ROWMASK */
CC_INLINE uint32
CC_FN(rowmask)(const cc_vu32 *v, cc_vu32 span)
{
	uint32		m = 0;

	CC_UNROLL
	for (int q = 0; q < CC_NV; q++)
		m |= CC_MOVEMASK((cc_vu32) (v[q] <= span)) << (q * CC_LANES);
	return m;
}
#define CC_ROWMASK CC_FN(rowmask)
#endif

/* One bit per value of a row, 32 to a u32 of bits (little endian) */
static inline void
CC_FN(unpack_range32)(const uint32 *src, uint32 W, uint32 rows, uint32 lo, uint32 span, uint64 *bits)
{
	uint32		mask = W >= 32 ? ~0u : (1u << W) - 1;
	uint32		row = 0;
	uint32	   *out = (uint32 *) bits;
	cc_vu32		vspan = (cc_vu32) {} + span;

	if (rows & 1)
		out[rows] = 0;
	if (W == 0)
	{
		for (uint32 r = 0; r < rows; r++)
			out[r] = (0u - lo) <= span ? ~0u : 0;
		return;
	}
	while (row < rows)
	{
		uint32		R = rows - row > 32 ? 32 : rows - row;

		for (uint32 r = 0; r < R; r++, row++)
		{
			uint32		bp = r * W;
			cc_row_t	v;

			CC_FN(row)(src + (size_t) (bp >> 5) * 32, bp & 31, W, mask, 0u - lo, v);
			out[row] = CC_ROWMASK(v, vspan);
		}
		src += R == 32 ? (size_t) 32 * W : (size_t) 32 * ((R * W + 31) / 32);
	}
}

/*
 * The lanes of x moved up by s with zeros below, for the log steps of the prefix sums (index N is the first lane of the zero operand), and the
 * last lane broadcast: shuffle indices are constants, so the lists are macros per lane count.
 */
#define CC_UP(i, s, N)	((i) < (s) ? (N) : (i) - (s))
#define CC_TOP(i, s, N)	((N) - 1)
#define CC_LIST2(F, s, N)	F(0, s, N), F(1, s, N)
#define CC_LIST4(F, s, N)	CC_LIST2(F, s, N), F(2, s, N), F(3, s, N)
#define CC_LIST8(F, s, N)	CC_LIST4(F, s, N), F(4, s, N), F(5, s, N), F(6, s, N), F(7, s, N)
#define CC_LIST16(F, s, N)	CC_LIST8(F, s, N), F(8, s, N), F(9, s, N), F(10, s, N), F(11, s, N), F(12, s, N), F(13, s, N), F(14, s, N), F(15, s, N)
#define CC_LISTN(n, F, s, N)	CC_LISTN_(n, F, s, N)
#define CC_LISTN_(n, F, s, N)	CC_LIST##n(F, s, N)
/*
 * __builtin_shufflevector is Clang's and GCC 12's. GCC 11 (el9) has __builtin_shuffle, whose mask is a vector of the lanes' width and count (x itself is
 * the type: the callers shuffle unsigned lanes) and which cannot change the length of the vector, so CC_HALF takes a half of x through a union.
 */
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 12)
#define CC_SHUFFLEVECTOR
#define CC_SHUF(x, y, F, s, N)	__builtin_shufflevector(x, y, CC_LISTN(N, F, s, N))
#else
#define CC_SHUF(x, y, F, s, N)	__builtin_shuffle(x, y, (__typeof__(x)) {CC_LISTN(N, F, s, N)})
#endif
#define CC_SCAN(x, N) \
	do { \
		x += CC_SHUF(x, (__typeof__(x)) {}, CC_UP, 1, N); \
		x += CC_SHUF(x, (__typeof__(x)) {}, CC_UP, 2, N); \
		if (N > 4) \
			x += CC_SHUF(x, (__typeof__(x)) {}, CC_UP, 4, N); \
		if (N > 8) \
			x += CC_SHUF(x, (__typeof__(x)) {}, CC_UP, 8, N); \
	} while (0)

#if CC_VB == 16
#define CC_L32 4
#define CC_L64 2
#elif CC_VB == 32
#define CC_L32 8
#define CC_L64 4
#else
#define CC_L32 16
#define CC_L64 8
#endif

/* dst[0] = first, dst[i] = dst[i - 1] + a[i] + add: the block prefix of the first vector has a[0] zeroed, later ones carry the last lane forward */
static inline void
CC_FN(prefix32)(const uint32 *a, uint32 n, uint32 *dst, uint32 first, uint32 add)
{
	cc_vu32		vadd = (cc_vu32) {} + add,
				carry = (cc_vu32) {} + first;
	uint32		i = 0;

	if (n == 0)
		return;
	for (; i + CC_LANES <= n; i += CC_LANES)
	{
		cc_vu32		x = *(const cc_vu32 *) (a + i) + vadd;

		if (i == 0)
			x[0] = 0;
		CC_SCAN(x, CC_L32);
		x += carry;
		*(cc_vu32 *) (dst + i) = x;
		carry = CC_SHUF(x, x, CC_TOP, 0, CC_L32);
	}
	if (i < n)
	{
		uint32		run = i ? dst[i - 1] : first;

		if (i == 0)
			dst[0] = run;
		for (uint32 k = i ? i : 1; k < n; k++)
		{
			run += a[k] + add;
			dst[k] = run;
		}
	}
}

static inline void
CC_FN(prefix64)(const uint32 *a, uint32 n, int64 *dst, int64 first, int64 add)
{
	cc_vu64		vadd = (cc_vu64) {} + (uint64) add,
				carry = (cc_vu64) {} + (uint64) first;
	uint32		i = 0;

	if (n == 0)
		return;
	for (; i + CC_LANES64 <= n; i += CC_LANES64)
	{
		cc_vu64		x = __builtin_convertvector(*(const cc_hu32 *) (a + i), cc_vu64) + vadd;

		if (i == 0)
			x[0] = 0;
		CC_SCAN(x, CC_L64);
		x += carry;
		*(cc_vu64 *) (dst + i) = x;
		carry = CC_SHUF(x, x, CC_TOP, 0, CC_L64);
	}
	if (i < n)
	{
		uint64		run = i ? (uint64) dst[i - 1] : (uint64) first;

		if (i == 0)
			dst[0] = (int64) run;
		for (uint32 k = i ? i : 1; k < n; k++)
		{
			run += (uint64) a[k] + (uint64) add;
			dst[k] = (int64) run;
		}
	}
}

static inline uint32
CC_FN(max32)(const uint32 *a, uint32 n)
{
	cc_vu32		m __attribute__((aligned(CC_VB))) = {};	/* read by lane below, so it lives on the stack: aligned as cc_row_t */
	uint32		i = 0,
				r = 0;

	for (; i + CC_LANES <= n; i += CC_LANES)
	{
		cc_vu32		x = *(const cc_vu32 *) (a + i),
					g = (cc_vu32) (x > m);

		m = (x & g) | (m & ~g);
	}
	for (int k = 0; k < CC_LANES; k++)
		r = m[k] > r ? m[k] : r;
	for (; i < n; i++)
		r = a[i] > r ? a[i] : r;
	return r;
}

/*
 * The doubles of the ALP node as the scalar kernels form them, (double) (dbase + u) / div. F8 converts (int32) (u ^ 2^31) and adds 2^31 + dbase, both
 * exact because the encoder keeps |digits| below 2^52; F4 divides in float, a vector of CC_LANES, and promotes the two halves. keys = the order
 * preserving int64 of the double: bits ^ ((bits >> 63) >> 1) with an arithmetic shift of the sign. dst is int64 keys or doubles, both 8 bytes;
 * the caller runs the scalar kernel on the tail.
 */
#define CC_IDX(i, s, N)	((i) + (s))
#ifdef CC_SHUFFLEVECTOR
#define CC_HALF(x, off)	__builtin_shufflevector(x, x, CC_LISTN(CC_L64, CC_IDX, off, 0))
#else
#define CC_HALF(x, off)	(((union {__typeof__(x) v; cc_hf32 h[2];}) {x}).h[(off) / CC_LANES64])
#endif

CC_INLINE void
CC_FN(put)(int64 *dst, cc_vf64 d, bool keys)
{
	cc_vi64		b = (cc_vi64) d;

	if (keys)
		b ^= (cc_vi64) ((cc_vu64) (b >> 63) >> 1);
	*(cc_vi64 *) dst = b;
}

CC_INLINE uint32
CC_FN(alp_run)(int64 *dst, const uint32 *u, uint32 n, int64 dbase, double div, bool f4, bool keys)
{
	uint32		i = 0;

	if (f4)
		for (; i + CC_LANES <= n; i += CC_LANES)
		{
			cc_vf32		q = __builtin_convertvector((cc_vi32) (*(const cc_vu32 *) (u + i) + (uint32) dbase), cc_vf32) / (float) div;

			CC_FN(put)(dst + i, __builtin_convertvector((cc_hf32) CC_HALF(q, 0), cc_vf64), keys);
			CC_FN(put)(dst + i + CC_LANES64, __builtin_convertvector((cc_hf32) CC_HALF(q, CC_LANES64), cc_vf64), keys);
		}
	else
		for (; i + CC_LANES64 <= n; i += CC_LANES64)
			CC_FN(put)(dst + i, (__builtin_convertvector((cc_hi32) (*(const cc_hu32 *) (u + i) ^ 0x80000000u), cc_vf64) + (2147483648.0 + (double) dbase)) / div, keys);
	return i;
}

static inline void
CC_FN(alp_keys_f8)(int64 *dst, const uint32 *u, uint32 n, int64 dbase, double div)
{
	uint32		i = CC_FN(alp_run)(dst, u, n, dbase, div, false, true);

	sift_cc_kernels_scalar.alp_keys_f8(dst + i, u + i, n - i, dbase, div);
}

static inline void
CC_FN(alp_keys_f4)(int64 *dst, const uint32 *u, uint32 n, int64 dbase, float div)
{
	uint32		i = CC_FN(alp_run)(dst, u, n, dbase, div, true, true);

	sift_cc_kernels_scalar.alp_keys_f4(dst + i, u + i, n - i, dbase, div);
}

static inline void
CC_FN(alp_f8)(double *dst, const uint32 *u, uint32 n, int64 dbase, double div)
{
	uint32		i = CC_FN(alp_run)((int64 *) dst, u, n, dbase, div, false, false);

	sift_cc_kernels_scalar.alp_f8(dst + i, u + i, n - i, dbase, div);
}

static inline void
CC_FN(alp_f4)(double *dst, const uint32 *u, uint32 n, int64 dbase, float div)
{
	uint32		i = CC_FN(alp_run)((int64 *) dst, u, n, dbase, div, true, false);

	sift_cc_kernels_scalar.alp_f4(dst + i, u + i, n - i, dbase, div);
}

static inline void
CC_FN(rle_expand32)(const uint32 *vals, const uint32 *lens, uint32 nruns, uint32 *dst)
{
	for (uint32 r = 0; r < nruns; r++)
	{
		cc_vu32		v = (cc_vu32) {} + vals[r];
		uint32		len = lens[r];

		for (uint32 k = 0; k < len; k += CC_LANES)
			*(cc_vu32 *) (dst + k) = v;
		dst += len;
	}
}

static inline void
CC_FN(scale32)(uint32 *a, uint32 n, uint32 base, uint32 mul)
{
	uint32		i = 0;

	for (; i + CC_LANES <= n; i += CC_LANES)
		*(cc_vu32 *) (a + i) = *(const cc_vu32 *) (a + i) * mul + base;
	sift_cc_kernels_scalar.scale32(a + i, n - i, base, mul);
}

static inline void
CC_FN(scale64)(int64 *a, uint32 n, int64 base, int64 mul)
{
	uint32		i = 0;

	for (; i + CC_LANES64 <= n; i += CC_LANES64)
		*(cc_vu64 *) (a + i) = *(const cc_vu64 *) (a + i) * (uint64) mul + (uint64) base;
	sift_cc_kernels_scalar.scale64(a + i, n - i, base, mul);
}

#define CC_TABLE(isa) \
	const SiftCcKernels sift_cc_kernels_##isa = { \
		.unpack32 = CC_FN(unpack32), \
		.unpack_range32 = CC_FN(unpack_range32), \
		.prefix32 = CC_FN(prefix32), \
		.prefix64 = CC_FN(prefix64), \
		.max32 = CC_FN(max32), \
		.alp_keys_f8 = CC_FN(alp_keys_f8), \
		.alp_keys_f4 = CC_FN(alp_keys_f4), \
		.rle_expand32 = CC_FN(rle_expand32), \
		.scale32 = CC_FN(scale32), \
		.scale64 = CC_FN(scale64), \
		.alp_f8 = CC_FN(alp_f8), \
		.alp_f4 = CC_FN(alp_f4), \
	}
