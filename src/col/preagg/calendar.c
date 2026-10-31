/*
 * col/preagg/calendar.c - the calendar cells of a time leaf: days, months, their keys
 * the cells a dimension of a cube takes from a date or a timestamp
 * a pure function of the value
 */
#include "core/host.h"
#include "col/preagg/pagg_build_int.h"

#define PAGG_USECS_DAY		INT64CONST(86400000000)

#define PAGG_EPOCH_DAYS		10957			/* 1970-01-01 to 2000-01-01 */

#define PAGG_TS_DAYS		106751991		/* the days a timestamp holds, either side of 2000-01-01 (INT64_MAX / USECS_PER_DAY) */

#define PAGG_DATE_DAYS		INT64CONST(2147483000)	/* and a date, which also has infinities */

/* Days since 1970-01-01 of a civil date and the civil month of a day number: the proleptic Gregorian calendar PostgreSQL's date_trunc uses (H. Hinnant's algorithms) */
static int64
civil_days(int64 y, unsigned m)
{
	int64		era;
	unsigned	yoe,
				doy;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned) (y - era * 400);
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5;
	return era * 146097 + (int64) (yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
}

static void
civil_month(int64 z, int64 *y, unsigned *m)
{
	int64		era;
	unsigned	doe,
				yoe,
				doy,
				mp;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (unsigned) (z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = yoe + era * 400 + (*m <= 2);
}

static int64
day_of(int64 us)
{
	int64		d = us / PAGG_USECS_DAY;

	return us % PAGG_USECS_DAY < 0 ? d - 1 : d;
}

/* the first day a cell may start on: 4713 BC, so that the start of the cell of any day a timestamp holds is a timestamp too */
static int64
cal_min(void)
{
	return civil_days(-4712, 1) - PAGG_EPOCH_DAYS;
}

int64
sift_cpagg_cal_cell(int grain, int64 day)
{
	int64		y;
	unsigned	m;

	if (grain == SIFT_CAL_DAY)
		return day;
	civil_month(day + PAGG_EPOCH_DAYS, &y, &m);
	return civil_days(y, grain == SIFT_CAL_MONTH ? m : 1) - PAGG_EPOCH_DAYS;
}

int64
sift_cpagg_cal_next(int grain, int64 day)
{
	int64		y;
	unsigned	m;

	if (grain == SIFT_CAL_DAY)
		return day + 1;
	civil_month(day + PAGG_EPOCH_DAYS, &y, &m);
	if (grain == SIFT_CAL_MONTH && m < 12)
		return civil_days(y, m + 1) - PAGG_EPOCH_DAYS;
	return civil_days(y + 1, 1) - PAGG_EPOCH_DAYS;
}

bool
sift_cpagg_cal_key_ok(int type, int grain, int64 key)
{
	int64		lim = type == SIFT_COLT_DATE ? PAGG_DATE_DAYS : PAGG_TS_DAYS;

	return grain >= SIFT_CAL_DAY && grain <= SIFT_CAL_YEAR && key >= cal_min() && key <= lim && sift_cpagg_cal_cell(grain, key) == key;
}

bool
sift_cpagg_cal_type(const SiftColSchema *s, int c)
{
	const SiftColDef *d = &s->cols[c];

	return (d->type == SIFT_COLT_DATE || d->type == SIFT_COLT_TIMESTAMP || d->type == SIFT_COLT_TIMESTAMPTZ) && d->store == SIFT_COLS_INT && d->parent < 0;
}

/* The day of each row of a date or timestamp leaf (a timestamptz in UTC); false (the unit has no calendar dim) for an infinity or a day no cell can start on */
bool
cpagg_cal_days(const SiftCPaggCol *c, bool date, uint32 nrows, int64 *out)
{
	int64		lo = cal_min(),
				hi = date ? PAGG_DATE_DAYS : PAGG_TS_DAYS;

	for (uint32 i = 0; i < nrows; i++)
	{
		int64		k = c->key[i],
					d;

		if (is_null(c->nulls, i))
		{
			out[i] = 0;
			continue;
		}
		if (date ? (k == PG_INT32_MIN || k == PG_INT32_MAX) : (k == PG_INT64_MAX || k == PG_INT64_MIN))
			return false;
		d = date ? k : day_of(k);
		if (d < lo || d > hi)
			return false;
		out[i] = d;
	}
	return true;
}

/* the cell of a grain of each day of cal_days (the NULLs keep 0) */
void
cpagg_cal_cells(const SiftCPaggCol *c, uint32 nrows, int grain, const int64 *days, int64 *out)
{
	int64		last_day = 0,
				last_cell = 0;
	bool		have = false;

	for (uint32 i = 0; i < nrows; i++)
	{
		if (is_null(c->nulls, i))
		{
			out[i] = 0;
			continue;
		}
		if (!have || days[i] != last_day)
		{
			last_cell = sift_cpagg_cal_cell(grain, days[i]);
			last_day = days[i];
			have = true;
		}
		out[i] = last_cell;
	}
}

bool
sift_cpagg_cal_rows(const SiftColSchema *s, int leaf, int grain, const SiftCPaggCol *c, int64 *cells)
{
	int64	   *days = palloc(Max(c->n, 1) * sizeof(int64));
	bool		ok = cpagg_cal_days(c, s->cols[leaf].type == SIFT_COLT_DATE, c->n, days);

	if (ok)
		cpagg_cal_cells(c, c->n, grain, days, cells);
	pfree(days);
	return ok;
}
