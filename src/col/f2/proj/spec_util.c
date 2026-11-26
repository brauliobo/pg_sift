/*
 * col/f2/proj/spec_util.c - the calendar of the text of a projection (its date and timestamp literals) and date_trunc over the dates and timestamps its keys may hold; an infinity is its
 * own bucket.
 */
#include "core/host.h"

#include "col/f2/proj/proj.h"

#define USECS_PER_DAY	INT64CONST(86400000000)
#define DAYS_1970_TO_2000	10957
#define USECS_PER_MIN	INT64CONST(60000000)
#define USECS_PER_HOUR	INT64CONST(3600000000)

static int64
floor_div(int64 a, int64 b)
{
	int64		q = a / b;

	return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/* Howard Hinnant's civil calendar: days since 1970-01-01 <-> year, month, day */
void
sift_proj_civil_from_days(int64 z, int64 *y, int *m, int *d)
{
	int64		era, yoe, doy, mp;

	z += 719468;
	era = floor_div(z, 146097);
	{
		int64		doe = z - era * 146097;

		yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	}
	mp = (5 * doy + 2) / 153;
	*d = (int) (doy - (153 * mp + 2) / 5 + 1);
	*m = (int) (mp < 10 ? mp + 3 : mp - 9);
	*y = yoe + era * 400 + (*m <= 2);
}

int64
sift_proj_days_from_civil(int64 y, int m, int d)
{
	int64		era, yoe, doy, doe;

	y -= m <= 2;
	era = floor_div(y, 400);
	yoe = y - era * 400;
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

int64
sift_proj_trunc_date(int64 days, SiftProjUnit unit)
{
	int64		y;
	int			m, d;

	if (days == PG_INT32_MIN || days == PG_INT32_MAX)
		return days;
	switch (unit)
	{
		case SPU_DAY:
			return days;
		case SPU_WEEK:
			return days - ((days % 7 + 7 + 5) % 7);
		default:
			break;
	}
	sift_proj_civil_from_days(days + DAYS_1970_TO_2000, &y, &m, &d);
	if (unit == SPU_QUARTER)
		m = (m - 1) / 3 * 3 + 1;
	else if (unit == SPU_YEAR)
		m = 1;
	return sift_proj_days_from_civil(y, m, 1) - DAYS_1970_TO_2000;
}

int64
sift_proj_trunc(int64 ts, SiftProjUnit unit)
{
	if (ts == PG_INT64_MIN || ts == PG_INT64_MAX)
		return ts;
	if (unit == SPU_MINUTE)
		return floor_div(ts, USECS_PER_MIN) * USECS_PER_MIN;
	if (unit == SPU_HOUR)
		return floor_div(ts, USECS_PER_HOUR) * USECS_PER_HOUR;
	return sift_proj_trunc_date(floor_div(ts, USECS_PER_DAY), unit) * USECS_PER_DAY;
}

int64
sift_proj_trunc_cell(SiftProjTime time, int64 v, SiftProjUnit unit)
{
	return time == SPT_DATE ? sift_proj_trunc_date(v, unit) : sift_proj_trunc(v, unit);
}

/* A value truncated to fine is truncated to coarse by truncating it again: every boundary of coarse is a boundary of fine (a week is not made of months, nor a month of weeks) */
bool
sift_proj_unit_refines(SiftProjUnit fine, SiftProjUnit coarse)
{
	static const uint8 rank[] = {0, 1, 2, 3, 0, 4, 5, 6};

	if (fine == coarse)
		return true;
	if (fine == SPU_WEEK || coarse == SPU_WEEK)
		return fine != SPU_WEEK && coarse == SPU_WEEK ? fine <= SPU_DAY : false;
	return rank[fine] != 0 && rank[coarse] != 0 && rank[fine] < rank[coarse];
}
