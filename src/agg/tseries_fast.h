/*
 * agg/tseries_fast.h - the batch kernels of date_bin() and date_trunc('month') of v6_datebin_win.h.
 *
 * A time series arrives in time order, so the rows of a batch fall in a few bins: a kernel keeps the bin of the last row as the interval [lo, hi) and answers a row inside it with two
 * comparisons and no division. A row outside it takes the arithmetic of timestamp_bin (the strides of every dashboard are divisions by a constant, any other stride the exact
 * 128-bit reciprocal of fast_mod64) and the interval it lands in becomes the new one. The interval holds only bins whose arithmetic raises nothing for any of their rows, so a
 * row of the interval has the answer, and the error, the arithmetic gives it.
 */
#ifndef SIFT_AGG_V6_TSERIES_FAST_H
#define SIFT_AGG_V6_TSERIES_FAST_H

#include "core/host.h"

#include "agg/vec.h"
#include "agg/batch.h"
#include "agg/win/datebin.h"

#define SIFT_TS_MIN_VALID	INT64CONST(-211813488000000000)
#define SIFT_TS_END_VALID	INT64CONST(9223371331200000000)

static inline bool
sift_ts_finite(int64 t)
{
	return t != INT64_MIN && t != INT64_MAX;
}

/* timestamp_bin of one finite or infinite source for a stride that is positive (the checks and their order are PostgreSQL's) */
extern SiftDateBinStatus sift_datebin_row(int64 ts, int64 origin, int64 stride, int64 *out);

/* The active rows of the batch whose input is not NULL into out[row], for a stride that is valid (b->stride_status == SDB_OK); the first row that raises ends the batch (*err_row) */
extern SiftDateBinStatus sift_datebin_batch_run(const SiftDateBin *b, const SiftBatchSel *bs, const SiftVec *in, int64 *out, uint32 *err_row);

#endif							/* SIFT_AGG_V6_TSERIES_FAST_H */
