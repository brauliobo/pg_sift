/*
 * shm_combiner.c - the combiner protocol of shm_combiner.h.
 */
#include "core/shm/combiner.h"

/* What a leader detached and owes a state, from the first detach to its resolve (n is 0 outside of it): a backend that exits in between resolves it on its way out */
typedef struct CombLead
{
	SiftCombArena arena;
	uint32		own;
	uint32		n;
	uint32		idx[SIFT_COMB_MAX_SLOTS];
	uint32		fin[SIFT_COMB_MAX_SLOTS];
} CombLead;

SIFT_SHM_BACKEND_LOCAL CombLead lead_state;
SIFT_SHM_BACKEND_LOCAL bool exit_hooked;

/* MEM[MEM-CORE-15]: SHM flat-combiner queue state (caller-owned SHM region) */
static void
pool_init(SiftCombPool *pool, uint32 nshards)
{
	Assert((uintptr_t) pool % PG_CACHE_LINE_SIZE == 0);
	pool->nshards = nshards;
	for (uint32 s = 0; s < SIFT_COMB_SHARDS; s++)
		pg_atomic_init_u64(&pool->free_mask[s], s < nshards ? UINT64_MAX : 0);
}

void
sift_comb_pool_init(SiftCombPool *pool)
{
	pool_init(pool, 1);
}

void
sift_comb_pool_init_sharded(SiftCombPool *pool)
{
	pool_init(pool, SIFT_COMB_SHARDS);
}

void
sift_comb_queue_init(SiftCombQueue *q, int tranche_id)
{
	sift_shm_lock_init(&q->lock, tranche_id);
	pg_atomic_init_u32(&q->wait_head, SIFT_COMB_NONE);
	pg_atomic_init_u32(&q->contend_streak, 0);
	pg_atomic_init_u32(&q->hot_epoch, 0);
}

static inline SiftCombHdr *
slot_at(const SiftCombArena *a, uint32 idx)
{
	return (SiftCombHdr *) (a->slots + (Size) idx * a->stride);
}

static int32
pool_claim(SiftCombPool *pool)
{
	uint32		home = (uint32) sift_shm_my_procno() % pool->nshards;

	for (uint32 i = 0; i < pool->nshards; i++)
	{
		uint32		shard = (home + i) % pool->nshards;
		uint64		mask = pg_atomic_read_u64(&pool->free_mask[shard]);

		while (mask != 0)
		{
			int			pos = pg_rightmost_one_pos64(mask);

			if (pg_atomic_compare_exchange_u64(&pool->free_mask[shard], &mask, mask & ~(UINT64CONST(1) << pos)))
				return shard * SIFT_COMB_POOL_SLOTS + pos;
		}
	}
	return -1;
}

static void
pool_release(SiftCombPool *pool, uint32 idx)
{
	pg_atomic_fetch_or_u64(&pool->free_mask[idx / SIFT_COMB_POOL_SLOTS], UINT64CONST(1) << (idx % SIFT_COMB_POOL_SLOTS));
}

static uint32
push(SiftCombQueue *q, SiftCombHdr *slot, uint32 idx)
{
	uint32		head = pg_atomic_read_u32(&q->wait_head);

	do
		slot->next = head;
	while (!pg_atomic_compare_exchange_u32(&q->wait_head, &head, idx));
	return head;
}

/* The stack as one exchange, appended to the leader's list oldest first */
static void
detach(const SiftCombArena *a, SiftCombQueue *q, CombLead *ld)
{
	uint32		lifo[SIFT_COMB_MAX_SLOTS];
	uint32		n = 0;

	for (uint32 i = pg_atomic_exchange_u32(&q->wait_head, SIFT_COMB_NONE); i != SIFT_COMB_NONE; i = slot_at(a, i)->next)
		lifo[n++] = i;
	while (n > 0)
	{
		ld->idx[ld->n] = lifo[--n];
		ld->fin[ld->n++] = SIFT_COMB_RETRY;
	}
}

