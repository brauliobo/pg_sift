/*
 * agg/par/winpart.h - window functions over whole partitions on the compute threads (core/par/pool.h), in pure C: no palloc, no ereport, and no memory of its own.
 *
 * An item is the rows of whole partitions, in the order the partitions arrived (agg/win/partsort.h sorts one at a time; a thread sorts the partitions of an item). The leader makes the arrays
 * of the input and of the results (the thread writes only the results and the scratch) and the thread gives each partition what agg/window.c gives it: the rows sorted by the window order (each key
 * ascending or descending, NULLs first or last, a float8 with NaN above everything; rows that tie keep their arrival order), the functions of agg/window.h with the frame PostgreSQL gives them, row by row
 * (ROWS) or by peer group (RANGE), and the same values: the sum of integers in 128 bits, the sum of a float8 in the order of the sorted rows with the overflow check of float8pl, the average of an int4 as
 * the exact quotient of numeric_div. Results are in sorted order: row o of the item is the o-th row of the sorted partitions, with its tag.
 *
 * The one thing a thread cannot do is raise: a float8 sum that overflows is SIFT_ERR_SERIAL, and the leader raises what float8pl raises.
 */
#ifndef SIFT_AGG_V6_PAR_WINPART_H
#define SIFT_AGG_V6_PAR_WINPART_H

#include "postgres.h"

#include "agg/window.h"
#include "core/par/arena.h"

typedef struct SiftParWinKey
{
	bool		desc;
	bool		nulls_first;
	bool		f64;			/* the values are the bits of a float8, else int64 */
} SiftParWinKey;

typedef struct SiftParWinSpec
{
	int			n_ord;
	SiftParWinKey ord[SIFT_WIN_MAX_KEYS];
	int			nfuncs;
	SiftWinFuncSpec func[SIFT_WIN_MAX_FUNCS];
} SiftParWinSpec;

/* The kinds and types of the arguments this file computes: false for others, which stay with agg/window.c */
extern bool sift_par_win_func_ok(const SiftWinFuncSpec *fn);

typedef struct SiftParWinItem
{
	uint32		n;				/* the rows */
	uint32		nparts;
	const uint32 *pstart;		/* nparts + 1: the first row of each partition, then n */
	const uint64 *tag;
	const int64 *ord[SIFT_WIN_MAX_KEYS];	/* the value of the order key of each row: an int32 sign-extended, an int64, the bits of a float8 */
	const uint8 *ord_null[SIFT_WIN_MAX_KEYS];	/* 1: NULL */
	const int64 *arg[SIFT_WIN_MAX_FUNCS];	/* the same for the argument of each function that has one */
	const uint8 *arg_null[SIFT_WIN_MAX_FUNCS];
	uint32	   *perm;			/* scratch, n entries */
	uint32	   *tmp;			/* scratch, n entries */
	uint8	   *peer;			/* scratch, n entries */
	uint64	   *otag;			/* out: the tag of the o-th row of the sorted partitions */
	SiftWinVal *res;			/* out: n * nfuncs values, the function f of row o at o * nfuncs + f */
	uint16	   *res_nulls;		/* out: bit f: the result of function f is NULL */
} SiftParWinItem;

/* true; false with SIFT_ERR_SERIAL for a float8 sum that overflows (the leader raises it) */
extern bool sift_par_win_run(const SiftParWinSpec *sp, const SiftParWinItem *it, SiftErr *err);

#endif							/* SIFT_AGG_V6_PAR_WINPART_H */
