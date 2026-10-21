/*
 * agg/kern_avx512.c - AVX-512 (F/BW/VL/DQ/VBMI2/VPOPCNTDQ) aggregation operators. Compares produce mask registers directly
 * (8 double rows per instruction), selections are applied as merging masked min/max and masked loads, lookups are masked
 * VPGATHER. Results are bit-identical to the scalar table (agg/kern.c). Slots that a per-group scatter does not speed up
 * (count_gid, sum_gid_*) are the scalar functions.
 */
#include "core/host.h"

#include "agg/kern_int.h"

#if defined(__x86_64__)
#include <immintrin.h>

#define K5 static inline __attribute__((always_inline)) SIFT_TARGET_AVX512
#define F5 static SIFT_TARGET_AVX512

#define L5(p)		_mm512_loadu_si512((const void *) (p))
#define S5(p, x)	_mm512_storeu_si512((void *) (p), x)
#define LPD(p)		_mm512_loadu_pd((const void *) (p))
#define SEL16(mask, i)	((__mmask16) ((mask)[(i) >> 6] >> ((i) & 63)))
#define SEL8(mask, i)	((__mmask8) ((mask)[(i) >> 6] >> ((i) & 63)))

/* ---- predicates ---- */

/* Same predicate mapping as the AVX2 level: "not <=" / "not <" are true for a NaN row; a NaN constant is the scalar function's */
F5 void
cmp_f64_avx512(uint64 *out, const double *v, int n, SiftCmpOp op, double c)
{
	const __m512d vc = _mm512_set1_pd(c);

	if (c != c)
	{
		agk_cmp_f64_scalar(out, v, n, op, c);
		return;
	}
#define CMPF(IMM) AGK_BITS(out, n, 8, 0, i, _mm512_cmp_pd_mask(LPD(v + i), vc, IMM))
	switch (op)
	{
		case SCMP_EQ:
			CMPF(_CMP_EQ_OQ);
			break;
		case SCMP_NE:
			CMPF(_CMP_NEQ_UQ);
			break;
		case SCMP_LT:
			CMPF(_CMP_LT_OQ);
			break;
		case SCMP_LE:
			CMPF(_CMP_LE_OQ);
			break;
		case SCMP_GT:
			CMPF(_CMP_NLE_UQ);
			break;
		default:
			CMPF(_CMP_NLT_UQ);
			break;
	}
#undef CMPF
}

F5 void
between_f64_avx512(uint64 *out, const double *v, int n, double lo, double hi)
{
	const __m512d vlo = _mm512_set1_pd(lo),
				vhi = _mm512_set1_pd(hi);

	if (lo != lo || hi != hi)
	{
		agk_between_f64_scalar(out, v, n, lo, hi);
		return;
	}
	AGK_BITS(out, n, 8, 0, i,
			 _mm512_mask_cmp_pd_mask(_mm512_cmp_pd_mask(LPD(v + i), vlo, _CMP_GE_OQ), LPD(v + i), vhi, _CMP_LE_OQ));
}

/* Rows whose gathered dword has a nonzero low byte (the LUT has SIFT_AGG_PAD readable bytes after it) */
K5 __mmask16
nonzero_byte(__m512i g)
{
	return _mm512_test_epi32_mask(g, _mm512_set1_epi32(0xFF));
}

/* Lanes outside the table are masked out of the gather (zero, so they read as "not set") */
K5 __mmask16
lut_i32_step(const uint8 *lut, __m512i x, __m512i vb, __m512i vn)
{
	__m512i		t = _mm512_sub_epi32(x, vb);

	return nonzero_byte(_mm512_mask_i32gather_epi32(_mm512_setzero_si512(), _mm512_cmplt_epu32_mask(t, vn), t, lut, 1));
}

