/*
 * agg/tup_int.h - the tuples of a chunk that reach the aggregates (agg/update.c)
 */
#ifndef SIFT_AGG_TUP_INT_H
#define SIFT_AGG_TUP_INT_H

#include "agg/agg_fwd_int.h"

/* the tuples of a chunk that reach the aggregates: positions in the chunk, element and row ordinals, and the output row of every join combo */
typedef struct Tup
{
	int			n;
	uint32	   *sel;
	uint32	   *tidx;
	uint32	   *ridx;
	uint32	   *cout[SIFT_AGG_MAX_COMBOS];
} Tup;

#endif
