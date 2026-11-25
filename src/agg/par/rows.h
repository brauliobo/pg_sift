/*
 * agg/par/rows.h - ORDER BY .. LIMIT K of the rows of one table for W threads, as an operator of core/par/morsel.h, in pure C (arenas of the leader, no server call; see agg/par/group.h for the
 * rules of the threads).
 *
 * Each thread keeps the K best rows it has seen (core/par/topk.h: a row is copied only once it is among them) and raises the bound that all share (SiftAtomicBound) as soon as its K-th best is
 * better than it was: every thread then skips the rows that cannot enter before it compares them, and the leader skips the granules whose zone of the first sort column cannot hold one
 * (sift_par_rows_excludes). The order is the sort columns as the statement gives them (direction, NULLs first or last; an integer, or text in byte order), then the address of the row: a total
 * order, so that the K best and their order are the same for any number of threads. finish hands them to the leader in order.
 */
#ifndef SIFT_AGG_V6_PAR_ROWS_H
#define SIFT_AGG_V6_PAR_ROWS_H

#include "postgres.h"

#include "agg/par/group.h"
#include "core/par/morsel.h"

#define SIFT_PAR_ROWS_MAX_KEYS	3
#define SIFT_PAR_ROWS_K_MAX		1024

typedef struct SiftParRowsSpec
{
	uint8		nkeys;			/* the sort columns, 1 .. SIFT_PAR_ROWS_MAX_KEYS */
	bool		str[SIFT_PAR_ROWS_MAX_KEYS];	/* a text column (bytes in byte order); else a 64-bit integer */
	bool		desc[SIFT_PAR_ROWS_MAX_KEYS];
	bool		nulls_first[SIFT_PAR_ROWS_MAX_KEYS];
	uint16		k;
} SiftParRowsSpec;

/* The rows of a unit of the scan: their addresses, and the sort column j in key[j][0..n) (isnull[j]: NULL for none) or str[j][0..n) (length SIFT_STR_NULL: NULL) */
typedef struct SiftParRowsBatch
{
	uint32		n;
	const uint64 *tid;
	const int64 *key[SIFT_PAR_ROWS_MAX_KEYS];
	const bool *isnull[SIFT_PAR_ROWS_MAX_KEYS];
	const SiftStrRef *str[SIFT_PAR_ROWS_MAX_KEYS];
} SiftParRowsBatch;

typedef struct SiftParRowsRow
{
	uint64		tid;
	int64		key[SIFT_PAR_ROWS_MAX_KEYS];
	SiftStrRef	str[SIFT_PAR_ROWS_MAX_KEYS];
	bool		isnull[SIFT_PAR_ROWS_MAX_KEYS];
} SiftParRowsRow;

typedef struct SiftParRowsHooks
{
	/* Thread `worker`: the rows of the items [lo, hi), in memory of scratch that is valid until the next call; false, with err filled, when it cannot (the items are none of them offered) */
	bool		(*fill) (void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParRowsBatch *out, SiftErr *err);
	/* The leader, once for each of the K best in order (the row is the callee's for the call only) */
	void		(*emit) (void *arg, const SiftParRowsRow *row);
	void	   *arg;
} SiftParRowsHooks;

typedef struct SiftParRows SiftParRows;

/* NULL, with err set, for a spec that does not hold or an arena that is too small for the K best of every thread */
extern SiftParRows *sift_par_rows_create(SiftParArenas *ar, const SiftParRowsSpec *spec, const SiftParRowsHooks *hooks, SiftErr *err);

/* The operator: sift_par_run(cfg, &sift_par_rows_ops, r, ar, rel) */
extern const SiftParOps sift_par_rows_ops;

/* Whether a granule whose first sort column (an integer) lies in [min, max] (has_null: and holds NULLs) cannot hold a row better than the K-th best the threads have so far; leader or thread */
extern bool sift_par_rows_excludes(const SiftParRows *r, int64 min, int64 max, bool has_null);

#endif							/* SIFT_AGG_V6_PAR_ROWS_H */
