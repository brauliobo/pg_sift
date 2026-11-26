/*
 * agg/vexpr/cal.c - the calendar kernels of agg/vexpr/cal.h. The clock a timestamptz is read on is the one of its row: the offset of the piece of the zone that holds its
 * instant (core/zoneoff.h), so that a batch that crosses a clock change reads each side of it right; a result of a day or more is the instant of its local midnight, in the
 * piece that holds that midnight, which is not always the row's. A period of a fixed length (second to week) is cut by the reciprocal of the period: the timestamp is
 * made unsigned by a multiple of the period that is above every timestamp's distance from 0, the quotient is the high half of its product with floor(2^64 /
 * period), which is the quotient or one below it, and one compare puts it right. A month, a quarter, a year and the parts of a date go through the days of the
 * proleptic Gregorian calendar, by Howard Hinnant's algorithms over the 400 year eras (a day of the era, the year of the era, the day of the year and the
 * month counted from March): integer operations only, the same ones for every row, so that the loops are the same C for every instruction set (core/fmv.h).
 */
#include "postgres.h"

#include "agg/vexpr/cal.h"
#include "core/zoneoff.h"
#include "core/fmv.h"

#define USECS_PER_DAY_I		INT64CONST(86400000000)
#define MIN_TS_US			INT64CONST(-211813488000000000)
#define PG_EPOCH_UNIX_US	INT64CONST(946684800000000)
#define PG_EPOCH_UNIX_S		INT64CONST(946684800)
#define BIAS_LIMIT			UINT64CONST(300000000000000000)
#define MONDAY_US			(2 * USECS_PER_DAY_I)
#define DAYS_TO_0000_03_01	730425

static const uint64 period_us[] = {1000000, 60000000, 3600000000, 86400000000, 604800000000};

/* d = the local time (a + off) cut to a multiple of per from shift */
SIFT_FMV_DEFINE_VOID(static, trunc_fixed, (int64 *d, const int64 *a, uint16 n, uint64 per, uint64 recip, uint64 bias, uint64 shift, const int64 *off),
						 (d, a, n, per, recip, bias, shift, off),
{
	SIFT_IVDEP
	for (int i = 0; i < n; i++)
	{
		const uint64 u = (uint64) a[i] + (uint64) off[i] - shift + bias;
		uint64		q = (uint64) (((unsigned __int128) u * recip) >> 64);

		q += (u - q * per) >= per;
		d[i] = (int64) (q * per - bias + shift);
	}
})

SIFT_FMV_DEFINE_VOID(static, ts_days, (int32 *days, const int64 *a, uint16 n, const int64 *off), (days, a, n, off),
{
	const uint64 per = USECS_PER_DAY_I,
				recip = UINT64_MAX / per,
				k = BIAS_LIMIT / per + 1,
				bias = k * per;

	SIFT_IVDEP
	for (int i = 0; i < n; i++)
	{
		const uint64 u = (uint64) a[i] + (uint64) off[i] + bias;
		uint64		q = (uint64) (((unsigned __int128) u * recip) >> 64);

		q += (u - q * per) >= per;
		days[i] = (int32) ((int64) q - (int64) k);
	}
})

/* the year, month and day of a date given as days since 2000-01-01 */
SIFT_FMV_DEFINE_VOID(static, civil_from_days, (int32 *SIFT_RESTRICT y, int32 *SIFT_RESTRICT m, int32 *SIFT_RESTRICT dd, const int32 *SIFT_RESTRICT days, uint16 n),
					 (y, m, dd, days, n),
{
	for (int i = 0; i < n; i++)
	{
		const int32 z = (int32) ((uint32) days[i] + (uint32) DAYS_TO_0000_03_01);
		const int32 era = (z >= 0 ? z : z - 146096) / 146097;
		const int32 doe = z - era * 146097;
		const int32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		const int32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		const int32 mp = (5 * doy + 2) / 153;
		const int32 mm = mp < 10 ? mp + 3 : mp - 9;

		y[i] = yoe + era * 400 + (mm <= 2);
		m[i] = mm;
		dd[i] = doy - (153 * mp + 2) / 5 + 1;
	}
})

