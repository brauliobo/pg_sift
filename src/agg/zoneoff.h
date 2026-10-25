/*
 * agg/zoneoff.h - the offsets of a time zone over a stretch of instants (agg/zoneoff.c)
 */
#ifndef SIFT_AGG_ZONEOFF_H
#define SIFT_AGG_ZONEOFF_H

#include "pgtime.h"

/* the clock of a zone as pieces of constant offset: piece i starts at from[i] (microseconds since 2000-01-01 UTC) and local = instant + off[i] */
typedef struct ZoneOff
{
	const pg_tz *tz;
	MemoryContext cxt;
	int			n;
	int			cap;
	int64	   *from;
	int64	   *off;
	int64		lo;				/* the instants the pieces cover */
	int64		hi;
} ZoneOff;

extern ZoneOff *zone_open(MemoryContext cxt, const pg_tz *tz);
extern void zone_cover(ZoneOff *z, int64 lo, int64 hi);
extern int	zone_piece(const ZoneOff *z, int64 t);
extern int64 zone_bucket(const ZoneOff *z, int64 t, int64 us);
extern int64 zone_cut(const ZoneOff *z, int64 t, int64 us);
extern int64 zone_instant(const ZoneOff *z, int64 bucket, int64 us);

#endif
