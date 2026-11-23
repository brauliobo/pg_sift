/*
 * agg/v6/v6_tseries_fast.c - the batch kernels of v6_tseries_fast.h.
 */
#include "agg/v6/v6_tseries_fast.h"

typedef unsigned __int128 u128;

#define MONTH_US_PER_DAY		INT64CONST(86400000000)
#define MONTH_DAYS_1970_TO_2000	10957
#define MONTH_MAX_DAYS			31

/* The reciprocal of d (d > 1) for fast_mod64 */
static inline u128
recip_of(uint64 d)
{
	return ~(u128) 0 / d + 1;
}

/* a % d for every a and every d > 1 from its reciprocal: two 64 x 64 -> 128 multiplies (Lemire, Kaser and Kurz, "Faster remainder by direct computation") */
static inline uint64
fast_mod64(uint64 a, u128 recip, uint64 d)
{
	const u128	low = recip * a;
	const u128	bottom = ((u128) (uint64) low * d) >> 64;
	const u128	top = (u128) (uint64) (low >> 64) * d;

	return (uint64) ((bottom + top) >> 64);
}

/* the bin of a finite source for a stride that is positive; recip 0 divides by stride itself (a constant after inlining) */
static inline __attribute__((always_inline)) SiftDateBinStatus
bin_slow(int64 ts, int64 origin, int64 stride, u128 recip, int64 *out)
{
	int64		diff, mod, result;

	if (!sift_ts_finite(ts))
	{
		*out = ts;
		return SDB_OK;
	}
	if (__builtin_sub_overflow(ts, origin, &diff))
		return SDB_INTERVAL_RANGE;
	mod = recip != 0 && diff >= 0 ? (int64) fast_mod64((uint64) diff, recip, (uint64) stride) : diff % stride;
	result = origin + (diff - mod);
	if (mod < 0 && (__builtin_sub_overflow(result, stride, &result) || result < SIFT_TS_MIN_VALID || result >= SIFT_TS_END_VALID))
		return SDB_TS_RANGE;
	*out = result;
	return SDB_OK;
}

SiftDateBinStatus
sift_datebin_row(int64 ts, int64 origin, int64 stride, int64 *out)
{
	return bin_slow(ts, origin, stride, 0, out);
}

static inline int64
sat_add(int64 a, int64 b)
{
	int64		s;

	return __builtin_add_overflow(a, b, &s) ? INT64_MAX : s;
}

/* The interval a bin answers for: every source of [lo, hi) bins to lo and none of them overflows the difference to the origin; empty (lo > hi) until a bin is seen */
typedef struct BinSpan
{
	int64		lo;
	int64		hi;
} BinSpan;

#define BIN_SPAN_EMPTY	((BinSpan) {INT64_MAX, INT64_MIN})

static inline bool
span_has(const BinSpan *s, int64 ts)
{
	return ts >= s->lo && ts < s->hi;
}

static inline __attribute__((always_inline)) SiftDateBinStatus
bin_run(const SiftDateBin *b, int64 stride, u128 recip, const SiftBatchSel *bs, const SiftVec *in, int64 *out, uint32 *err_row)
{
	const int64 *x = in->data;
	const int64 origin = b->origin;
	const int64 reach = origin < 0 ? INT64_MAX + origin + 1 : INT64_MAX;
	BinSpan		span = BIN_SPAN_EMPTY;
	SiftBatchIter it;
	uint32		r;

	sift_batch_iter_init(&it, bs);
	while (sift_batch_iter_next(&it, &r))
	{
		const int64 ts = x[r];

		if (!sift_vec_valid_at(in, r))
			continue;
		if (likely(span_has(&span, ts)))
			out[r] = span.lo;
		else
		{
			const SiftDateBinStatus st = bin_slow(ts, origin, stride, recip, &out[r]);

			if (st != SDB_OK)
			{
				*err_row = r;
				return st;
			}
			if (sift_ts_finite(ts) && out[r] >= SIFT_TS_MIN_VALID && out[r] < SIFT_TS_END_VALID)
				span = (BinSpan) {out[r], Min(sat_add(out[r], stride), reach)};
		}
	}
	return SDB_OK;
}