static void
resolve(const SiftCombArena *a, CombLead *ld, uint32 own_idx)
{
	for (uint32 i = 0; i < ld->n; i++)
	{
		SiftCombHdr *hdr = slot_at(a, ld->idx[i]);
		int32		procno = hdr->procno;

		pg_write_barrier();
		pg_atomic_write_u32(&hdr->state, ld->fin[i]);
		if (ld->idx[i] != own_idx)
			sift_shm_wake(procno);
	}
	ld->n = 0;
}

/* A hot queue gives the writers that are about to commit the time of a few pauses to join the batch; a window nobody joined cools the queue, so that a writer alone pays nothing for it */
static void
gather(SiftCombQueue *q, const SiftCombOps *ops)
{
	if (ops->gather_spins == 0 || !sift_comb_queue_hot(q))
		return;
	for (uint32 i = 0; i < ops->gather_spins; i++)
		pg_spin_delay();
	if (pg_atomic_read_u32(&q->wait_head) == SIFT_COMB_NONE)
		sift_comb_queue_cool(q);
}

static void
leader_rounds(const SiftCombArena *a, SiftCombQueue *q, const SiftCombOps *ops, void *ctx, SiftCombHdr *key,
			bool key_local)
{
	CombLead   *ld = &lead_state;

	for (uint32 round = 0; round < SIFT_COMB_ROUNDS; round++)
	{
		SiftCombHdr *batch[SIFT_COMB_MAX_SLOTS + 1];
		uint32		sel[SIFT_COMB_MAX_SLOTS + 1];
		uint32		first = ld->n;
		uint32		nb = 0;

		if (round == 0 && key_local)
			batch[nb++] = key;
		if (round == 0)
			gather(q, ops);
		detach(a, q, ld);
		for (uint32 i = first; i < ld->n; i++)
		{
			SiftCombHdr *s = slot_at(a, ld->idx[i]);

			if (nb < ops->max_batch && ops->same_group(key, s))
			{
				sel[nb] = i;
				batch[nb++] = s;
			}
		}
		if (nb > 0)
			ops->apply(ctx, batch, nb);
		for (uint32 b = key_local && round == 0 ? 1 : 0; b < nb; b++)
			ld->fin[sel[b]] = SIFT_COMB_DONE;
		if (pg_atomic_read_u32(&q->wait_head) == SIFT_COMB_NONE)
			break;
	}
}

static void
wait_slot(SiftCombHdr *slot)
{
	bool		waited = false;

	SIFT_SHM_HOLD_INTERRUPTS();
	while (pg_atomic_read_u32(&slot->state) == SIFT_COMB_WAITING)
	{
		sift_shm_wait();
		waited = true;
	}
	pg_read_barrier();
	SIFT_SHM_RESUME_INTERRUPTS();
	if (waited)
		sift_shm_wait_done();
}

/* A backend that exits (FATAL) while it leads gives the followers it detached the state it has for them: RETRY where it did not apply them */
static void
lead_exit(int code, Datum arg)
{
	if (lead_state.n > 0)
		resolve(&lead_state.arena, &lead_state, lead_state.own);
}

/*
 * The queue lock is held; it is released before the followers are woken. A parked leader (key_idx its slot) whose apply() fails keeps its slot until it is resolved: an earlier leader may
 * have detached it and write its state still
 */
static void
run_leader(const SiftCombArena *a, SiftCombQueue *q, const SiftCombOps *ops, void *ctx, SiftCombHdr *key, uint32 key_idx)
{
	CombLead   *ld = &lead_state;
	bool		key_local = key_idx == SIFT_COMB_NONE;

	Assert(ops->max_batch >= 1 && ops->max_batch <= SIFT_COMB_MAX_SLOTS);
	ld->n = 0;
	ld->arena = *a;
	ld->own = key_idx;
	SIFT_SHM_TRY();
	{
		leader_rounds(a, q, ops, ctx, key, key_local);
	}
	SIFT_SHM_CATCH();
	{
		sift_shm_unlock(&q->lock);
		resolve(a, ld, key_idx);
		if (!key_local)
		{
			wait_slot(key);
			pool_release(a->pool, key_idx);
		}
		SIFT_SHM_RETHROW();
	}
	SIFT_SHM_END_TRY();
	sift_shm_unlock(&q->lock);
	resolve(a, ld, key_idx);
}

