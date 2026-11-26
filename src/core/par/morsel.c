/*
 * core/par/morsel.c - the guided work queue of core/par/morsel.h.
 */
#include "postgres.h"

#include <pthread.h>
#include <time.h>

#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "core/par/morsel.h"

Size
sift_morsel_size(uint32 n_parts)
{
	return add_size(offsetof(SiftMorselQueue, part_start), mul_size(sizeof(uint32), (Size) n_parts + 1));
}

void
sift_morsel_init(SiftMorselQueue *q, uint32 n_parts, const uint32 *granules, uint32 n_participants)
{
	uint64		total = 0;

	Assert(n_participants >= 1);
	for (uint32 p = 0; p < n_parts; p++)
	{
		q->part_start[p] = (uint32) total;
		total += granules[p];
		if (total > UINT32_MAX)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("too many granules in one parallel scan"),
							errdetail("A parallel scan holds at most %u granules.", UINT32_MAX), errhint("Scan fewer parts at once.")));
	}
	q->part_start[n_parts] = (uint32) total;
	q->total = (uint32) total;
	q->n_parts = n_parts;
	q->n_participants = n_participants;
	q->n_lanes = Min(n_participants, SIFT_MORSEL_LANES);
	sift_bound_init(&q->topn_theta, 0);
	for (uint32 l = 0; l < q->n_lanes; l++)
	{
		pg_atomic_init_u32(&q->lanes[l].cursor, (uint32) (total * l / q->n_lanes));
		q->lanes[l].end = (uint32) (total * (l + 1) / q->n_lanes);
	}
}

uint32
sift_morsel_run(uint32 left, uint32 n_participants)
{
	return pg_prevpower2_32(Max(1, Min(left / (4 * n_participants), SIFT_MORSEL_MAX)));
}