SiftDateBinStatus
sift_datebin_batch_run(const SiftDateBin *b, const SiftBatchSel *bs, const SiftVec *in, int64 *out, uint32 *err_row)
{
	switch (b->stride_us)
	{
		case INT64CONST(1000000):
			return bin_run(b, INT64CONST(1000000), 0, bs, in, out, err_row);
		case INT64CONST(60000000):
			return bin_run(b, INT64CONST(60000000), 0, bs, in, out, err_row);
		case INT64CONST(300000000):
			return bin_run(b, INT64CONST(300000000), 0, bs, in, out, err_row);
		case INT64CONST(600000000):
			return bin_run(b, INT64CONST(600000000), 0, bs, in, out, err_row);
		case INT64CONST(900000000):
			return bin_run(b, INT64CONST(900000000), 0, bs, in, out, err_row);
		case INT64CONST(1800000000):
			return bin_run(b, INT64CONST(1800000000), 0, bs, in, out, err_row);
		case INT64CONST(3600000000):
			return bin_run(b, INT64CONST(3600000000), 0, bs, in, out, err_row);
		case INT64CONST(21600000000):
			return bin_run(b, INT64CONST(21600000000), 0, bs, in, out, err_row);
		case INT64CONST(43200000000):
			return bin_run(b, INT64CONST(43200000000), 0, bs, in, out, err_row);
		case INT64CONST(86400000000):
			return bin_run(b, INT64CONST(86400000000), 0, bs, in, out, err_row);
		case INT64CONST(604800000000):
			return bin_run(b, INT64CONST(604800000000), 0, bs, in, out, err_row);
		case 1:
			return bin_run(b, 1, 0, bs, in, out, err_row);
		default:
			return bin_run(b, b->stride_us, recip_of((uint64) b->stride_us), bs, in, out, err_row);
	}
}

/* ---- date_trunc('month') ---- */

/* The first day of the month of a day counted from 2000-01-01 (the civil-from-days and days-from-civil of the proleptic Gregorian calendar, with the era as the unit) */
static int64
month_first_day(int64 day)
{
	const int64 z = day + MONTH_DAYS_1970_TO_2000 + 719468,
				era = (z >= 0 ? z : z - 146096) / 146097;
	const int64 doe = z - era * 146097,
				yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365,
				doy = doe - (365 * yoe + yoe / 4 - yoe / 100),
				mp = (5 * doy + 2) / 153,
				dom = doy - (153 * mp + 2) / 5 + 1;

	return day - (dom - 1);
}

void
sift_datemonth_batch(const SiftBatchSel *bs, const SiftVec *in, int64 *out)
{
	const int64 *x = in->data;
	BinSpan		span = BIN_SPAN_EMPTY;
	SiftBatchIter it;
	uint32		r;

	sift_batch_iter_init(&it, bs);
	while (sift_batch_iter_next(&it, &r))
	{
		const int64 ts = x[r];
		int64		first, lo, hi;

		if (!sift_vec_valid_at(in, r))
			continue;
		if (likely(span_has(&span, ts)))
		{
			out[r] = span.lo;
			continue;
		}
		if (!sift_ts_finite(ts))
		{
			out[r] = ts;
			continue;
		}
		first = month_first_day(ts >= 0 ? ts / MONTH_US_PER_DAY : -((-ts + MONTH_US_PER_DAY - 1) / MONTH_US_PER_DAY));
		lo = first * MONTH_US_PER_DAY;
		out[r] = lo;
		if (!__builtin_mul_overflow(month_first_day(first + MONTH_MAX_DAYS), MONTH_US_PER_DAY, &hi) && lo <= ts && ts < hi)
			span = (BinSpan) {lo, hi};
	}
}
