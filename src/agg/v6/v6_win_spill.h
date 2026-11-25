/*
 * agg/v6/v6_win_spill.h - a partition of the window sort (v6_win_partsort.h) that is more than the memory it may hold, sorted by a tuplesort (it spills to files past its budget and is cleaned up
 * with the query, an error or a cancel as any sort of PostgreSQL). The rows go in as the cells of the input, in the order they came; they come back sorted by the order keys of the window and, on
 * a tie, by that order: what the sort in memory gives. keep > 0 asks for the first keep rows only, which the sort holds with a bounded heap.
 */
#ifndef SIFT_AGG_V6_WIN_SPILL_H
#define SIFT_AGG_V6_WIN_SPILL_H

#include "postgres.h"

#include "agg/v6/v6_win_partsort.h"

typedef struct SiftWinSpill SiftWinSpill;

/* The rows read back: cx holds what the cells point to, the arrays the caller made for one batch */
typedef struct SiftWinSpillOut
{
	MemoryContext cx;
	char	   *col[SIFT_V6_MAX_COLS];
	uint8	   *state[SIFT_V6_MAX_COLS];
	uint64	   *tag;
	int64	   *peer[SIFT_WIN_MAX_KEYS];
} SiftWinSpillOut;

/* The sort for a partition, of at most bytes of memory, in a context of parent */
extern SiftWinSpill *sift_win_spill_begin(MemoryContext parent, const SiftWinPsSpec *spec, uint64 bytes);
/* A row: the state (SIFT_CELL_STATE_*) of each column and where its cell is */
extern void sift_win_spill_put(SiftWinSpill *sp, uint64 tag, const uint8 *state, const char *const *cell);
extern void sift_win_spill_sort(SiftWinSpill *sp);
/* Up to m rows in order, 0 when they are all given; the changes of the order keys count from the first row */
extern uint16 sift_win_spill_read(SiftWinSpill *sp, uint16 m, SiftWinSpillOut *out);
extern void sift_win_spill_end(SiftWinSpill *sp);

#endif							/* SIFT_AGG_V6_WIN_SPILL_H */
