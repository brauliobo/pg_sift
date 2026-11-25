/*
 * agg/ht/state.h - the aggregate states a group keeps in the state block of its table (agg/ht/ht.h) when they do not fit
 * the 32 bytes of the slot: the exact sum of a wide integer or numeric with its count (sum, avg), the minimum and maximum of a
 * 128-bit value, and the running variance of a float8 (stddev, variance). Every state is a multiple of 16 bytes, starts zeroed (an
 * empty state: n == 0) and is merged by the same functions that build it, in any order; the variance is exact in the order of
 * the merge only up to float rounding, like every float8 sum (sift.agg_exact_float).
 */
#ifndef SIFT_AGG_GROUP_STATE_H
#define SIFT_AGG_GROUP_STATE_H

#include "agg/vec.h"

static inline void
sift_i128_add(SiftI128 *a, SiftI128 b)
{
	const uint64 lo = a->lo + b.lo;

	a->hi = (int64) ((uint64) a->hi + (uint64) b.hi + (lo < a->lo));
	a->lo = lo;
}

static inline SiftI128
sift_i128_of_i64(int64 v)
{
	SiftI128	r = {(uint64) v, v < 0 ? -1 : 0};

	return r;
}

static inline bool
sift_i128_lt(SiftI128 a, SiftI128 b)
{
	return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

typedef struct SiftStateSum128
{
	SiftI128	sum;
	int64		n;
	int64		pad;
} SiftStateSum128;

static inline void
sift_state_sum128_add(SiftStateSum128 *s, SiftI128 v)
{
	sift_i128_add(&s->sum, v);
	s->n++;
}

static inline void
sift_state_sum128_merge(SiftStateSum128 *dst, const SiftStateSum128 *src)
{
	sift_i128_add(&dst->sum, src->sum);
	dst->n += src->n;
}

typedef struct SiftStateMinMax128
{
	SiftI128	min;
	SiftI128	max;
	int64		n;
	int64		pad;
} SiftStateMinMax128;

static inline void
sift_state_minmax128_add(SiftStateMinMax128 *s, SiftI128 v)
{
	if (s->n == 0 || sift_i128_lt(v, s->min))
		s->min = v;
	if (s->n == 0 || sift_i128_lt(s->max, v))
		s->max = v;
	s->n++;
}

static inline void
sift_state_minmax128_merge(SiftStateMinMax128 *dst, const SiftStateMinMax128 *src)
{
	if (src->n == 0)
		return;
	if (dst->n == 0 || sift_i128_lt(src->min, dst->min))
		dst->min = src->min;
	if (dst->n == 0 || sift_i128_lt(dst->max, src->max))
		dst->max = src->max;
	dst->n += src->n;
}

/* n values, their mean and m2 = the sum of the squares of their distances to the mean (Welford, merged as Chan et al.) */
typedef struct SiftStateVar
{
	int64		n;
	double		mean;
	double		m2;
	int64		pad;
} SiftStateVar;

static inline void
sift_state_var_add(SiftStateVar *s, double x)
{
	const double d = x - s->mean;

	s->n++;
	s->mean += d / (double) s->n;
	s->m2 += d * (x - s->mean);
}

static inline void
sift_state_var_merge(SiftStateVar *dst, const SiftStateVar *src)
{
	const int64 n = dst->n + src->n;
	const double d = src->mean - dst->mean;

	if (src->n == 0)
		return;
	if (dst->n == 0)
	{
		*dst = *src;
		return;
	}
	dst->mean += d * (double) src->n / (double) n;
	dst->m2 += src->m2 + d * d * (double) dst->n * (double) src->n / (double) n;
	dst->n = n;
}

#endif							/* SIFT_AGG_GROUP_STATE_H */
