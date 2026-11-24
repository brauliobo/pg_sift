/*
 * agg/vexpr_cal.h - the calendar kernels of the expression machine (agg/vexpr.c): date_trunc and EXTRACT of a timestamp (int64 microseconds since
 * 2000-01-01) or a date (int32 days since 2000-01-01) over the rows of a batch. Each takes the rows whose result matters (chk: the live rows that are valid)
 * and gives the first row, in row order, whose result PostgreSQL would not give (-1: none), so that the machine can order that error among the others.
 */
#ifndef SIFT_AGG_VEXPR_CAL_H
#define SIFT_AGG_VEXPR_CAL_H

#include "agg/vexpr.h"
#include "core/zoneoff.h"

#pragma GCC visibility push(hidden)

/*
 * d = date_trunc(unit, a) of a timestamp (zone NULL) or of a timestamptz read in the zone (it covers the instants the call needs): the local time of the row cut, and
 * for a second, minute or hour the instant of it in the row's own offset, for a day or more in the offset PostgreSQL gives that local time. The first row of chk whose
 * result is before the smallest timestamp, or -1.
 */
extern int	vcal_date_trunc(int64 *d, const int64 *a, uint16 n, SiftVxUnit unit, ZoneOff *zone, const uint64 *chk, int32 *scratch);

/*
 * d = the date a[i] (int32 days) as the timestamp PostgreSQL makes of it, for the rows of chk: its midnight, in the zone for a timestamptz (zone NULL: a timestamp); an infinity stays one.
 * The first row of chk whose date is outside the timestamps, or -1.
 */
extern int	vcal_date_to_ts(int64 *d, const int32 *a, uint16 n, ZoneOff *zone, const uint64 *chk);

/*
 * d = EXTRACT(field FROM a): a is a date when is_date, else a timestamp, read in the zone when it is a timestamptz (NULL: a timestamp); the result is the numeric of scale 0 (6 for the epoch of a timestamp) in int64.
 * special: the rows of a that are an infinity, which are NULL for every field but year and epoch; the first row of chk that is an infinity under year or
 * epoch is the result (those have no value in 64 bits), -1 when none.
 */
extern int	vcal_extract(int64 *d, const void *a, bool is_date, uint16 n, SiftVxField field, ZoneOff *zone, const uint64 *chk, uint64 *special, int32 *scratch);

/* the scale of EXTRACT(field FROM a) */
extern int	vcal_extract_scale(SiftVxField field, bool is_date);

/* the int32 scratch the kernels need: 5 arrays of SIFT_VEC_LEN */
#define VCAL_SCRATCH_INTS	(5 * SIFT_VEC_LEN)

#pragma GCC visibility pop

#endif							/* SIFT_AGG_VEXPR_CAL_H */
