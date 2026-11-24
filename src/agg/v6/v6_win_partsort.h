/*
 * agg/v6/v6_win_partsort.h - a source that sorts inside each partition, for the window node (v6_window.h) over parts that are sorted by the PARTITION BY keys but not by the window order.
 *
 * The source it wraps gives the rows with the rows of one partition together (parts sorted by the partition keys, in any direction). One partition at a time is held, sorted by the window
 * order (each key ascending or descending, NULLs first or last, as the query wrote it) and given back in batches. What the engine of agg/window.h and SiftWinx need of the keys is only when
 * they change, so the batches carry them as integers: column ncols is the number of the partition (one for every key of PARTITION BY, whatever their types, a string among them) and column
 * ncols + 1 + j counts the changes of the value of order key j in the sorted partition. Every column of the input is given as it was, so the arguments of the functions and the tags
 * (what the source fetches a row by) are the input's. A partition that does not fit max_bytes is sorted by v6_win_spill.h, which spills to files.
 *
 * keep: the engine wants no more than the first keep rows of each partition (ROW_NUMBER <= K): the rows past the best keep are dropped as the partition is read, so the partition costs
 * O(rows log keep) and keep rows of memory. 0 keeps every row.
 */
#ifndef SIFT_AGG_V6_WIN_PARTSORT_H
#define SIFT_AGG_V6_WIN_PARTSORT_H

#include "agg/window.h"
#include "agg/v6/v6_vexpr.h"

typedef struct SiftWinPsKey
{
	int			slot;			/* the column of the input */
	bool		desc;
	bool		nulls_first;
} SiftWinPsKey;

typedef struct SiftWinPsSpec
{
	int			ncols;
	SiftVecType type[SIFT_V6_MAX_COLS];
	int16		scale[SIFT_V6_MAX_COLS];
	int			n_part;			/* 0: the whole input is one partition */
	int			part_slot[SIFT_WIN_MAX_KEYS];
	int			n_ord;
	SiftWinPsKey ord[SIFT_WIN_MAX_KEYS];
	uint64		max_bytes;
	uint64		keep;
} SiftWinPsSpec;

/* the column of the partition number in the batches, and the column of the changes of order key j */
static inline int
sift_win_ps_part_slot(const SiftWinPsSpec *s)
{
	return s->ncols;
}

static inline int
sift_win_ps_peer_slot(const SiftWinPsSpec *s, int j)
{
	return s->ncols + 1 + j;
}

/* the bytes of a cell of a column of a type */
extern size_t sift_win_ps_elem_size(SiftVecType t);

/* the types a column may have to be held: false for others (the planner leaves the window to PostgreSQL) */
extern bool sift_win_ps_type_ok(SiftVecType t);

/* The source over in (closed with it); the batches live until the next call */
extern SiftV6Source *sift_win_partsort_open(MemoryContext cx, SiftV6Source *in, const SiftWinPsSpec *spec);

#endif							/* SIFT_AGG_V6_WIN_PARTSORT_H */
