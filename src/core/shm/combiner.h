/*
 * shm_combiner.h - the lock-free wait-list combiner under SiftL0GroupCommit and SiftManifestCommitCombiner.
 *
 * A writer that finds the queue lock busy parks its request in a slot of a shared pool and pushes the slot on the queue's
 * Treiber stack. The first writer to push (the stack was empty) is the leader: it takes the queue lock, detaches the whole
 * stack in one exchange, hands the requests that belong to its group to apply() in batches of at most max_batch, releases the lock
 * and only then wakes the followers. A writer that finds the lock free applies its own request and the followers that arrived meanwhile.
 * The requests of other groups (apply() runs with the leader's context) go back on the stack, oldest at the bottom, and where they
 * are all that is on it the oldest is woken to lead them (SIFT_COMB_LEAD): one wake-up, and they stay one batch.
 * apply() runs under the queue lock, writes hdr.result (and hdr.out) of every request of the batch and must not submit.
 */
#ifndef SIFT_SHM_COMBINER_H
#define SIFT_SHM_COMBINER_H

#include "core/shm/port.h"

#define SIFT_COMB_NONE			UINT32_MAX
#define SIFT_COMB_POOL_SLOTS	64		/* the slots of one shard of the pool */
#define SIFT_COMB_SHARDS		4
#define SIFT_COMB_MAX_SLOTS		(SIFT_COMB_POOL_SLOTS * SIFT_COMB_SHARDS)
#define SIFT_COMB_ROUNDS		4
#define SIFT_COMB_HOT_EPOCHS	4
#define SIFT_COMB_HOT_STREAK	2

#define SIFT_COMB_IDLE			0U
#define SIFT_COMB_WAITING		1U
#define SIFT_COMB_DONE			2U
#define SIFT_COMB_RETRY			3U	/* the leader failed or exited before it applied the request: its writer parks it again */
#define SIFT_COMB_LEAD			4U	/* the request is at the bottom of the stack again: its writer leads it */

/* First member of every request slot */
typedef struct SiftCombHdr
{
	pg_atomic_uint32 state;
	uint32		next;
	int32		procno;
	uint32		result;			/* written by apply() */
	uint64		out;			/* written by apply() */
} SiftCombHdr;

StaticAssertDecl(sizeof(SiftCombHdr) == 24, "SiftCombHdr must stay 24 bytes");

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftCombQueue
{
	SiftShmLock lock;
	pg_atomic_uint32 wait_head;
	pg_atomic_uint32 contend_streak;
	pg_atomic_uint32 hot_epoch;
} SiftCombQueue;

StaticAssertDecl(sizeof(SiftCombQueue) == PG_CACHE_LINE_SIZE, "SiftCombQueue must occupy exactly one cache line");

/* The free slots of the pool, a word of SIFT_COMB_POOL_SLOTS for each shard on a line of its own: a backend takes from the shard of its own number first, so that the writers do not all retry on one line */
typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftCombShard
{
	pg_atomic_uint64 free_mask;
} SiftCombShard;

typedef struct SiftCombPool
{
	SiftCombShard shard[SIFT_COMB_SHARDS];
} SiftCombPool;

StaticAssertDecl(sizeof(SiftCombShard) == PG_CACHE_LINE_SIZE, "a shard of the pool occupies exactly one cache line");

typedef struct SiftCombArena
{
	SiftCombPool *pool;
	char	   *slots;
	Size		stride;
} SiftCombArena;

typedef struct SiftCombOps
{
	bool		(*same_group) (const SiftCombHdr *lead, const SiftCombHdr *other);
	void		(*apply) (void *ctx, SiftCombHdr *const *batch, uint32 n);
	uint32		max_batch;
	uint32		gather_spins;	/* where the queue is hot, the leader waits for this many pauses for the writers that find the lock busy to join its batch (0: it does not) */
} SiftCombOps;

/* A pool of SIFT_COMB_MAX_SLOTS slots (the arena holds that many) */
extern void sift_comb_pool_init(SiftCombPool *pool);
extern void sift_comb_queue_init(SiftCombQueue *q, int tranche_id);

/* Runs req (a caller-local slot of arena->stride bytes whose first `used` bytes are the request) to completion: req->result and req->out are set */
extern void sift_comb_submit(const SiftCombArena *arena, SiftCombQueue *q, const SiftCombOps *ops, void *ctx,
							 SiftCombHdr *req, Size used);

static inline bool
sift_comb_queue_hot(SiftCombQueue *q)
{
	return pg_atomic_read_u32(&q->contend_streak) >= SIFT_COMB_HOT_STREAK || pg_atomic_read_u32(&q->hot_epoch) > 0;
}

/* One slab flush of the queue's work went by: a queue that stayed quiet cools down by one epoch */
static inline void
sift_comb_queue_cool(SiftCombQueue *q)
{
	uint32		epoch = pg_atomic_read_u32(&q->hot_epoch);

	if (pg_atomic_read_u32(&q->contend_streak) < SIFT_COMB_HOT_STREAK && epoch > 0)
		pg_atomic_write_u32(&q->hot_epoch, epoch - 1);
}

#endif							/* SIFT_SHM_COMBINER_H */