/* A queue that is hot stays so: its counters are read first and written only when they move, so that the writers that find it contended do not take turns on its line */
static void
note_contention(SiftCombQueue *q)
{
	if (pg_atomic_read_u32(&q->contend_streak) < SIFT_COMB_HOT_STREAK)
		(void) pg_atomic_add_fetch_u32(&q->contend_streak, 1);
	if (pg_atomic_read_u32(&q->contend_streak) >= SIFT_COMB_HOT_STREAK && pg_atomic_read_u32(&q->hot_epoch) != SIFT_COMB_HOT_EPOCHS)
		pg_atomic_write_u32(&q->hot_epoch, SIFT_COMB_HOT_EPOCHS);
}

static void
leader_locked(const SiftCombArena *a, SiftCombQueue *q, const SiftCombOps *ops, void *ctx, SiftCombHdr *req)
{
	run_leader(a, q, ops, ctx, req, SIFT_COMB_NONE);
}

static void
leader_parked(const SiftCombArena *a, SiftCombQueue *q, const SiftCombOps *ops, void *ctx, SiftCombHdr *slot, uint32 idx)
{
	sift_shm_lock(&q->lock);
	if (pg_atomic_read_u32(&slot->state) != SIFT_COMB_WAITING)
	{
		sift_shm_unlock(&q->lock);
		return;
	}
	run_leader(a, q, ops, ctx, slot, idx);
	/* an earlier lock holder may have detached this slot and not resolved it yet: it does so without the lock */
	wait_slot(slot);
}

static void
park(const SiftCombArena *a, SiftCombQueue *q, const SiftCombOps *ops, void *ctx, SiftCombHdr *req, Size used,
	 uint32 idx)
{
	SiftCombHdr *slot = slot_at(a, idx);

	memcpy(slot, req, used);
	slot->procno = sift_shm_my_procno();
	slot->result = 0;
	slot->out = 0;
	pg_atomic_write_u32(&slot->state, SIFT_COMB_WAITING);
	pg_write_barrier();
	if (push(q, slot, idx) == SIFT_COMB_NONE)
		leader_parked(a, q, ops, ctx, slot, idx);
	else
		wait_slot(slot);
}

void
sift_comb_submit(const SiftCombArena *a, SiftCombQueue *q, const SiftCombOps *ops, void *ctx, SiftCombHdr *req,
				 Size used)
{
	int32		idx;
	SiftCombHdr *slot;
	uint32		state;

	if (!exit_hooked)
	{
		sift_shm_before_exit(lead_exit);
		exit_hooked = true;
	}
	if (sift_shm_lock_try(&q->lock))
	{
		if (pg_atomic_read_u32(&q->contend_streak) != 0)
			pg_atomic_write_u32(&q->contend_streak, 0);
		leader_locked(a, q, ops, ctx, req);
		return;
	}
	note_contention(q);
	if ((idx = pool_claim(a->pool)) < 0)
	{
		sift_shm_lock(&q->lock);
		leader_locked(a, q, ops, ctx, req);
		return;
	}
	park(a, q, ops, ctx, req, used, idx);
	slot = slot_at(a, idx);
	state = pg_atomic_read_u32(&slot->state);
	Assert(state == SIFT_COMB_DONE || state == SIFT_COMB_RETRY);
	req->result = slot->result;
	req->out = slot->out;
	pool_release(a->pool, idx);
	if (state == SIFT_COMB_RETRY)
	{
		sift_shm_lock(&q->lock);
		leader_locked(a, q, ops, ctx, req);
	}
}
