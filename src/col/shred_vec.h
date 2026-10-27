/*
 * col/shred_vec.h - the shredder kernels that select rows by their state byte (state_bits, minmax_i64, minmax_f64), written once with the GCC/Clang vector
 * extensions and compiled by each level file at its width (SIFT_V_BYTES of core/simd_isa.h) under the level's target options. Included by
 * col/shred_kern_avx2.c and col/shred_kern_neon.c after they define SIFT_VEC_<level>, SV_FN(x) (the kernels' names) and sv_words(p, value, ovf): the 64 bits of the states at p
 * that equal VALUE and OVERFLOW (the one operation of the level that is a movemask).
 */
#include "core/simd_isa.h"

#define SV_L	(SIFT_V_BYTES / 8)

/* a level whose kernels the table of shred_kern.c names from another file defines SV_STORAGE empty */
#ifndef SV_STORAGE
#define SV_STORAGE static
#endif

typedef uint8 sv_b __attribute__((vector_size(SV_L), aligned(1), may_alias));
typedef uint64 sv_u __attribute__((vector_size(SIFT_V_BYTES), aligned(8), may_alias));
typedef int64 sv_s __attribute__((vector_size(SIFT_V_BYTES), aligned(8), may_alias));
typedef double sv_d __attribute__((vector_size(SIFT_V_BYTES), aligned(8), may_alias));

/* the lanes of the SV_L states at p that are VALUE, as all ones */
#define SV_VALUE_LANES(p)	((sv_s) (__builtin_convertvector(*(const sv_b *) (p), sv_u) == (sv_u) {} + SIFT_SHRED_VALUE))

/* the shredder never produces a NaN or a -0.0, so a minimum is a compare and a select, or the instruction of the level */
#ifndef SV_MIN_F64
#ifdef SIFT_V_MIN_F64
#define SV_MIN_F64(a, b)	SIFT_V_MIN_F64(a, b)
#define SV_MAX_F64(a, b)	SIFT_V_MAX_F64(a, b)
#else
#define SV_MIN_F64(a, b)	SIFT_V_BLEND(sv_d, (b) < (a), b, a)
#define SV_MAX_F64(a, b)	SIFT_V_BLEND(sv_d, (b) > (a), b, a)
#endif
#endif

SV_STORAGE void
SV_FN(state_bits)(const uint8 *state, uint32 n, uint64 *nulls, uint64 *ovf)
{
	uint32		i = 0;

	for (; i + 64 <= n; i += 64)
	{
		uint64		value;

		sv_words(state + i, &value, &ovf[i / 64]);
		nulls[i / 64] = ~value;
	}
	if (i < n)
	{
		uint64		nw = 0,
					ow = 0;

		for (uint32 j = 0; i + j < n; j++)
		{
			nw |= (uint64) (state[i + j] != SIFT_SHRED_VALUE) << j;
			ow |= (uint64) (state[i + j] == SIFT_SHRED_OVERFLOW) << j;
		}
		nulls[i / 64] = nw;
		ovf[i / 64] = ow;
	}
}

SV_STORAGE bool
SV_FN(minmax_i64)(const uint8 *state, const int64 *val, uint32 n, int64 *min, int64 *max)
{
	const sv_s	imax = (sv_s) {} + PG_INT64_MAX,
				imin = (sv_s) {} + PG_INT64_MIN;
	sv_s		mn = imax,
				mx = imin,
				any = {};
	uint32		i = 0;
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;
	bool		found;

	for (; i + SV_L <= n; i += SV_L)
	{
		const sv_s	m = SV_VALUE_LANES(state + i),
					v = *(const sv_s *) (val + i),
					a = SIFT_V_BLEND(sv_s, m, v, imax),
					b = SIFT_V_BLEND(sv_s, m, v, imin);

		mn = SIFT_V_BLEND(sv_s, a < mn, a, mn);
		mx = SIFT_V_BLEND(sv_s, b > mx, b, mx);
		any |= m;
	}
	found = SIFT_V_ANY(any);
	for (int k = 0; k < SV_L; k++)
	{
		lo = Min(lo, mn[k]);
		hi = Max(hi, mx[k]);
	}
	for (; i < n; i++)
	{
		if (state[i] != SIFT_SHRED_VALUE)
			continue;
		lo = Min(lo, val[i]);
		hi = Max(hi, val[i]);
		found = true;
	}
	if (found)
	{
		*min = lo;
		*max = hi;
	}
	return found;
}

#ifndef SV_NO_MINMAX_F64
SV_STORAGE bool
SV_FN(minmax_f64)(const uint8 *state, const double *val, uint32 n, double *min, double *max)
{
	const sv_d	pinf = (sv_d) {} + __builtin_inf(),
				ninf = (sv_d) {} - __builtin_inf();
	sv_d		mn = pinf,
				mx = ninf;
	sv_s		any = {};
	uint32		i = 0;
	double		lo = __builtin_inf(),
				hi = -__builtin_inf();
	bool		found;

	for (; i + SV_L <= n; i += SV_L)
	{
		const sv_s	m = SV_VALUE_LANES(state + i);
		const sv_d	v = *(const sv_d *) (val + i);

		mn = SV_MIN_F64(mn, SIFT_V_BLEND(sv_d, m, v, pinf));
		/* this operand order is the one GCC keeps as VMAXPD (the other becomes a compare and a blend); the shredder has no NaN or -0.0, so either is exact */
		mx = SV_MAX_F64(SIFT_V_BLEND(sv_d, m, v, ninf), mx);
		any |= m;
	}
	found = SIFT_V_ANY(any);
	for (int k = 0; k < SV_L; k++)
	{
		lo = mn[k] < lo ? mn[k] : lo;
		hi = mx[k] > hi ? mx[k] : hi;
	}
	for (; i < n; i++)
	{
		if (state[i] != SIFT_SHRED_VALUE)
			continue;
		lo = val[i] < lo ? val[i] : lo;
		hi = val[i] > hi ? val[i] : hi;
		found = true;
	}
	if (found)
	{
		*min = lo;
		*max = hi;
	}
	return found;
}
#endif
