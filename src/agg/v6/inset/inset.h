/*
 * agg/v6/inset/inset.h - `column = ANY (array)` of the integer class as a set tested per row: the values in ascending order, and the rows of a batch whose column is one of them.
 */
#ifndef SIFT_AGG_V6_INSET_H
#define SIFT_AGG_V6_INSET_H

#include "postgres.h"

#include "agg/vec.h"

#define SIFT_V6_INSET_PLAIN	16		/* the values searched as they are: a set of more has an index */

typedef struct SiftV6InSet
{
	int			slot;			/* the column of the input */
	int			n;				/* the distinct values */
	const int64 *vals;			/* ... ascending */
	int64		vmin;			/* vals[0] and vals[n - 1] (n = 0: an empty range) */
	int64		vmax;
	/* a set of more than SIFT_V6_INSET_PLAIN values: a blocked bloom filter of at most 16 KB, the cache of a core, in front of a table of the values */
	uint64	   *bloom;			/* 2 bits of one word a value */
	uint32		bloom_mask;		/* the words - 1 */
	int64	   *table;			/* open-addressed, a half full at most; SIFT_V6_INSET_EMPTY marks a free slot */
	uint32		table_mask;
	bool		has_empty;		/* the value that marks a free slot is in the set */
} SiftV6InSet;

#define SIFT_V6_INSET_EMPTY	PG_INT64_MIN

/* the index of a set of vals[0 .. n) (ascending, distinct) in the memory context keep; a set whose table would not fit work_mem has none, and its values are searched */
extern void sift_v6_inset_index(MemoryContext keep, SiftV6InSet *s);

/* The rows of out (a bitmap over the n rows of col) whose column is in the set stay; left: the rows of out on entry; the rows left on return */
extern uint16 sift_v6_inset_filter(const SiftV6InSet *s, const SiftVec *col, uint16 n, uint64 *out, uint16 left);

#endif							/* SIFT_AGG_V6_INSET_H */