SIFT_FMV_DEFINE_VOID(static, days_from_civil, (int32 *SIFT_RESTRICT days, const int32 *SIFT_RESTRICT y, const int32 *SIFT_RESTRICT m, const int32 *SIFT_RESTRICT dd, uint16 n),
					 (days, y, m, dd, n),
{
	for (int i = 0; i < n; i++)
	{
		const int32 yy = y[i] - (m[i] <= 2);
		const int32 era = (yy >= 0 ? yy : yy - 399) / 400;
		const int32 yoe = yy - era * 400;
		const int32 doy = (153 * (m[i] > 2 ? m[i] - 3 : m[i] + 9) + 2) / 5 + dd[i] - 1;
		const int32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

		days[i] = era * 146097 + doe - DAYS_TO_0000_03_01;
	}
})

SIFT_FMV_DEFINE_VOID(static, days_to_local, (int64 *d, const int32 *days, uint16 n), (d, days, n),
{
	SIFT_IVDEP
	for (int i = 0; i < n; i++)
		d[i] = (int64) days[i] * USECS_PER_DAY_I;
})

static inline bool
is_special_ts(int64 v)
{
	return v == INT64_MIN || v == INT64_MAX;
}

static int
first_row(const uint64 *rows, uint16 n)
{
	for (int w = 0; w < (n + 63) / 64; w++)
		if (rows[w])
			return w * 64 + __builtin_ctzll(rows[w]);
	return -1;
}

static const int64 no_offset[SIFT_VEC_LEN];

/* the least and greatest of v over the rows of chk that are not an infinity of a; false when there is none */
static bool
finite_span(const int64 *v, const int64 *a, uint16 n, const uint64 *chk, int64 *lo, int64 *hi)
{
	*lo = INT64_MAX;
	*hi = INT64_MIN;
	for (int i = 0; i < n; i++)
		if ((chk[i >> 6] >> (i & 63) & 1) && !is_special_ts(a[i]))
		{
			*lo = Min(*lo, v[i]);
			*hi = Max(*hi, v[i]);
		}
	return *lo <= *hi;
}

/* offs[i] = the offset of the piece of the zone that holds the instant a[i], for the rows of chk (the others are 0) */
static void
row_offsets(int64 *offs, ZoneOff *zone, const int64 *a, uint16 n, const uint64 *chk)
{
	int64		lo,
				hi;

	if (!finite_span(a, a, n, chk, &lo, &hi))
	{
		memset(offs, 0, sizeof(int64) * n);
		return;
	}
	zone_cover(zone, lo, hi);
	for (int i = 0; i < n; i++)
		offs[i] = (chk[i >> 6] >> (i & 63) & 1) && !is_special_ts(a[i]) ? zone_offset_at(zone, a[i]) : 0;
}

/* offs[i] = the offset of the local time local[i], for the rows of chk (the others are 0): the piece PostgreSQL puts a local time in */
static void
local_offsets(int64 *offs, ZoneOff *zone, const int64 *local, const int64 *a, uint16 n, const uint64 *chk)
{
	int64		lo,
				hi,
				prev = 0,
				prev_off = 0;
	bool		have_prev = false;

	if (!finite_span(local, a, n, chk, &lo, &hi))
	{
		memset(offs, 0, sizeof(int64) * n);
		return;
	}
	zone_cover(zone, lo, hi);
	for (int i = 0; i < n; i++)
	{
		if (!((chk[i >> 6] >> (i & 63) & 1) && !is_special_ts(a[i])))
			offs[i] = 0;
		else
		{
			if (!have_prev || local[i] != prev)
				prev_off = zone_local_offset(zone, local[i]);
			prev = local[i];
			have_prev = true;
			offs[i] = prev_off;
		}
	}
}

