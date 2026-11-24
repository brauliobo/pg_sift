/*
 * core/par/morsel.h - the work queue of a parallel scan: the granules of every part of the scan as one flat list, which the participants
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
 * A Top-N tournament over the granules (agg/topn_v6.h) shares what it has learned in topn_theta (a SiftAtomicBound), alone on its own cache line pair: the encoded
 * bound of the K-th best row of the participant that is furthest along (0 = none yet). It only grows (a compare-and-swap that keeps the larger), and every
 * participant reads it before it takes a granule; once a granule cannot hold a row better than the bound, the rest of the list cannot either and
 * sift_morsel_stop ends it for all.
 *
 * The queue is in shared memory, allocated with sift_morsel_size() bytes aligned to a cache line, and has no pointer: it is valid at any address.
 *
 * The same queue hands the items of an operator to the threads of the pool: sift_par_run (below) is the one driver of every parallel operator of a backend. An operator is the functions of a
 * SiftParOps, and every one of them but the two that say so runs on any thread with no call to the server.
 */
#ifndef SIFT_CORE_PAR_MORSEL_H
#define SIFT_CORE_PAR_MORSEL_H

#include "port/atomics.h"

#include "core/par/arena.h"
#include "core/par/topk.h"
#include "core/par/pool.h"

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
	SiftAtomicBound topn_theta pg_attribute_aligned(2 * PG_CACHE_LINE_SIZE);
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

/* ---- the driver of the threads of a backend ---- */

/* The memory of each thread, made by the leader before the run and freed with its context */
typedef struct SiftParArenas
{
	uint32		n;
	struct
	{
		SiftArena	scratch;	/* one morsel's decode: rewound before each */
		SiftArena	state;		/* what phase 1 leaves for phase 2: the partition streams, the table of a thread, its K best */
		SiftArena	work;		/* phase 2: rewound after each partition */
	}			w[SIFT_PAR_MAX_THREADS];
} SiftParArenas;

extern SiftParArenas *sift_par_arenas_create(MemoryContext cx, uint32 nthreads, size_t scratch_bytes, size_t state_bytes, size_t work_bytes);

typedef struct SiftParRange
{
	uint32		lo;
	uint32		hi;
} SiftParRange;

typedef enum SiftParMode
{
	SIFT_PAR_RANGE = 0,			/* the items [0, items) are split in guided morsels that every thread takes */
	SIFT_PAR_RING = 1			/* the leader makes the items one by one into a ring of ring_slots slots (SiftParOps.feed), every thread takes the ones that are made */
} SiftParMode;

/* The ring of a run in RING mode: at most this many slots */
#define SIFT_PAR_RING_MAX		64

/* A run: RANGE: the items [0, items) are split in morsels; RING: the items are what feed makes. Phase 2 has the partitions that seal returns */
typedef struct SiftParCfg
{
	SiftParMode mode;
	uint32		nthreads;		/* 1: every phase runs on the leader, with no wakeup of the pool */
	uint32		items;			/* RANGE: the number of items */
	uint32		ring_slots;		/* RING: the slots, 1 .. SIFT_PAR_RING_MAX (0: twice the threads); item i is in slot i % ring_slots */
} SiftParCfg;

typedef struct SiftParOps
{
	/*
	 * RING, the leader (the server may be called): makes item `item` in its slot, the next after the ones made; false: there are no more. The slot is not touched by the leader after
	 * feed returns until release.
	 */
	bool		(*feed_slot) (void *ctx, uint32 item);
	/* RING, the leader: item is done on every thread, or was replayed: its slot is free, in the order of the items */
	void		(*release_slot) (void *ctx, uint32 item);
	/* RING, the leader (NULL: always): false holds feed_slot back until a release changes the answer; it is true while no item is made and not released */
	bool		(*may_feed) (void *ctx);
	/* RING: release_slot delivers the item (the server is called): a run that fails does not release the items it leaves, which are not done */
	bool		release_emits;
	/*
	 * RING, the leader: phase 1 of the item said SIFT_ERR_ARENA_FULL or SIFT_ERR_SERIAL, so that nothing of it is folded: it is the backend path's, and its slot is still the item's. Runs
	 * before release_slot, in the order of the items.
	 */
	void		(*fallback_item) (void *ctx, uint32 item);
	/*
	 * Thread `worker`: the items [lo, hi) (RING: the item lo). false, with err filled, when it stops before the end: *done is the number of items from lo that are in. SIFT_ERR_WORK_MEM is the
	 * memory of the thread: the others stop, and overflow_drain follows. RING: SIFT_ERR_ARENA_FULL and SIFT_ERR_SERIAL leave the item to fallback_item and the run goes on.
	 */
	bool		(*phase1) (void *ctx, int worker, uint32 lo, uint32 hi, SiftArena *scratch, uint32 *done, SiftErr *err);
	/* The leader, after every thread has left phase 1: the partitions of phase 2 (0: the answer is done). May call the server */
	bool		(*seal) (void *ctx, uint32 *nparts, SiftErr *err);
	/* Thread `worker`: partition `part`; work is rewound after it */
	bool		(*phase2) (void *ctx, int worker, uint32 part, SiftArena *work, SiftErr *err);
	/* The leader, when every phase is done: merges the threads' results and emits them. May call the server */
	void		(*finish) (void *ctx);
	/* The leader, after SIFT_ERR_WORK_MEM: delivers everything that is folded, and the items that are not (left) are for the serial path. May call the server */
	void		(*overflow_drain) (void *ctx, const SiftParRange *left, uint32 nleft);
} SiftParOps;

/*
 * Runs the operator on cfg->nthreads threads: the leader is thread 0. A thread of the pool is started in PG_TRY and waited for in PG_FINALLY, so that an error, a cancel or a statement timeout of
 * the leader leaves none running; every thread reads sift_par_pool_cancelled between its units of work. The first error of a thread is raised on the leader as the backend path raises it
 * (rel: its index, or NULL), except SIFT_ERR_WORK_MEM, which hands the operator to overflow_drain and returns false: the caller goes on with the items in left on the serial path.
 */
extern bool sift_par_run(const SiftParCfg *cfg, const SiftParOps *ops, void *ctx, SiftParArenas *arenas, Relation rel);

#endif							/* SIFT_CORE_PAR_MORSEL_H */
