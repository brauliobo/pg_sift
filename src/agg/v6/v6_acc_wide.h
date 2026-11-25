/*
 * agg/v6/v6_acc_wide.h - the state of SUM and AVG of a derived numeric (V6A_SUM_WIDE of v6_acc.h), in the 32 bytes of every accumulator: the sum as a signed 192-bit integer in two's complement
 * in v0, v1 and aux (the top limb), the rows with a value in n. A NaN row makes the sum NaN for good: aux is then V6W_NAN, the one top limb a sum cannot have (a sum that reaches it raises).
 * Nothing points out of the state, so that the spill of the groups and the merge of the partitions of a parallel run copy and combine it as any other.
 */
#ifndef SIFT_AGG_V6_ACC_WIDE_H
#define SIFT_AGG_V6_ACC_WIDE_H

#include "core/host.h"

#include "agg/vec.h"
#include "agg/v6/v6_acc_type.h"

#define V6W_NAN		PG_INT64_MIN

extern _Noreturn void sift_v6_wide_overflow(void);

static inline bool
v6acc_wide_is_nan(const V6Acc *a)
{
	return a->aux == V6W_NAN;
}

static inline void
v6acc_wide_set_nan(V6Acc *a)
{
	a->aux = V6W_NAN;
}

/* a += the 192-bit integer (l2, l1, l0) */
static inline void
v6acc_wide_add(V6Acc *a, uint64 l0, uint64 l1, int64 l2)
{
	const unsigned __int128 lo = ((unsigned __int128) (uint64) a->v1 << 64) | a->v0;
	const unsigned __int128 sum = lo + (((unsigned __int128) l1 << 64) | l0);
	int64		top;

	if (v6acc_wide_is_nan(a))
		return;
	if (__builtin_add_overflow(a->aux, l2, &top) | __builtin_add_overflow(top, (int64) (sum < lo), &top) | (top == V6W_NAN))
		sift_v6_wide_overflow();
	a->v0 = (uint64) sum;
	a->v1 = (int64) (sum >> 64);
	a->aux = top;
}

static inline void
v6acc_wide_add_i64(V6Acc *a, int64 x)
{
	v6acc_wide_add(a, (uint64) x, (uint64) (x >> 63), x >> 63);
}

static inline void
v6acc_wide_add_i128(V6Acc *a, SiftI128 x)
{
	v6acc_wide_add(a, x.lo, (uint64) x.hi, x.hi >> 63);
}

static inline void
v6acc_wide_add_i256(V6Acc *a, const SiftI256 *x)
{
	v6acc_wide_add(a, x->l[0], x->l[1], (int64) x->l[2]);
}

static inline void
v6acc_wide_combine(V6Acc *d, const V6Acc *s)
{
	d->n += s->n;
	if (v6acc_wide_is_nan(s))
		v6acc_wide_set_nan(d);
	else
		v6acc_wide_add(d, s->v0, (uint64) s->v1, s->aux);
}

/* the sum fits the 128 bits of v0 and v1 */
static inline bool
v6acc_wide_fits_i128(const V6Acc *a)
{
	return a->aux == (a->v1 >> 63);
}

#endif							/* SIFT_AGG_V6_ACC_WIDE_H */
