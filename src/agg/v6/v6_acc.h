/*
 * agg/v6/v6_acc.h - the accumulator of an aggregate of SiftAggV6 (32 bytes: n, v0, v1, aux), how two partial states of it combine, and the base-10000 digits of a scaled 128-bit sum.
 *
 * The combine is exact and associative for every kind: counts add, integer sums add as 128-bit integers, a minimum or maximum keeps the better (a float with PostgreSQL's order: every NaN above
 * +Infinity), the argument of arg_max/arg_min keeps the better key, and the count of a COUNT(DISTINCT) is a piece of the count (the groups are disjoint across partitions) that adds. group.c folds
 * the rows of a batch into these states and the partitions of a parallel run combine them with sift_v6_acc_combine: one function for both, so a parallel answer is the serial one.
 */
#ifndef SIFT_AGG_V6_ACC_H
#define SIFT_AGG_V6_ACC_H

#include <math.h>

#include "core/host.h"

#include "utils/float.h"

#include "agg/group/state.h"
#include "agg/v6/v6_argext.h"
#include "agg/v6/v6_acc_type.h"
#include "agg/v6/v6_fsum.h"


/* How an aggregate combines: the operation is chosen once per aggregate of the statement, from its kind and the class of its argument */
typedef enum V6AccOp
{
	V6A_NONE = 0,				/* GROUPING: no state */
	V6A_COUNT,					/* count(*), count(x): n adds */
	V6A_COUNT_PIECE,			/* count(DISTINCT x) of disjoint groups: v0 adds */
	V6A_SUM_INT,				/* sum, avg of an integer or a scaled numeric: the 128-bit sum in v0, v1 and the count in n */
	V6A_SUM_F64,				/* sum, avg of a float: the three bins of v6_fsum.h */
	V6A_SUM_F64_ROW,			/* sift.agg_exact_float: the running double sum, in the order of the rows */
	V6A_MIN_INT,
	V6A_MAX_INT,
	V6A_MIN_F64,
	V6A_MAX_F64,
	V6A_ARG_MAX,
	V6A_ARG_MIN
} V6AccOp;

static inline double
v6acc_f64(const V6Acc *a)
{
	double		d;

	memcpy(&d, &a->v0, sizeof(d));
	return d;
}

static inline void
v6acc_set_f64(V6Acc *a, double d)
{
	memcpy(&a->v0, &d, sizeof(d));
}

static inline SiftI128
v6acc_i128(const V6Acc *a)
{
	return (SiftI128) {a->v0, a->v1};
}

static inline void
v6acc_set_i128(V6Acc *a, SiftI128 s)
{
	a->v0 = s.lo;
	a->v1 = s.hi;
}

/* float8 order: every NaN equals every NaN and is above every number */
static inline int
v6acc_f8_cmp(double a, double b)
{
	if (isnan(a))
		return isnan(b) ? 0 : 1;
	return isnan(b) ? -1 : (a < b ? -1 : a > b);
}

static inline void
sift_v6_acc_combine(V6AccOp op, V6Acc *d, const V6Acc *s)
{
	switch (op)
	{
		case V6A_COUNT:
			d->n += s->n;
			d->aux += s->aux;
			break;
		case V6A_COUNT_PIECE:
			d->v0 += s->v0;
			d->aux |= s->aux;
			break;
		case V6A_SUM_F64:
			v6fsum_combine(d, s);
			break;
		case V6A_SUM_F64_ROW:
			v6acc_set_f64(d, float8_pl(v6acc_f64(d), v6acc_f64(s)));
			d->n += s->n;
			d->aux += s->aux;
			break;
		case V6A_SUM_INT:
			{
				SiftI128	x = v6acc_i128(d);

				sift_i128_add(&x, v6acc_i128(s));
				v6acc_set_i128(d, x);
				d->n += s->n;
				d->aux += s->aux;
			}
			break;
		case V6A_MIN_INT:
		case V6A_MAX_INT:
			d->aux += s->aux;
			if (s->n == 0)
				break;
			if (d->n == 0 || (op == V6A_MIN_INT ? (int64) s->v0 < (int64) d->v0 : (int64) s->v0 > (int64) d->v0))
				d->v0 = s->v0;
			d->n += s->n;
			break;
		case V6A_MIN_F64:
		case V6A_MAX_F64:
			d->aux += s->aux;
			if (s->n == 0)
				break;
			if (d->n == 0 || (op == V6A_MIN_F64 ? v6acc_f8_cmp(v6acc_f64(s), v6acc_f64(d)) < 0 : v6acc_f8_cmp(v6acc_f64(s), v6acc_f64(d)) > 0))
				d->v0 = s->v0;
			d->n += s->n;
			break;
		case V6A_ARG_MAX:
		case V6A_ARG_MIN:
			sift_argacc_merge(&d->n, &d->v0, &d->v1, &d->aux, op == V6A_ARG_MAX ? SIFT_ARG_MAX : SIFT_ARG_MIN, s->n, s->v0, s->v1, s->aux);
			break;
		case V6A_NONE:
			break;
	}
}

/*
 * A scaled 128-bit integer (v * 10^-scale) as the base-10000 digits PostgreSQL's numeric holds, most significant first, in the wire format numeric_recv reads: weight 0 is the units group, the
 * fraction (scale places) follows in groups of four and the last of them is padded with zeros. One division of the 128-bit value by 10000 for each group, the rest is on 64-bit words; no
 * decimal string is made and parsed. Returns the number of digits (0: zero) or -1 when the value scaled to a whole group of places does not fit 128 bits (the caller formats it the slow way).
 */
#define V6_NUMERIC_MAX_DIGITS	12
#define V6_NUMERIC_SIGN_NEG		0x4000	/* the sign word of the wire format of a negative numeric */

static inline int
sift_v6_i128_digits(SiftI128 v, int scale, bool *neg, int16 *digits, int *weight)
{
	static const unsigned __int128 pow10[] = {1, 10, 100, 1000};
	unsigned __int128 u = ((unsigned __int128) (uint64) v.hi << 64) | v.lo;
	const int	pad = (4 - scale % 4) % 4,
				frac = (scale + pad) / 4;
	uint16		raw[V6_NUMERIC_MAX_DIGITS + 1];
	int			n = 0,
				last = 0;

	*neg = v.hi < 0;
	if (*neg)
		u = -u;
	if (u > ~(unsigned __int128) 0 / pow10[pad])
		return -1;
	u *= pow10[pad];
	for (; u != 0; n++)
	{
		const unsigned __int128 q = u / 10000;

		raw[n] = (uint16) (u - q * 10000);
		u = q;
	}
	*weight = 0;
	if (n == 0)
		return 0;
	while (raw[last] == 0)
		last++;
	*weight = n - 1 - frac;
	for (int i = n - 1; i >= last; i--)
		*digits++ = (int16) raw[i];
	return n - last;
}

#endif							/* SIFT_AGG_V6_ACC_H */
