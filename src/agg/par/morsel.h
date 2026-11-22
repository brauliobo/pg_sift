/*
 * agg/par/morsel.h - the work queue of a parallel scan: the granules of every part of the scan as one flat list, which the participants
 * (the leader and the workers) take from by a compare-and-swap on a cursor.
 *
 * A claim is a run of granules of one part: (part, first, n). The run is long while the list is long and shrinks as it is consumed, so
 * that the participants finish together whatever the sizes of the parts: guided, SIFT_MORSEL_MAX granules while more than
 * 4 * SIFT_MORSEL_MAX * participants are left, then a power of 2 below left / (4 * participants) down to one granule (the tail of the
 * scan is a granule of work apart, not a morsel). A run never crosses the end of a part, so a claim is read through one part, and a
 * part with a thousand granules is shared by all the participants.
 *
 * One cursor that thirty participants swap is a cache line that every core of every chiplet takes from the others for each claim. The list is
 * cut into lanes, SIFT_MORSEL_LANES at most, each a contiguous share of it with a cursor of its own on a line of its own: a participant
 * takes from the lane its number selects (participants of a lane are neighbours, and so are the granules they read, so what they read is
 * in the cache of their chiplet), and when its lane is empty it takes from the lane with the most granules left, so that the lists finish
 * together all the same. With one participant there is one lane and the list is read in order.
 *
 * A Top-N tournament over the granules (agg/topn_v6.h) shares what it has learned in topn_theta_bits, alone on its own cache line pair: the encoded
 * bound of the K-th best row of the participant that is furthest along (0 = none yet). It only grows (a compare-and-swap that keeps the larger), and every
 * participant reads it before it takes a granule; once a granule cannot hold a row better than the bound, the rest of the list cannot either and
 * sift_morsel_stop ends it for all.
 *
 * The queue is in shared memory, allocated with sift_morsel_size() bytes aligned to a cache line, and has no pointer: it is valid at any address.
 */
#ifndef SIFT_AGG_PAR_MORSEL_H
#define SIFT_AGG_PAR_MORSEL_H

#include "port/atomics.h"

/* the most granules of a claim: 8 * 8192 rows */
#define SIFT_MORSEL_MAX			8

typedef struct SiftMorsel
{
	uint32		part;
	uint32		first;			/* the first granule of the run, within the part */
	uint32		n;
} SiftMorsel;

/* the lanes of a queue: the most that the participants are spread over */
#define SIFT_MORSEL_LANES		8

typedef struct SiftMorselLane
{
	pg_atomic_uint32 cursor;	/* the next granule of the lane's share of the flat list */
	uint32		end;			/* one past its last */
} pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftMorselLane;

typedef struct SiftMorselQueue
{
	uint32		total;
	uint32		n_parts;
	uint32		n_participants;
	uint32		n_lanes;
	SiftMorselLane lanes[SIFT_MORSEL_LANES] pg_attribute_aligned(PG_CACHE_LINE_SIZE);
	pg_atomic_uint64 topn_theta_bits pg_attribute_aligned(2 * PG_CACHE_LINE_SIZE);
	uint32		part_start[FLEXIBLE_ARRAY_MEMBER];	/* n_parts + 1: the flat index of the first granule of each part, then the total */
} pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftMorselQueue;

/* The alignment of every shared block that holds a queue: its base and the offsets of the queues in it */
#define SIFT_PAR_SHM_ALIGN		(2 * PG_CACHE_LINE_SIZE)

StaticAssertDecl(_Alignof(SiftMorselQueue) == SIFT_PAR_SHM_ALIGN, "a queue is placed at the alignment of its type");
StaticAssertDecl(sizeof(SiftMorselLane) == PG_CACHE_LINE_SIZE && offsetof(SiftMorselQueue, lanes) == PG_CACHE_LINE_SIZE,
				 "every lane of the morsel queue is alone on its cache line");

/* The bytes of the queue of n_parts parts */
extern Size sift_morsel_size(uint32 n_parts);
/* Fills q (sift_morsel_size(n_parts) bytes) for the parts with granules[p] granules, shared by n_participants participants */
extern void sift_morsel_init(SiftMorselQueue *q, uint32 n_parts, const uint32 *granules, uint32 n_participants);
/* The next run of granules for the participant (0 .. n_participants - 1), false when the list is consumed */
extern bool sift_morsel_claim(SiftMorselQueue *q, uint32 participant, SiftMorsel *m);
/* The bound shared by a Top-N tournament: 0 until a participant has K rows; sift_morsel_theta_raise keeps the larger of the stored and the given bits */
extern uint64 sift_morsel_theta_load(SiftMorselQueue *q);
extern void sift_morsel_theta_raise(SiftMorselQueue *q, uint64 bits);
/* Ends the list for every participant: the claims that follow find nothing */
extern void sift_morsel_stop(SiftMorselQueue *q);
/* The run of the next claim of a list with `left` granules left (the guided size) */
extern uint32 sift_morsel_run(uint32 left, uint32 n_participants);

#endif							/* SIFT_AGG_PAR_MORSEL_H */