#define DATE_TS_FIRST	(-2451545)
#define DATE_TS_END		106751983

int
vcal_date_to_ts(int64 *d, const int32 *a, uint16 n, ZoneOff *zone, const uint64 *chk)
{
	uint64		ok[SIFT_VEC_WORDS];
	int64		offs[SIFT_VEC_LEN];
	int			first = -1;

	memcpy(ok, chk, sizeof(uint64) * ((n + 63) / 64));
	for (int i = 0; i < n; i++)
	{
		const bool	inf = a[i] == INT32_MIN || a[i] == INT32_MAX;

		d[i] = inf ? (a[i] == INT32_MIN ? INT64_MIN : INT64_MAX) : (int64) a[i] * USECS_PER_DAY_I;
		if (!inf && (a[i] < DATE_TS_FIRST || a[i] >= DATE_TS_END) && (chk[i >> 6] >> (i & 63) & 1))
		{
			first = first < 0 ? i : first;
			ok[i >> 6] &= ~(UINT64CONST(1) << (i & 63));
			d[i] = 0;
		}
	}
	if (zone)
	{
		local_offsets(offs, zone, d, d, n, ok);
		for (int i = 0; i < n; i++)
			d[i] -= offs[i];
	}
	return first;
}

int
vcal_date_trunc(int64 *d, const int64 *a, uint16 n, SiftVxUnit unit, ZoneOff *zone, const uint64 *chk, int32 *scratch)
{
	uint64		bad[SIFT_VEC_WORDS],
				is_min[SIFT_VEC_WORDS],
				is_max[SIFT_VEC_WORDS];
	int64		offs[SIFT_VEC_LEN];
	const int64 *off = no_offset;

	for (int w = 0; w < (n + 63) / 64; w++)
	{
		uint64		lo = 0,
					hi = 0;

		for (int j = 0; j < Min(64, n - w * 64); j++)
		{
			lo |= (uint64) (a[w * 64 + j] == INT64_MIN) << j;
			hi |= (uint64) (a[w * 64 + j] == INT64_MAX) << j;
		}
		is_min[w] = lo;
		is_max[w] = hi;
	}
	if (zone)
	{
		row_offsets(offs, zone, a, n, chk);
		off = offs;
	}
	if (unit <= SVU_WEEK)
	{
		const uint64 per = period_us[unit],
					bias = per * (BIAS_LIMIT / per + 1);

		trunc_fixed(d, a, n, per, UINT64_MAX / per, bias, unit == SVU_WEEK ? (uint64) MONDAY_US : 0, off);
	}
	else
	{
		int32	   *days = scratch,
				   *y = scratch + SIFT_VEC_LEN,
				   *m = scratch + 2 * SIFT_VEC_LEN,
				   *dd = scratch + 3 * SIFT_VEC_LEN;

		ts_days(days, a, n, off);
		civil_from_days(y, m, dd, days, n);
		for (int i = 0; i < n; i++)
		{
			m[i] = unit == SVU_MONTH ? m[i] : unit == SVU_QUARTER ? (m[i] - 1) / 3 * 3 + 1 : 1;
			dd[i] = 1;
		}
		days_from_civil(days, y, m, dd, n);
		days_to_local(d, days, n);
	}
	if (zone)
	{
		if (unit >= SVU_DAY)
			local_offsets(offs, zone, d, a, n, chk);
		for (int i = 0; i < n; i++)
			d[i] -= offs[i];
	}
	for (int w = 0; w < (n + 63) / 64; w++)
	{
		uint64		low = 0,
					rows = chk[w];

		for (int j = 0; j < Min(64, n - w * 64); j++)
		{
			const int	i = w * 64 + j;

			if ((is_min[w] | is_max[w]) >> j & 1)
				d[i] = (is_max[w] >> j & 1) ? INT64_MAX : INT64_MIN;
			else
				low |= (uint64) (d[i] < MIN_TS_US) << j;
		}
		bad[w] = low & rows;
	}
	return first_row(bad, n);
}