F5 void
lut_i32_avx512(uint64 *out, const int32 *v, int n, int32 base, uint32 nlut, const uint8 *lut)
{
	const __m512i vb = _mm512_set1_epi32(base),
				vn = _mm512_set1_epi32(nlut);

	AGK_BITS(out, n, 16, 0, i, lut_i32_step(lut, L5(v + i), vb, vn));
}

/* bit e = bit parent[e] of rowmask: gather the dword that holds it (k: the lanes that exist), shift it down */
K5 __mmask16
row_bits(const uint64 *rowmask, __m512i p, __mmask16 k)
{
	__m512i		g = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), k, _mm512_srli_epi32(p, 5), rowmask, 4);

	return _mm512_test_epi32_mask(_mm512_srlv_epi32(g, _mm512_and_si512(p, _mm512_set1_epi32(31))), _mm512_set1_epi32(1));
}

F5 void
gather_bits_avx512(uint64 *out, const uint64 *rowmask, const uint32 *parent, int m)
{
	AGK_BITS2(out, m, 16, 0, e, row_bits(rowmask, L5(parent + e), 0xFFFF),
			  row_bits(rowmask, _mm512_maskz_loadu_epi32(AGK_LANES(m - e, 16), parent + e), AGK_LANES(m - e, 16)));
}

/* ---- group ids ---- */

F5 void
gid_code_avx512(uint32 *gid, const uint32 *code, const uint32 *map, int n, uint32 mult)
{
	const __m512i vm = _mm512_set1_epi32(mult),
				z = _mm512_setzero_si512();
	int			i = 0;

	for (; i + 16 <= n; i += 16)
		S5(gid + i, _mm512_add_epi32(L5(gid + i), _mm512_mullo_epi32(_mm512_i32gather_epi32(L5(code + i), map, 4), vm)));
	if (i < n)
	{
		const __mmask16 k = AGK_LOW(n - i);
		__m512i		g = _mm512_mask_i32gather_epi32(z, k, _mm512_maskz_loadu_epi32(k, code + i), map, 4);

		_mm512_mask_storeu_epi32(gid + i, k, _mm512_add_epi32(_mm512_maskz_loadu_epi32(k, gid + i), _mm512_mullo_epi32(g, vm)));
	}
}

F5 void
gid_i32_avx512(uint32 *gid, const int32 *v, int32 base, const uint32 *map, int n, uint32 mult)
{
	const __m512i vm = _mm512_set1_epi32(mult),
				vb = _mm512_set1_epi32(base),
				z = _mm512_setzero_si512();
	int			i = 0;

	for (; i + 16 <= n; i += 16)
		S5(gid + i, _mm512_add_epi32(L5(gid + i),
									 _mm512_mullo_epi32(_mm512_i32gather_epi32(_mm512_sub_epi32(L5(v + i), vb), map, 4), vm)));
	if (i < n)
	{
		const __mmask16 k = AGK_LOW(n - i);
		__m512i		g = _mm512_mask_i32gather_epi32(z, k, _mm512_sub_epi32(_mm512_maskz_loadu_epi32(k, v + i), vb), map, 4);

		_mm512_mask_storeu_epi32(gid + i, k, _mm512_add_epi32(_mm512_maskz_loadu_epi32(k, gid + i), _mm512_mullo_epi32(g, vm)));
	}
}

/* ---- reductions ---- */

/* 16 rows per step: the two halves are sign-extended under their mask bits into two 8 x int64 accumulators */
K5 int64
sum_i32_t(const int32 *v, const uint64 *mask, int n, const bool masked)
{
	__m512i		a0 = _mm512_setzero_si512(),
				a1 = a0;

	for (int i = 0; i < n; i += 16)
	{
		const uint32 k = masked ? SEL16(mask, i) : (uint32) AGK_LANES(n - i, 16);

		a0 = _mm512_add_epi64(a0, _mm512_maskz_cvtepi32_epi64((__mmask8) k, _mm256_loadu_si256((const __m256i *) (v + i))));
		a1 = _mm512_add_epi64(a1, _mm512_maskz_cvtepi32_epi64((__mmask8) (k >> 8), _mm256_loadu_si256((const __m256i *) (v + i + 8))));
	}
	return _mm512_reduce_add_epi64(_mm512_add_epi64(a0, a1));
}

