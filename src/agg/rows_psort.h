/*
 * agg/rows_psort.h - the external sort of the (group, value, bits) pairs of a count(DISTINCT) of agg/rows.c, on the pairs themselves: a run is sorted by a radix sort of the 12 key bytes with
 * the pairs of equal (group, value) merged on the way (their bits OR-ed), written to a temporary file as 16 byte records, and the runs are merged by a tournament of at most 64 of them at a
 * time, so that a pair costs no tuple, no slot and no comparison function call.
 */
#ifndef SIFT_AGG_ROWS_PSORT_H
#define SIFT_AGG_ROWS_PSORT_H

#include "postgres.h"

/* a pair of a count(DISTINCT) set: bit b of mask is set when the b-th aggregate of the set took a row of the group with the value */
typedef struct RowsPair
{
	int64		v;
	uint32		g;
	uint32		mask;			/* 0: the slot is free */
} RowsPair;

typedef struct RowsPSort RowsPSort;

/* A sort that holds at most mem_bytes of pairs in memory (the run being filled and the buffer its radix sort needs) */
extern RowsPSort *rows_psort_begin(MemoryContext cx, uint64 mem_bytes);
extern void rows_psort_put(RowsPSort *s, uint32 g, int64 v, uint32 mask);
/* After the last put: the pairs can be taken */
extern void rows_psort_finish(RowsPSort *s);
/* The next pair in the order of (g, v), the bits of all the pairs of the same (g, v) in it; false at the end */
extern bool rows_psort_next(RowsPSort *s, RowsPair *out);
extern void rows_psort_end(RowsPSort *s);

/* The pairs a[0 .. n-1] in the order of (g, v), those of equal (g, v) one with their bits OR-ed; scratch has room for n pairs. Returns the pairs left. Exported for the tests */
extern uint64 rows_psort_sort_pairs(RowsPair *a, RowsPair *scratch, uint64 n);

#endif							/* SIFT_AGG_ROWS_PSORT_H */