int
vcal_extract_scale(SiftVxField field, bool is_date)
{
	return field == SVF_EPOCH && !is_date ? 6 : 0;
}

static inline bool
is_infinity_extract(SiftVxField field)
{
	return field == SVF_YEAR || field == SVF_EPOCH;
}

void
vcal_extract(int64 *d, const void *a, bool is_date, uint16 n, SiftVxField field, ZoneOff *zone, const uint64 *chk, uint64 *special, int32 *scratch)
{
	int32	   *days = scratch,
			   *y = scratch + SIFT_VEC_LEN,
			   *m = scratch + 2 * SIFT_VEC_LEN,
			   *dd = scratch + 3 * SIFT_VEC_LEN,
			   *jan1 = scratch + 4 * SIFT_VEC_LEN;
	const int64 *ts = a;
	const int32 *dt = a;
	int64		offs[SIFT_VEC_LEN];
	const int64 *off = no_offset;

	for (int w = 0; w < (n + 63) / 64; w++)
	{
		uint64		sp = 0;

		for (int j = 0; j < Min(64, n - w * 64); j++)
		{
			const int	i = w * 64 + j;

			sp |= (uint64) (is_date ? (dt[i] == INT32_MIN || dt[i] == INT32_MAX) : is_special_ts(ts[i])) << j;
		}
		special[w] = is_infinity_extract(field) ? 0 : sp;
	}
	if (is_date)
		memcpy(days, dt, (size_t) n * sizeof(int32));
	else
	{
		if (zone)
		{
			row_offsets(offs, zone, ts, n, chk);
			off = offs;
		}
		ts_days(days, ts, n, off);
	}
	if (field != SVF_EPOCH && field != SVF_HOUR && field != SVF_MINUTE && field != SVF_DOW && field != SVF_ISODOW)
		civil_from_days(y, m, dd, days, n);
	if (field == SVF_DOY)
	{
		for (int i = 0; i < n; i++)
		{
			m[i] = 1;
			dd[i] = 1;
		}
		days_from_civil(jan1, y, m, dd, n);
	}
	for (int i = 0; i < n; i++)
	{
		const int32 dow = (int32) ((days[i] + 6) % 7 + 7) % 7;

		switch (field)
		{
			case SVF_YEAR: d[i] = y[i] > 0 ? y[i] : y[i] - 1; break;
			case SVF_QUARTER: d[i] = (m[i] - 1) / 3 + 1; break;
			case SVF_MONTH: d[i] = m[i]; break;
			case SVF_DAY: d[i] = dd[i]; break;
			case SVF_DOW: d[i] = dow; break;
			case SVF_ISODOW: d[i] = dow == 0 ? 7 : dow; break;
			case SVF_DOY: d[i] = days[i] - jan1[i] + 1; break;
			case SVF_HOUR: d[i] = (int64) (((ts[i] + off[i]) - (int64) days[i] * USECS_PER_DAY_I) / 3600000000); break;
			case SVF_MINUTE: d[i] = (int64) ((((ts[i] + off[i]) - (int64) days[i] * USECS_PER_DAY_I) % 3600000000) / 60000000); break;
			default: d[i] = is_date ? (int64) dt[i] * 86400 + PG_EPOCH_UNIX_S : ts[i] + PG_EPOCH_UNIX_US;
		}
		if (is_infinity_extract(field) && (is_date ? (dt[i] == INT32_MIN || dt[i] == INT32_MAX) : is_special_ts(ts[i])))
			d[i] = (is_date ? dt[i] == INT32_MAX : ts[i] == INT64_MAX) ? INT64_MAX : INT64_MIN;
	}
}
