/*
 * agg/v6/group/nan.h - what the accumulator of an aggregate keeps of the NaN rows of a numeric column, apart from the rows that have a value: V6Acc.aux counts them (COUNT DISTINCT: 1 when there
 * is one). A NaN is not NULL: COUNT counts it, a SUM or an AVG that meets one is NaN, a MAX is NaN, a MIN is NaN when no other row has a value, and COUNT DISTINCT counts it as one more value.
 */
#ifndef SIFT_AGG_V6_GROUP_NAN_H
#define SIFT_AGG_V6_GROUP_NAN_H

#include "agg/v6/group/int.h"
#include "agg/v6/batch.h"
#include "core/popcount.h"

static inline bool
v6acc_nan_row(const SiftVec *v, uint16 r)
{
	return sift_vec_nan_at(v, r);
}

/* the NaN rows among those a bitmap of n_raw rows takes */
static inline uint64
v6acc_nan_rows(const SiftVec *v, const uint64 *take, uint16 n_raw)
{
	uint64		n = 0;

	for (int w = 0; w < (n_raw + 63) / 64; w++)
		n += sift_pc64(take[w] & v->nan[w]);
	return n;
}

static inline void
v6acc_merge_nan(const SiftV6Agg *a, V6Acc *d, const V6Acc *s)
{
	if (a->kind == SV6_COUNT_DISTINCT)
		d->aux |= s->aux;
	else
		d->aux += s->aux;
}

/* the count COUNT and COUNT DISTINCT leave: the rows with a value, or the distinct ones, and the NaN rows */
static inline int64
v6acc_count(const SiftV6Agg *a, const V6Acc *acc)
{
	if (a->kind == SV6_COUNT_DISTINCT)
		return (int64) acc->v0 + (acc->aux != 0);
	return acc->n + acc->aux;
}

/* is the result of SUM, AVG, MAX or MIN the NaN: a NaN row decides all but a minimum that another row's value is below */
static inline bool
v6acc_is_nan(const SiftV6Agg *a, const V6Acc *acc)
{
	return acc->aux != 0 && (a->kind != SV6_MIN || acc->n == 0);
}

#endif							/* SIFT_AGG_V6_GROUP_NAN_H */
