/*
 * shm_combiner.h - the lock-free wait-list combiner under SiftL0GroupCommit and SiftManifestCommitCombiner.
 *
 * A writer that finds the queue lock busy parks its request in a slot of a shared pool and pushes the slot on the queue's
 * Treiber stack. The first writer to push (the stack was empty) is the leader: it takes the queue lock, detaches the whole
 * stack in one exchange, hands the requests that belong to its group to apply() as ONE batch, releases the lock and only then
 * wakes the followers. A writer that finds the lock free applies its own request and the followers that arrived meanwhile.
 * apply() runs under the queue lock, writes hdr.result (and hdr.out) of every request of the batch and must not submit.
 */
#ifndef SIFT_SHM_COMBINER_H
#define SIFT_SHM_COMBINER_H

#include "core/shm/port.h"

#define SIFT_COMB_NONE			UINT32_MAX
#define SIFT_COMB_POOL_SLOTS	64
#define SIFT_COMB_ROUNDS		4
#define SIFT_COMB_HOT_EPOCHS	4
#define SIFT_COMB_HOT_STREAK	2

#define SIFT_COMB_IDLE			0U
#define SIFT_COMB_WAITING		1U
#define SIFT_COMB_DONE			2U
#define SIFT_COMB_RETRY			3U

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

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftCombPool
{
	pg_atomic_uint64 free_mask;
} SiftCombPool;

StaticAssertDecl(sizeof(SiftCombPool) == PG_CACHE_LINE_SIZE, "SiftCombPool must occupy exactly one cache line");

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
} SiftCombOps;

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