/* The part of the flat granule g: the last part that starts at or before it (the parts with no granule start where the next does) */
static uint32
part_of(const SiftMorselQueue *q, uint32 g)
{
	uint32		lo = 0,
				hi = q->n_parts;

	while (hi - lo > 1)
	{
		const uint32 mid = lo + (hi - lo) / 2;

		if (q->part_start[mid] <= g)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

/* A run from the lane: guided by the granules the lane has left, shared by the participants that read it */
static bool
lane_claim(SiftMorselQueue *q, SiftMorselLane *lane, uint32 sharers, SiftMorsel *m)
{
	uint32		cur = pg_atomic_read_u32(&lane->cursor);

	while (cur < lane->end)
	{
		const uint32 part = part_of(q, cur);
		const uint32 run = Min(sift_morsel_run(lane->end - cur, sharers), q->part_start[part + 1] - cur);

		if (pg_atomic_compare_exchange_u32(&lane->cursor, &cur, cur + run))
		{
			m->part = part;
			m->first = cur - q->part_start[part];
			m->n = run;
			return true;
		}
	}
	return false;
}

/* The lane with the most granules left, NULL when every lane is consumed */
static SiftMorselLane *
fullest_lane(SiftMorselQueue *q)
{
	SiftMorselLane *best = NULL;
	uint32		best_left = 0;

	for (uint32 l = 0; l < q->n_lanes; l++)
	{
		const uint32 cur = pg_atomic_read_u32(&q->lanes[l].cursor);
		const uint32 left = cur < q->lanes[l].end ? q->lanes[l].end - cur : 0;

		if (left > best_left)
		{
			best = &q->lanes[l];
			best_left = left;
		}
	}
	return best;
}

bool
sift_morsel_claim(SiftMorselQueue *q, uint32 participant, SiftMorsel *m)
{
	SiftMorselLane *lane = &q->lanes[participant % q->n_lanes];
	uint32		sharers = Max(1U, (q->n_participants + q->n_lanes - 1) / q->n_lanes);

	while (lane)
	{
		if (lane_claim(q, lane, sharers, m))
			return true;
		lane = fullest_lane(q);
		sharers = q->n_participants;
	}
	return false;
}

uint64
sift_morsel_theta_load(SiftMorselQueue *q)
{
	return sift_bound_load(&q->topn_theta);
}

void
sift_morsel_theta_raise(SiftMorselQueue *q, uint64 bits)
{
	sift_bound_raise(&q->topn_theta, bits);
}

void
sift_morsel_stop(SiftMorselQueue *q)
{
	for (uint32 l = 0; l < q->n_lanes; l++)
		pg_atomic_write_u32(&q->lanes[l].cursor, q->lanes[l].end);
}

/* ---- the driver ---- */

SiftParArenas *
sift_par_arenas_create(MemoryContext cx, uint32 nthreads, size_t scratch_bytes, size_t state_bytes, size_t work_bytes)
{
	SiftParArenas *ar = MemoryContextAllocZero(cx, sizeof(SiftParArenas));
	const size_t sz[3] = {TYPEALIGN(SIFT_ARENA_ALIGN, scratch_bytes), TYPEALIGN(SIFT_ARENA_ALIGN, state_bytes), TYPEALIGN(SIFT_ARENA_ALIGN, work_bytes)};

	Assert(nthreads >= 1 && nthreads <= SIFT_PAR_MAX_THREADS);
	ar->n = nthreads;
	for (uint32 t = 0; t < nthreads; t++)
	{
		uint8	   *slab = MemoryContextAllocAligned(cx, sz[0] + sz[1] + sz[2], SIFT_ARENA_ALIGN, MCXT_ALLOC_HUGE);

		sift_arena_init(&ar->w[t].scratch, slab, sz[0]);
		sift_arena_init(&ar->w[t].state, slab + sz[0], sz[1]);
		sift_arena_init(&ar->w[t].work, slab + sz[0] + sz[1], sz[2]);
	}
	return ar;
}

#define RING_POLL_NS	50000000	/* how long the leader waits for a thread before it looks at the interrupts */

/* What a slot of the ring holds, by the item in it */
enum
{
	RING_RUN = 0,
	RING_DONE,
	RING_REPLAY,
	RING_FAILED
};

/* The items made by the leader and taken by the threads: all counters and states are under mu (an item is a granule of 8192 rows: a few thousand a second, not a hot lock) */
typedef struct SiftRing
{
	pthread_mutex_t mu;
	pthread_cond_t cv;
	uint32		slots;
	uint32		published;		/* the items made */
	uint32		claimed;		/* the items a thread has taken */
	uint32		released;		/* the items whose slot is free again */
	bool		eof;
	uint8		state[SIFT_PAR_RING_MAX];
} SiftRing;

typedef struct SiftParRun
{
	const SiftParOps *ops;
	void	   *ctx;
	SiftParArenas *ar;
	SiftMorselQueue *q;
	SiftRing	ring;
	SiftParMode mode;
	bool		completed;		/* run_phases returned: the error, if any, is one of the latch */
	int			nthreads;
	SiftErrLatch lt;
	atomic_bool halt;
	atomic_uint next_part;
	uint32		nparts;
	SiftParRange left[SIFT_PAR_MAX_THREADS];
} SiftParRun;

static bool
halted(const SiftParRun *r)
{
	return atomic_load_explicit(&r->halt, memory_order_relaxed) || sift_errlatch_peek(&r->lt);
}

static void
run_halt(SiftParRun *r)
{
	atomic_store_explicit(&r->halt, true, memory_order_relaxed);
	if (r->mode == SIFT_PAR_RING && r->nthreads > 1)
	{
		pthread_mutex_lock(&r->ring.mu);
		pthread_cond_broadcast(&r->ring.cv);
		pthread_mutex_unlock(&r->ring.mu);
	}
}

static void
fail(SiftParRun *r, const SiftErr *err)
{
	sift_errlatch_post(&r->lt, err);
	run_halt(r);
}

static void
phase1_loop(SiftParRun *r, int t)
{
	SiftArena  *scratch = &r->ar->w[t].scratch;
	SiftMorsel	m;

	while (!halted(r) && sift_morsel_claim(r->q, t, &m))
	{
		uint32		done = 0;
		SiftErr		err = {0};

		sift_arena_reset(scratch);
		if (!r->ops->phase1(r->ctx, t, m.first, m.first + m.n, scratch, &done, &err))
		{
			r->left[t] = (SiftParRange) {m.first + done, m.first + m.n};
			fail(r, &err);
			return;
		}
		if (t == 0)
			CHECK_FOR_INTERRUPTS();
	}
}

/* ---- the ring ---- */

/* Thread t takes item: what it says of it is the state of the slot */
static uint8
ring_item(SiftParRun *r, int t, uint32 item)
{
	SiftArena  *scratch = &r->ar->w[t].scratch;
	SiftErr		err = {0};
	uint32		done = 0;

	sift_arena_reset(scratch);
	if (r->ops->phase1(r->ctx, t, item, item + 1, scratch, &done, &err))
		return RING_DONE;
	if (err.code == SIFT_ERR_ARENA_FULL || err.code == SIFT_ERR_SERIAL)
		return RING_REPLAY;
	fail(r, &err);
	return RING_FAILED;
}

/* An item of the ring is the leader's alone to pass on, when it is replayed on the backend path or done */
static void
ring_settle_item(SiftParRun *r, uint32 item, bool replay)
{
	if (replay && r->ops->fallback_item != NULL)
		r->ops->fallback_item(r->ctx, item);
	r->ops->release_slot(r->ctx, item);
}

/* A helper thread: takes the items as they are made, until the leader says there are no more */
static void
ring_helper(SiftParRun *r, int t)
{
	SiftRing   *g = &r->ring;

	pthread_mutex_lock(&g->mu);
	while (!halted(r))
	{
		if (g->claimed < g->published)
		{
			const uint32 item = g->claimed++;
			uint8		st;

			pthread_mutex_unlock(&g->mu);
			st = ring_item(r, t, item);
			pthread_mutex_lock(&g->mu);
			g->state[item % g->slots] = st;
			pthread_cond_broadcast(&g->cv);
		}
		else if (g->eof)
			break;
		else
			pthread_cond_wait(&g->cv, &g->mu);
	}
	pthread_mutex_unlock(&g->mu);
}

/* The slots at the head of the ring whose items are done, passed on in the order of the items (the lock is held on entry and on return) */
static void
ring_reclaim(SiftParRun *r)
{
	SiftRing   *g = &r->ring;

	while (g->released < g->published && g->state[g->released % g->slots] != RING_RUN && g->state[g->released % g->slots] != RING_FAILED)
	{
		const uint32 item = g->released;
		const uint8 st = g->state[item % g->slots];

		pthread_mutex_unlock(&g->mu);
		ring_settle_item(r, item, st == RING_REPLAY);
		pthread_mutex_lock(&g->mu);
		g->released++;
	}
}

/* The operator's say on making another item, asked without the lock of the ring: what it does may leave by an error, and the threads are stopped through that lock then (the lock is held on entry and on return) */
static bool
ring_may_feed(SiftParRun *r)
{
	SiftRing   *g = &r->ring;
	bool		may;

	if (r->ops->may_feed == NULL)
		return true;
	pthread_mutex_unlock(&g->mu);
	may = r->ops->may_feed(r->ctx);
	pthread_mutex_lock(&g->mu);
	return may;
}

/* The leader waits for a thread to settle an item, and takes the interrupts meanwhile: a cancel is a flag the backend sets, it wakes nobody (the lock is held on entry and on return) */
static void
ring_wait(SiftRing *g)
{
	struct timespec until;

	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_nsec += RING_POLL_NS;
	until.tv_sec += until.tv_nsec / 1000000000;
	until.tv_nsec %= 1000000000;
	pthread_cond_timedwait(&g->cv, &g->mu, &until);
	if (InterruptPending)
	{
		pthread_mutex_unlock(&g->mu);
		CHECK_FOR_INTERRUPTS();
		pthread_mutex_lock(&g->mu);
	}
}

/* Thread 0: makes the items while there is room, takes one when the ring is full, passes the slots on; never waits while it has something to do */
static void
ring_leader(SiftParRun *r)
{
	SiftRing   *g = &r->ring;

	pthread_mutex_lock(&g->mu);
	while (!halted(r))
	{
		ring_reclaim(r);
		if (!g->eof && g->published - g->released < g->slots && ring_may_feed(r))
		{
			bool		made;

			pthread_mutex_unlock(&g->mu);
			CHECK_FOR_INTERRUPTS();
			made = r->ops->feed_slot(r->ctx, g->published);
			pthread_mutex_lock(&g->mu);
			g->state[g->published % g->slots] = RING_RUN;
			g->published += made;
			g->eof = !made;
			pthread_cond_broadcast(&g->cv);
		}
		else if (g->claimed < g->published)
		{
			const uint32 item = g->claimed++;
			uint8		st;

			pthread_mutex_unlock(&g->mu);
			st = ring_item(r, 0, item);
			pthread_mutex_lock(&g->mu);
			g->state[item % g->slots] = st;
		}
		else if (g->eof && g->released == g->published)
			break;
		else
			ring_wait(g);
	}
	pthread_mutex_unlock(&g->mu);
}

/* One thread: the leader makes an item, runs it, passes its slot on */
static void
ring_inline(SiftParRun *r)
{
	for (uint32 item = 0; !halted(r); item++)
	{
		uint8		st;

		CHECK_FOR_INTERRUPTS();
		if (!r->ops->feed_slot(r->ctx, item))
			break;
		st = ring_item(r, 0, item);
		r->ring.published = item + 1;
		r->ring.state[item % r->ring.slots] = st;
		if (st == RING_FAILED)
			break;
		ring_settle_item(r, item, st == RING_REPLAY);
		r->ring.released = item + 1;
	}
}

static void
phase1_ring(SiftParRun *r, int t)
{
	if (r->nthreads == 1)
		ring_inline(r);
	else if (t == 0)
		ring_leader(r);
	else
		ring_helper(r, t);
}

/* What the ring holds when the run ends before its items did: the leader passes it on (the items not folded are replayed when the run goes on on the backend path) */
static void
ring_close(SiftParRun *r, bool replay)
{
	SiftRing   *g = &r->ring;

	if (r->ops->release_emits && (!r->completed || sift_errlatch_peek(&r->lt)))
		return;
	for (; g->released < g->published; g->released++)
		ring_settle_item(r, g->released, replay && g->state[g->released % g->slots] != RING_DONE);
}

static void
phase2_loop(SiftParRun *r, int t)
{
	SiftArena  *work = &r->ar->w[t].work;

	while (!halted(r))
	{
		const uint32 part = atomic_fetch_add_explicit(&r->next_part, 1, memory_order_relaxed);
		const SiftArenaMark mark = sift_arena_mark(work);
		SiftErr		err = {0};
		bool		ok;

		if (part >= r->nparts)
			return;
		ok = r->ops->phase2(r->ctx, t, part, work, &err);
		sift_arena_rewind(work, mark);
		if (!ok)
			return fail(r, &err);
		if (t == 0)
			CHECK_FOR_INTERRUPTS();
	}
}

static void
job_phase1(int worker, void *r)
{
	if (((SiftParRun *) r)->mode == SIFT_PAR_RING)
		phase1_ring(r, worker);
	else
		phase1_loop(r, worker);
}

static void
job_phase2(int worker, void *r)
{
	phase2_loop(r, worker);
}

static void
job_stop(void *r)
{
	run_halt(r);
}

static void
run_phase(SiftParRun *r, SiftParJobFn job, void (*loop) (SiftParRun *r, int t))
{
	if (r->nthreads > 1)
		sift_par_pool_start(r->nthreads, job, job_stop, r);
	loop(r, 0);
	if (r->nthreads > 1)
		sift_par_pool_wait();
}

/* What the threads did not fold when they stopped: the rest of the morsels they failed in, and what no thread claimed */
static uint32
leftover(const SiftParRun *r, SiftParRange *out)
{
	uint32		n = 0;

	if (r->mode == SIFT_PAR_RING)
		return 0;
	for (int t = 0; t < r->nthreads; t++)
		if (r->left[t].lo < r->left[t].hi)
			out[n++] = r->left[t];
	for (uint32 l = 0; l < r->q->n_lanes; l++)
	{
		const uint32 cur = pg_atomic_read_u32(&r->q->lanes[l].cursor);

		if (cur < r->q->lanes[l].end)
			out[n++] = (SiftParRange) {cur, r->q->lanes[l].end};
	}
	return n;
}

static void
run_phases(SiftParRun *r)
{
	SiftErr		err = {0};

	run_phase(r, job_phase1, r->mode == SIFT_PAR_RING ? phase1_ring : phase1_loop);
	if (halted(r))
		return;
	if (r->ops->seal != NULL && !r->ops->seal(r->ctx, &r->nparts, &err))
		return fail(r, &err);
	if (r->nparts > 0)
		run_phase(r, job_phase2, phase2_loop);
}

/* Whether the run stopped for memory, and the operator drains what it folded */
static bool
overflowed(SiftParRun *r, SiftErr *err)
{
	return sift_errlatch_get(&r->lt, err) && err->code == SIFT_ERR_WORK_MEM && r->ops->overflow_drain != NULL;
}

bool
sift_par_run(const SiftParCfg *cfg, const SiftParOps *ops, void *ctx, SiftParArenas *arenas, Relation rel)
{
	SiftParRun	r = {.ops = ops,.ctx = ctx,.ar = arenas,.mode = cfg->mode,.nthreads = cfg->nthreads};
	SiftErr		err;

	Assert(cfg->nthreads >= 1 && cfg->nthreads <= arenas->n);
	sift_errlatch_init(&r.lt);
	atomic_init(&r.halt, false);
	atomic_init(&r.next_part, 0);
	if (cfg->mode == SIFT_PAR_RING)
	{
		Assert(ops->feed_slot != NULL && ops->release_slot != NULL && cfg->ring_slots <= SIFT_PAR_RING_MAX);
		r.ring.slots = cfg->ring_slots > 0 ? cfg->ring_slots : Min(2 * cfg->nthreads, SIFT_PAR_RING_MAX);
		pthread_mutex_init(&r.ring.mu, NULL);
		pthread_cond_init(&r.ring.cv, NULL);
	}
	else
	{
		r.q = MemoryContextAllocAligned(CurrentMemoryContext, sift_morsel_size(1), SIFT_PAR_SHM_ALIGN, 0);
		sift_morsel_init(r.q, 1, &cfg->items, cfg->nthreads);
	}
	PG_TRY();
	{
		run_phases(&r);
		r.completed = true;
	}
	PG_FINALLY();
	{
		if (r.nthreads > 1 && sift_par_pool_busy())
		{
			job_stop(&r);
			sift_par_pool_wait();
		}
		if (r.mode == SIFT_PAR_RING && !(r.completed && overflowed(&r, &err)))
			ring_close(&r, false);
		if (r.mode == SIFT_PAR_RING)
		{
			pthread_cond_destroy(&r.ring.cv);
			pthread_mutex_destroy(&r.ring.mu);
		}
	}
	PG_END_TRY();
	if (sift_errlatch_get(&r.lt, &err))
	{
		SiftParRange left[SIFT_PAR_MAX_THREADS + SIFT_MORSEL_LANES];

		if (err.code != SIFT_ERR_WORK_MEM || ops->overflow_drain == NULL)
			sift_err_rethrow(rel, &err);
		if (r.mode == SIFT_PAR_RING)
			ring_close(&r, true);
		ops->overflow_drain(ctx, left, leftover(&r, left));
		if (r.q != NULL)
			pfree(r.q);
		return false;
	}
	if (ops->finish != NULL)
		ops->finish(ctx);
	if (r.q != NULL)
		pfree(r.q);
	return true;
}
