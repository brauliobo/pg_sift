/*
 * agg/v6/acc_type.h - the accumulator of an aggregate of SiftAggV6 (32 bytes: n, v0, v1, aux)
 */
#ifndef SIFT_AGG_V6_ACC_TYPE_H
#define SIFT_AGG_V6_ACC_TYPE_H

#include "core/host.h"

typedef struct V6Acc
{
	int64		n;				/* the values folded: a count, or 0 for an empty state */
	uint64		v0;				/* the low half of an integer sum, a double sum, a minimum or maximum; COUNT(DISTINCT): the distinct values counted so far */
	int64		v1;				/* the high half of an integer sum */
	int64		aux;			/* sift.arg_max, sift.arg_min: 1 when the best val is NULL; COUNT, SUM, AVG, MIN, MAX of a numeric: the rows whose value is NaN (COUNT DISTINCT: 1 when there is one) */
} V6Acc;

StaticAssertDecl(sizeof(V6Acc) == 32, "an accumulator is 32 bytes");

#endif							/* SIFT_AGG_V6_ACC_TYPE_H */