F5 int64
sum_i32_avx512(const int32 *v, const uint64 *mask, int n)
{
	return mask ? sum_i32_t(v, mask, n, true) : sum_i32_t(v, mask, n, false);
}

/*
 * The specified accumulators are eight chains of n / 8 dependent adds: plain adds on two ymm accumulators (the latency of a
 * merge-masked or a zmm add is longer, docs/KERNELS.md; a masked load costs more than a plain one, so only the rows of a
 * mask and the tail use it); an unselected lane is loaded as +0.0, which adds nothing (an accumulator is never -0.0).
 */
K5 double
sum_f64_t(const double *v, const uint64 *mask, int n, const bool masked)
{
	__m256d		a0 = _mm256_setzero_pd(),
				a1 = a0;
	double		a[8];

	int			i = 0;

	for (; !masked && i + 8 <= n; i += 8)
	{
		a0 = _mm256_add_pd(a0, _mm256_loadu_pd(v + i));
		a1 = _mm256_add_pd(a1, _mm256_loadu_pd(v + i + 4));
	}
	for (; i < n; i += 8)
	{
		const uint32 k = masked ? SEL8(mask, i) : (uint32) AGK_LANES(n - i, 8);

		a0 = _mm256_add_pd(a0, _mm256_maskz_loadu_pd(k & 15, v + i));
		a1 = _mm256_add_pd(a1, _mm256_maskz_loadu_pd(k >> 4, v + i + 4));
	}
	_mm256_storeu_pd(a, a0);
	_mm256_storeu_pd(a + 4, a1);
	return agk_canon_nan(((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7])));
}

F5 double
sum_f64_avx512(const double *v, const uint64 *mask, int n)
{
	return mask ? sum_f64_t(v, mask, n, true) : sum_f64_t(v, mask, n, false);
}

/* Unselected lanes are loaded as +0.0; VFPCLASS 0x99 is QNaN, SNaN, +Inf, -Inf */
K5 bool
absmax_f64_t(const double *v, const uint64 *mask, int n, double *mx, const bool masked)
{
	__m512d		m0 = _mm512_setzero_pd(),
				m1 = m0;
	uint32		bad = 0;

	for (int i = 0; i < n; i += 16)
	{
		const uint32 k = masked ? SEL16(mask, i) : (uint32) AGK_LANES(n - i, 16);
		const __m512d x0 = _mm512_maskz_loadu_pd((__mmask8) k, v + i),
					x1 = _mm512_maskz_loadu_pd((__mmask8) (k >> 8), v + i + 8);

		bad |= _mm512_fpclass_pd_mask(x0, 0x99) | _mm512_fpclass_pd_mask(x1, 0x99);
		m0 = _mm512_max_pd(m0, _mm512_abs_pd(x0));
		m1 = _mm512_max_pd(m1, _mm512_abs_pd(x1));
	}
	if (bad)
		return false;
	*mx = _mm512_reduce_max_pd(_mm512_max_pd(m0, m1));
	return true;
}

F5 bool
absmax_f64_avx512(const double *v, const uint64 *mask, int n, double *mx)
{
	return mask ? absmax_f64_t(v, mask, n, mx, true) : absmax_f64_t(v, mask, n, mx, false);
}

