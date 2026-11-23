/*
 * core/f2/unit_queue.h - the queue of units the workers of a parallel scan of a table take their next piece of work from: 256 bytes in shared memory, each claim a share of what is left,
 * so that the last worker is not left a long tail. The claimed word and the read-only ones are a cache line apart. The units are numbered [0, nunits); what a unit is, the scan says. (The morsels of the parallel aggregate, core/par/morsel.h,
 * are another queue: variable-length lanes that the aggregate sizes by row.)
 */
#ifndef SIFT_UNIT_QUEUE_H
#define SIFT_UNIT_QUEUE_H

#include "postgres.h"

#include "port/atomics.h"

#define SIFT_UNIT_QUEUE_BYTES		256
#define SIFT_UNIT_QUEUE_SHARE		8	/* a claim is 1/8 of the units left ... */
#define SIFT_UNIT_QUEUE_MAX_CLAIM	16	/* ... and at most this many */

typedef struct SiftUnitQueue
{
	pg_atomic_uint64 next;		/* the first unit nobody has claimed; the only word that changes while the scan runs */
	char		pad[PG_CACHE_LINE_SIZE - sizeof(pg_atomic_uint64)];
	uint64		nunits;
	char		pad2[SIFT_UNIT_QUEUE_BYTES - PG_CACHE_LINE_SIZE - sizeof(uint64)];
} SiftUnitQueue;

StaticAssertDecl(sizeof(SiftUnitQueue) == SIFT_UNIT_QUEUE_BYTES, "SiftUnitQueue must be 256 bytes");

extern void sift_unit_queue_init(SiftUnitQueue *q, uint64 nunits);
extern void sift_unit_queue_reset(SiftUnitQueue *q);

/* The next run of units [*first, *first + *count) for the caller; false when none is left */
extern bool sift_unit_queue_next(SiftUnitQueue *q, uint64 *first, uint32 *count);

#endif							/* SIFT_UNIT_QUEUE_H */
