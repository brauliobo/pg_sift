/*
 * agg/v6/fsum.h - the float8 sum of SiftAggV6 that gives the same bits for any order of the rows and any split of them between processes.
 *
 * A float is split along a fixed grid of 26-bit bins whose edges are bit positions of the number line (bit P of the grid is 2^(P - 1074)). The state keeps the three top bins, from the bin of the largest
 * number met (its anchor) down, each as the plain integer sum of the bin's digits of every number: the digits of a number do not depend on the other numbers, an integer sum is exact in any order, and a
 * larger anchor drops the whole lowest bins (what a number has below the three bins is not kept, in any order). The three bins hold the 53 bits of a number whose top bit is in the anchor bin, so the sum of
 * one number is that number. The columns have 37 bits of room above a digit: a group may hold 2^37 numbers.
 *
 * The state is a V6Acc whose columns are v0, v1 and aux (the anchor bin, then the two below). n holds the count of rows in its low 48 bits, the anchor bin + 1 above them (0: no number but zeros yet) and
 * the flags of the numbers that are not finite. A NaN, or an infinity of each sign, is a flag: the sum is NaN when a NaN or both infinities were met, else that infinity.
 */
#ifndef SIFT_AGG_V6_FSUM_H
#define SIFT_AGG_V6_FSUM_H

#include <math.h>

#include "utils/float.h"

#include "agg/v6/acc_type.h"

#define V6FSUM_BITS			26
#define V6FSUM_MASK			((UINT64CONST(1) << V6FSUM_BITS) - 1)
#define V6FSUM_COUNT_BITS	48
#define V6FSUM_COUNT_MASK	((UINT64CONST(1) << V6FSUM_COUNT_BITS) - 1)
#define V6FSUM_TOP_SHIFT	48
#define V6FSUM_FLAG_SHIFT	56
#define V6FSUM_NAN			1
#define V6FSUM_PINF			2
#define V6FSUM_NINF			4
#define V6FSUM_MAX_ROWS		137438953472	/* 2^37 */

static inline int64
v6fsum_count(const V6Acc *a)
{
	return (int64) ((uint64) a->n & V6FSUM_COUNT_MASK);
}

/* the anchor bin, -1 when no number but zeros was folded */
static inline int
v6fsum_top(const V6Acc *a)
{
	return (int) (((uint64) a->n >> V6FSUM_TOP_SHIFT) & 0xff) - 1;
}

static inline unsigned
v6fsum_flags(const V6Acc *a)
{
	return (unsigned) ((uint64) a->n >> V6FSUM_FLAG_SHIFT);
}

static inline void
v6fsum_set_top(V6Acc *a, int top)
{
	a->n = (int64) (((uint64) a->n & ~((UINT64CONST(0xff)) << V6FSUM_TOP_SHIFT)) | ((uint64) (top + 1) << V6FSUM_TOP_SHIFT));
}

static inline void
v6fsum_cols(const V6Acc *a, int64 c[3])
{
	c[0] = (int64) a->v0;
	c[1] = a->v1;
	c[2] = a->aux;
}

static inline void
v6fsum_set_cols(V6Acc *a, const int64 c[3])
{
	a->v0 = (uint64) c[0];
	a->v1 = c[1];
	a->aux = c[2];
}

/* the columns of a state anchored delta bins lower, as they are under the higher anchor */
static inline void
v6fsum_shift(int64 c[3], int delta)
{
	for (int k = 2; k >= 0; k--)
		c[k] = k >= delta ? c[k - delta] : 0;
}

static inline void
v6fsum_raise(V6Acc *a, int top)
{
	const int	cur = v6fsum_top(a);

	if (cur >= 0)
	{
		int64		c[3];

		v6fsum_cols(a, c);
		v6fsum_shift(c, top - cur);
		v6fsum_set_cols(a, c);
	}
	v6fsum_set_top(a, top);
}