K5 bool
minmax_i32_t(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, int n, const bool masked)
{
	__m512i		lo = _mm512_set1_epi32(INT32_MAX),
				hi = _mm512_set1_epi32(INT32_MIN);
	uint32		any = 0;
	int			i = 0;

	for (; !masked && i + 16 <= n; i += 16)
	{
		const __m512i x = L5(v + i);

		lo = _mm512_min_epi32(lo, x);
		hi = _mm512_max_epi32(hi, x);
	}
	for (; i < n; i += 16)
	{
		const __mmask16 k = masked ? SEL16(mask, i) : (__mmask16) AGK_LANES(n - i, 16);
		const __m512i x = L5(v + i);

		any |= k;
		lo = _mm512_mask_min_epi32(lo, k, lo, x);
		hi = _mm512_mask_max_epi32(hi, k, hi, x);
	}
	if (!masked ? n == 0 : !any)
		return false;
	*mn = _mm512_reduce_min_epi32(lo);
	*mx = _mm512_reduce_max_epi32(hi);
	return true;
}

F5 bool
minmax_i32_avx512(int32 *mn, int32 *mx, const int32 *v, const uint64 *mask, int n)
{
	return mask ? minmax_i32_t(mn, mx, v, mask, n, true) : minmax_i32_t(mn, mx, v, mask, n, false);
}

K5 bool
minmax_i64_t(int64 *mn, int64 *mx, const int64 *v, const uint64 *mask, int n, const bool masked)
{
	__m512i		lo = _mm512_set1_epi64(INT64_MAX),
				hi = _mm512_set1_epi64(INT64_MIN);
	uint32		any = 0;
	int			i = 0;

	for (; !masked && i + 8 <= n; i += 8)
	{
		const __m512i x = L5(v + i);

		lo = _mm512_min_epi64(lo, x);
		hi = _mm512_max_epi64(hi, x);
	}
	for (; i < n; i += 8)
	{
		const __mmask8 k = masked ? SEL8(mask, i) : (__mmask8) AGK_LANES(n - i, 8);
		const __m512i x = L5(v + i);

		any |= k;
		lo = _mm512_mask_min_epi64(lo, k, lo, x);
		hi = _mm512_mask_max_epi64(hi, k, hi, x);
	}
	if (!masked ? n == 0 : !any)
		return false;
	*mn = _mm512_reduce_min_epi64(lo);
	*mx = _mm512_reduce_max_epi64(hi);
	return true;
}

F5 bool
minmax_i64_avx512(int64 *mn, int64 *mx, const int64 *v, const uint64 *mask, int n)
{
	return mask ? minmax_i64_t(mn, mx, v, mask, n, true) : minmax_i64_t(mn, mx, v, mask, n, false);
}

/*
 * VMINPD / VMAXPD return their second operand when the first is a NaN, so with the running value second a NaN never enters
 * it; NaN rows are counted on the side. The minimum is NaN only when every selected row is (the running minimum then never
 * left +Inf and the maximum never left -Inf: lo <= hi tells that a non-NaN row was seen), the maximum when any is.
 * Four accumulator pairs, 32 rows a step: the chain of one is too long for the loop to go at the load rate. A group past
 * the end has an empty mask, so its load touches nothing.
 */
