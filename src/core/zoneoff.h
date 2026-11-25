/*
 * core/zoneoff.h - the offsets of a time zone over a stretch of instants (core/zoneoff.c)
 */
#ifndef SIFT_CORE_ZONEOFF_H
#define SIFT_CORE_ZONEOFF_H

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
	int64		reach;			/* the pieces are PostgreSQL's boundaries up to here; after it the zone has no more boundaries in its table but a rule that goes on changing the offset */
} ZoneOff;

extern ZoneOff *zone_open(MemoryContext cxt, const pg_tz *tz);
extern void zone_cover(ZoneOff *z, int64 lo, int64 hi);
extern bool zone_is_utc(const pg_tz *tz);
extern int	zone_piece(const ZoneOff *z, int64 t);
extern int64 zone_offset_at(const ZoneOff *z, int64 t);
extern int64 zone_local_offset(const ZoneOff *z, int64 local);
extern int64 zone_bucket(const ZoneOff *z, int64 t, int64 us);
extern int64 zone_cut(const ZoneOff *z, int64 t, int64 us);
extern int64 zone_instant(const ZoneOff *z, int64 bucket, int64 us);

#endif