/* one finite number that is not zero: its sign, mantissa and the grid position of the mantissa's lowest bit */
static inline void
v6fsum_deposit(V6Acc *a, double x)
{
	uint64		u;
	uint64		m;
	int			e;
	int			pos;
	int			top;
	int64		c[3];
	int			s;
	unsigned __int128 t;

	memcpy(&u, &x, sizeof(u));
	e = (int) ((u >> 52) & 0x7ff);
	m = (u & ((UINT64CONST(1) << 52) - 1)) | (e != 0 ? UINT64CONST(1) << 52 : 0);
	pos = e != 0 ? e - 1 : 0;
	top = (pos + 52) / V6FSUM_BITS;
	if (top > v6fsum_top(a))
		v6fsum_raise(a, top);
	s = pos - V6FSUM_BITS * (v6fsum_top(a) - 2);
	t = s >= 0 ? (unsigned __int128) m << s : (-s < 64 ? (unsigned __int128) (m >> -s) : 0);
	v6fsum_cols(a, c);
	if ((u >> 63) != 0)
	{
		c[0] -= (int64) (t >> (2 * V6FSUM_BITS));
		c[1] -= (int64) ((t >> V6FSUM_BITS) & V6FSUM_MASK);
		c[2] -= (int64) (t & V6FSUM_MASK);
	}
	else
	{
		c[0] += (int64) (t >> (2 * V6FSUM_BITS));
		c[1] += (int64) ((t >> V6FSUM_BITS) & V6FSUM_MASK);
		c[2] += (int64) (t & V6FSUM_MASK);
	}
	v6fsum_set_cols(a, c);
}

/* a float of any kind into the bins and flags; the caller counts the row */
static inline void
v6fsum_add(V6Acc *a, double x)
{
	if (isnan(x))
		a->n |= (int64) V6FSUM_NAN << V6FSUM_FLAG_SHIFT;
	else if (isinf(x))
		a->n |= (int64) (x > 0 ? V6FSUM_PINF : V6FSUM_NINF) << V6FSUM_FLAG_SHIFT;
	else if (x != 0)
		v6fsum_deposit(a, x);
}

/* a state of the same group folded into d: the sum of the columns under the higher anchor */
static inline void
v6fsum_combine(V6Acc *d, const V6Acc *s)
{
	const int	stop = v6fsum_top(s);
	const int64 n = v6fsum_count(d) + v6fsum_count(s);
	const uint64 flags = (uint64) (v6fsum_flags(d) | v6fsum_flags(s)) << V6FSUM_FLAG_SHIFT;

	if (stop >= 0)
	{
		int64		sc[3];
		int64		dc[3];

		if (stop > v6fsum_top(d))
			v6fsum_raise(d, stop);
		v6fsum_cols(s, sc);
		v6fsum_shift(sc, v6fsum_top(d) - stop);
		v6fsum_cols(d, dc);
		for (int k = 0; k < 3; k++)
			dc[k] += sc[k];
		v6fsum_set_cols(d, dc);
	}
	d->n = (int64) (((uint64) d->n & ~(V6FSUM_COUNT_MASK | (UINT64CONST(7) << V6FSUM_FLAG_SHIFT))) | (uint64) n | flags);
}

/* a plain double sum of count numbers (a device's partial result) as a state */
static inline V6Acc
v6fsum_of_double(double sum, int64 count)
{
	V6Acc		a = {.n = count};

	v6fsum_add(&a, sum);
	return a;
}

/* the sum as the double nearest to what the three bins hold */
static inline double
v6fsum_value(const V6Acc *a)
{
	const unsigned flags = v6fsum_flags(a);
	int64		c[3];
	__int128	t;
	double		sum;

	if ((flags & V6FSUM_NAN) || (flags & (V6FSUM_PINF | V6FSUM_NINF)) == (V6FSUM_PINF | V6FSUM_NINF))
		return NAN;
	if (flags != 0)
		return (flags & V6FSUM_PINF) ? INFINITY : -INFINITY;
	if (v6fsum_top(a) < 0)
		return 0.0;
	v6fsum_cols(a, c);
	t = ((__int128) c[0] << (2 * V6FSUM_BITS)) + ((__int128) c[1] << V6FSUM_BITS) + c[2];
	sum = ldexp((double) t, V6FSUM_BITS * (v6fsum_top(a) - 2) - 1074);
	if (isinf(sum))
		float_overflow_error();
	return sum;
}

#endif							/* SIFT_AGG_V6_FSUM_H */