K5 bool
minmax_f64_t(double *mn, double *mx, const double *v, const uint64 *mask, int n, const bool masked)
{
	__m512d		lo[4],
				hi[4];
	uint32		anynan = 0,
				any = 0;
	int			i = 0;

	for (int g = 0; g < 4; g++)
	{
		lo[g] = _mm512_set1_pd(INFINITY);
		hi[g] = _mm512_set1_pd(-INFINITY);
	}
	for (; !masked && i + 32 <= n; i += 32)
	{
		AGK_UNROLL(4)
		for (int g = 0; g < 4; g++)
		{
			const __m512d x = LPD(v + i + 8 * g);

			anynan |= _mm512_cmp_pd_mask(x, x, _CMP_UNORD_Q);
			lo[g] = _mm512_min_pd(x, lo[g]);
			hi[g] = _mm512_max_pd(x, hi[g]);
		}
	}
	for (; i < n; i += 32)
	{
		AGK_UNROLL(4)
		for (int g = 0; g < 4; g++)
		{
			const int	r = n - i - 8 * g;
			const __mmask8 k = r <= 0 ? 0 : masked ? SEL8(mask, i + 8 * g) : (__mmask8) AGK_LANES(r, 8);
			const __m512d x = _mm512_maskz_loadu_pd(k, v + i + 8 * g);

			any |= k;
			anynan |= _mm512_mask_cmp_pd_mask(k, x, x, _CMP_UNORD_Q);
			lo[g] = _mm512_mask_min_pd(lo[g], k, x, lo[g]);
			hi[g] = _mm512_mask_max_pd(hi[g], k, x, hi[g]);
		}
	}
	if (!masked ? n == 0 : !any)
		return false;
	for (int g = 1; g < 4; g++)
	{
		lo[0] = _mm512_min_pd(lo[g], lo[0]);
		hi[0] = _mm512_max_pd(hi[g], hi[0]);
	}
	{
		const double l = _mm512_reduce_min_pd(lo[0]),
					h = _mm512_reduce_max_pd(hi[0]);

		*mn = agk_canon_minmax(l <= h ? l : __builtin_nan(""));
		*mx = agk_canon_minmax(anynan ? __builtin_nan("") : h);
	}
	return true;
}

F5 bool
minmax_f64_avx512(double *mn, double *mx, const double *v, const uint64 *mask, int n)
{
	return mask ? minmax_f64_t(mn, mx, v, mask, n, true) : minmax_f64_t(mn, mx, v, mask, n, false);
}

/*
 * The selected values are compressed to the front of a buffer behind the previous value (VPCOMPRESSD), then every value is
 * compared with its predecessor in the buffer. Without a mask the rows are compared with themselves shifted by one.
 * The vector has at most SIFT_AGG_VEC rows.
 */
F5 int64
count_changes_avx512(const uint32 *v, const uint64 *mask, int n, uint32 *prev)
{
	uint32		buf[SIFT_AGG_VEC + 32];
	int64		cnt = 0;
	int			c = 0;

	if (!mask)
	{
		if (n == 0)
			return 0;
		cnt = v[0] != *prev;
		for (int i = 1; i < n; i += 16)
			cnt += __builtin_popcount(_mm512_cmpneq_epi32_mask(L5(v + i), L5(v + i - 1)) & AGK_LANES(n - i, 16));
		*prev = v[n - 1];
		return cnt;
	}
	buf[0] = *prev;
	for (int i = 0; i < n; i += 16)
	{
		const __mmask16 k = SEL16(mask, i);

		S5(buf + 1 + c, _mm512_maskz_compress_epi32(k, L5(v + i)));
		c += __builtin_popcount(k);
	}
	S5(buf + 1 + c, _mm512_setzero_si512());
	for (int j = 0; j < c; j += 16)
		cnt += __builtin_popcount(_mm512_cmpneq_epi32_mask(L5(buf + j), L5(buf + j + 1)) & AGK_LANES(c - j, 16));
	*prev = buf[c];
	return cnt;
}

#define AGK_INIT5(s)	.s = s##_avx512,

const SiftAggKernels sift_aggkern_avx512 = {
	AGK_INIT5(cmp_f64) AGK_INIT5(between_f64) AGK_INIT5(lut_i32) AGK_INIT5(gather_bits)
	AGK_INIT5(gid_code) AGK_INIT5(gid_i32)
	AGK_INIT5(sum_i32) AGK_INIT5(sum_f64) AGK_INIT5(absmax_f64) AGK_INIT5(minmax_i32) AGK_INIT5(minmax_i64) AGK_INIT5(minmax_f64)
	.count_gid = agk_count_gid_scalar,
	.sum_gid_i32 = agk_sum_gid_i32_scalar,
	.sum_gid_f64 = agk_sum_gid_f64_scalar,
	AGK_INIT5(count_changes)
};

#endif							/* __x86_64__ */
