/*
 * agg/zoneoff.c - the offsets of a time zone over a stretch of instants
 *
 * date_trunc, extract and date() of a timestamptz read the clock of the session's zone. A step of that clock (a day, an hour) is not a step of UTC where the zone has an
 * offset, and the offset changes where it has daylight saving time. The offsets of the zone over the instants of a batch are a few pieces of constant offset (PostgreSQL's
 * pg_next_dst_boundary finds the boundaries), and the number of the step of an instant is the quotient of its local time, as the local time PostgreSQL's own functions
 * start from (timestamp2tm reads the same table).
 */
#include "postgres.h"

#include "datatype/timestamp.h"
#include "utils/datetime.h"

#include "core/host.h"
#include "agg/zoneoff.h"

/* the instants of a step reach this far beyond the instants of the rows */
#define ZONE_MARGIN		(3 * USECS_PER_DAY)
#define UNIX_TO_PG_SECS	((int64) SECS_PER_DAY * (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE))

ZoneOff *
zone_open(MemoryContext cxt, const pg_tz *tz)
{
	ZoneOff    *z = MemoryContextAllocZero(cxt, sizeof(ZoneOff));

	z->tz = tz;
	z->cxt = cxt;
	z->lo = PG_INT64_MAX;
	z->hi = PG_INT64_MIN;
	return z;
}

static int64
floor_secs(int64 us)
{
	return us / USECS_PER_SEC - (us % USECS_PER_SEC < 0);
}

static void
zone_push(ZoneOff *z, int64 from, int64 off)
{
	if (z->n == z->cap)
	{
		z->cap = z->cap ? z->cap * 2 : 16;
		z->from = z->from ? repalloc(z->from, z->cap * sizeof(int64)) : MemoryContextAlloc(z->cxt, z->cap * sizeof(int64));
		z->off = z->off ? repalloc(z->off, z->cap * sizeof(int64)) : MemoryContextAlloc(z->cxt, z->cap * sizeof(int64));
	}
	z->from[z->n] = from;
	z->off[z->n++] = off;
}

/* the pieces cover [lo, hi] and the margin around it (the window only grows) */
void
zone_cover(ZoneOff *z, int64 lo, int64 hi)
{
	int64		t,
				end;

	if (z->n > 0 && lo >= z->lo && hi <= z->hi)
		return;
	z->lo = Min(z->lo, lo);
	z->hi = Max(z->hi, hi);
	t = z->lo < MIN_TIMESTAMP + ZONE_MARGIN ? MIN_TIMESTAMP : z->lo - ZONE_MARGIN;
	end = z->hi > END_TIMESTAMP - ZONE_MARGIN ? END_TIMESTAMP : z->hi + ZONE_MARGIN;
	z->n = 0;
	for (;;)
	{
		pg_time_t	at = floor_secs(t) + UNIX_TO_PG_SECS,
					boundary = 0;
		long		before,
					after;
		int			bdst,
					adst;
		int			r = pg_next_dst_boundary(&at, &before, &bdst, &boundary, &after, &adst, z->tz);

		SIFT_INVARIANT(r >= 0, "sift: no offsets of the time zone at %lld", (long long) t);
		zone_push(z, t, (int64) before * USECS_PER_SEC);
		if (r == 0 || (boundary - UNIX_TO_PG_SECS) * USECS_PER_SEC > end)
			break;
		t = (boundary - UNIX_TO_PG_SECS) * USECS_PER_SEC;
	}
}

/* the piece of the instant t (the first or the last one for an instant the pieces do not reach) */
int
zone_piece(const ZoneOff *z, int64 t)
{
	int			lo = 0,
				hi = z->n - 1;

	while (lo < hi)
	{
		int			mid = (lo + hi + 1) >> 1;

		if (z->from[mid] <= t)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

/* the number of the step of us microseconds of the local clock that the instant t is in */
int64
zone_bucket(const ZoneOff *z, int64 t, int64 us)
{
	int64		l = t + z->off[zone_piece(z, t)];

	return l / us - (l % us < 0);
}

/*
 * An instant that is in step `bucket` of the local clock, for the key's expression to be evaluated at: the middle of the step, in the offset of a piece that puts it there
 * (a local time that a clock change repeats has two, and the step of one that it skips has none: any instant will do for an expression nobody reads).
 */
int64
zone_instant(const ZoneOff *z, int64 bucket, int64 us)
{
	int64		local = bucket * us + us / 2,
				t = 0;

	for (int i = 0; i < z->n; i++)
	{
		t = local - z->off[i];
		if (zone_bucket(z, t, us) == bucket)
			break;
	}
	return Max(MIN_TIMESTAMP, Min(t, END_TIMESTAMP - 1));
}

/* the instant date_trunc cuts t to, to a step of us microseconds of the local clock, keeping the offset of t */
int64
zone_cut(const ZoneOff *z, int64 t, int64 us)
{
	int64		l = t + z->off[zone_piece(z, t)],
				rem = l % us;

	return t - (rem < 0 ? rem + us : rem);
}
