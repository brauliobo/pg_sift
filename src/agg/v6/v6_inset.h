/*
 * agg/v6/v6_inset.h - `column = ANY (array)` of the integer class as a set tested per row: the values in ascending order, and the rows of a batch whose column is one of them.
 */
#ifndef SIFT_AGG_V6_INSET_H
#define SIFT_AGG_V6_INSET_H

#include "postgres.h"

#include "agg/vec.h"

typedef struct SiftV6InSet
{
	int			slot;			/* the column of the input */
	int			n;
	const int64 *vals;
} SiftV6InSet;

/* The rows of out (a bitmap over the n rows of col) whose column is in the set stay; left: the rows of out on entry; the rows left on return */
extern uint16 sift_v6_inset_filter(const SiftV6InSet *s, const SiftVec *col, uint16 n, uint64 *out, uint16 left);

#endif							/* SIFT_AGG_V6_INSET_H */
