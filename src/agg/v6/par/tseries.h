/*
 * agg/v6/par/tseries.h - the batch of a time-bucket aggregation, in pure C: GROUP BY date_bin(stride, ts, origin) (or date_trunc('month', ts)) of a time series, with integer keys beside it and the sums
 * of integer columns, as the words that agg/v6/par/group.h folds.
 *
 * The bins are those of the serial scan (agg/v6/v6_datebin_win.h: the same quotient, the same infinite timestamps, the NULL bin of a NULL source). A key is the 64-bit value with its sign bit flipped,
 * all ones is the NULL key. What the words cannot say exactly is the scan's: a bin that raises (the scan raises it only for a row that survives its predicates), a NULL in a counted or summed column, a key
 * whose word is the NULL one.
 */
#ifndef SIFT_AGG_V6_PAR_TSERIES_H
#define SIFT_AGG_V6_PAR_TSERIES_H

#include "postgres.h"

#include "agg/v6/par/cols.h"
#include "agg/v6/par/group.h"
#include "agg/v6/v6_datebin_win.h"

#define SIFT_TS_NULL_WORD		UINT64_MAX
#define SIFT_TS_SIGN			(UINT64CONST(1) << 63)
#define SIFT_TS_LEAVES_MAX		(SIFT_PAR_GROUP_MAX_KEYS + 2 * SIFT_PAR_GROUP_MAX_SUM)

/* One key of the statement: a column of the window, binned or not */
typedef struct SiftTsKey
{
	int			at;				/* its column among the columns of the shape */
	bool		binned;
	bool		month;
	SiftDateBin bin;
} SiftTsKey;

typedef struct SiftTsShape
{
	int			nk;
	SiftTsKey	key[SIFT_PAR_GROUP_MAX_KEYS];
	int			nsum;
	int			sum_at[SIFT_PAR_GROUP_MAX_SUM];
	int			ncount;			/* the columns COUNT(col) counts */
	int			count_at[SIFT_PAR_GROUP_MAX_SUM];
	int			ncols;
	SiftParGroupSpec gs;
} SiftTsShape;

/* The key and argument words of the n rows of a granule whose bit is set in active (nact of them), in memory of a. false with err: SIFT_ERR_SERIAL for the rows the scan must do, SIFT_ERR_ARENA_FULL */
extern bool sift_tseries_batch(const SiftTsShape *sh, const SiftParCol *col, const uint64 *active, uint32 nact, int n, SiftArena *a, SiftErr *err, SiftParGroupBatch *out);

/* The value of a key word: false for the NULL key */
static inline bool
sift_tseries_key_value(uint64 word, int64 *v)
{
	*v = (int64) (word ^ SIFT_TS_SIGN);
	return word != SIFT_TS_NULL_WORD;
}

#endif							/* SIFT_AGG_V6_PAR_TSERIES_H */
