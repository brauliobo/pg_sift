/*
 * core/par/ring.c - core/par/morsel.h in RING mode: the leader makes the items into a ring of slots, the threads take them, the leader passes the slots on in order and replays on its own
 * what a thread leaves to it (SIFT_ERR_ARENA_FULL, SIFT_ERR_SERIAL), on 1 to 4 threads that the trap shim flags as workers (no palloc, no ereport).
 */
#include "postgres.h"

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include "core/hash.h"
#include "core/par/morsel.h"
#include "core/par/pool.h"
#include "miscadmin.h"
#include "agg/par/host.h"
#include "test/common/pgshim.h"
#include "test/common/check.h"

#define ITEMS		6000

typedef struct RingCtx
{
	struct RingCtx *inner;		/* a run the leader makes from feed_slot, of the item inner_at */
	uint32		inner_at;
	uint32		cancel_in_may_feed_at;	/* the fed item at which may_feed is interrupted */
	atomic_int	hang;			/* the first item a helper takes does not end until hang_release */
	atomic_int	hang_taken;
	atomic_int	hang_release;
	pthread_t	leader;
	uint32		slots;
	uint32		nitems;
	uint64		slot_val[SIFT_PAR_RING_MAX];
	int64		slot_owner[SIFT_PAR_RING_MAX];	/* the item in the slot, -1: free */
	atomic_uint folded[ITEMS];
	atomic_uint replayed[ITEMS];
	atomic_uint released[ITEMS];
	atomic_uint ran[SIFT_PAR_MAX_THREADS];
	uint32		fed;
	uint32		leave_every;	/* the item that is a multiple of it is left to the leader, alternately as ARENA_FULL and SERIAL */
	uint32		fail_at;
	SiftErrCode fail_code;
	uint32		cancel_at;
	bool		finished;
	bool		drained;
	uint64		sum;			/* what the threads folded and the leader replayed */
	atomic_ullong tsum;
	uint32		last_released;
	uint32		nreleased;
	uint32		max_inflight;	/* the most items made and not passed on, seen as one is made */
} RingCtx;

static uint64
value(uint32 item)
{
	return sift_fmix64(item + 1) >> 8;
}

static void
on_leader(const RingCtx *c)
{
	SIFT_CHECK(pthread_equal(pthread_self(), c->leader));
}

static void ring_nested(RingCtx *inner);

static bool
ring_feed(void *vc, uint32 item)
{
	RingCtx    *c = vc;
	const uint32 s = item % c->slots;

	on_leader(c);
	if (c->inner != NULL && item == c->inner_at)
		ring_nested(c->inner);
	SIFT_CHECK(item == c->fed && c->slot_owner[s] == -1);
	if (item == c->nitems)
		return false;
	c->max_inflight = Max(c->max_inflight, item - c->nreleased + 1);
	c->slot_owner[s] = item;
	c->slot_val[s] = value(item);
	c->fed = item + 1;
	if (item + 1 == c->cancel_at)
		InterruptPending = QueryCancelPending = true;
	return true;
}

static void
ring_release(void *vc, uint32 item)
{
	RingCtx    *c = vc;
	const uint32 s = item % c->slots;

	on_leader(c);
	SIFT_CHECK(c->slot_owner[s] == item && (item == 0 ? c->last_released == 0 : c->last_released + 1 == item));
	c->slot_owner[s] = -1;
	c->last_released = item;
	c->nreleased++;
	atomic_fetch_add(&c->released[item], 1);
}

static void
ring_replay(void *vc, uint32 item)
{
	RingCtx    *c = vc;
	const uint32 s = item % c->slots;

	on_leader(c);
	SIFT_CHECK(c->slot_owner[s] == item);
	atomic_fetch_add(&c->replayed[item], 1);
	c->sum += c->slot_val[s];
}

static bool
ring_phase1(void *vc, int t, uint32 lo, uint32 hi, SiftArena *scratch, uint32 *done, SiftErr *err)
{
	RingCtx    *c = vc;
	const uint32 s = lo % c->slots;

	sift_test_in_worker = t != 0;
	atomic_fetch_add(&c->ran[t], 1);
	SIFT_CHECK(hi == lo + 1);
	SIFT_CHECK(scratch->used == 0);
	SIFT_CHECK(c->slot_owner[s] == lo);
	SIFT_CHECK(c->slot_val[s] == value(lo));
	if (t != 0 && atomic_load(&c->hang) && atomic_exchange(&c->hang_taken, 1) == 0)
		while (!atomic_load(&c->hang_release))
			usleep(1000);
	*done = 0;
	SIFT_CHECK(sift_arena_alloc(scratch, 64, 8) != NULL);
	if (lo == c->fail_at)
		return sift_err_fill(err, c->fail_code, lo, 0, "the item failed"), false;
	if (c->leave_every > 0 && lo % c->leave_every == 0)
		return sift_err_fill(err, lo / c->leave_every % 2 ? SIFT_ERR_SERIAL : SIFT_ERR_ARENA_FULL, lo, 0, "left to the leader"), false;
	atomic_fetch_add(&c->folded[lo], 1);
	atomic_fetch_add(&c->tsum, c->slot_val[s]);
	return true;
}

static void
ring_finish(void *vc)
{
	((RingCtx *) vc)->finished = true;
}

static void
ring_drain(void *vc, const SiftParRange *left, uint32 nleft)
{
	SIFT_CHECK(nleft == 0);
	((RingCtx *) vc)->drained = true;
}

static const SiftParOps ring_ops = {.feed_slot = ring_feed,.release_slot = ring_release,.fallback_item = ring_replay,.phase1 = ring_phase1,.finish = ring_finish,
	.overflow_drain = ring_drain};

/* the items in flight are held to THROTTLE, and the run that fails does not pass on the items it leaves */
#define THROTTLE	3

static bool
ring_may_feed(void *vc)
{
	const RingCtx *c = vc;

	on_leader(c);
	return c->fed == c->nreleased || c->fed - c->nreleased < THROTTLE;
}

static const SiftParOps throttle_ops = {.feed_slot = ring_feed,.release_slot = ring_release,.fallback_item = ring_replay,.phase1 = ring_phase1,.finish = ring_finish,
	.overflow_drain = ring_drain,.may_feed = ring_may_feed,.release_emits = true};

#ifndef __SANITIZE_THREAD__
/* the statement is cancelled inside may_feed, as an error or a FATAL of the operator would end it */
static bool
ring_may_feed_cancelled(void *vc)
{
	RingCtx    *c = vc;

	on_leader(c);
	if (c->fed == c->cancel_in_may_feed_at)
	{
		InterruptPending = QueryCancelPending = true;
		CHECK_FOR_INTERRUPTS();
	}
	return true;
}

static const SiftParOps may_feed_cancel_ops = {.feed_slot = ring_feed,.release_slot = ring_release,.fallback_item = ring_replay,.phase1 = ring_phase1,
	.finish = ring_finish,.overflow_drain = ring_drain,.may_feed = ring_may_feed_cancelled};
#endif

typedef struct RunArgs
{
	const SiftParOps *ops;
	RingCtx    *c;
	SiftParCfg	cfg;
	SiftParArenas *ar;
	bool		ok;
} RunArgs;

static void
run_it(void *arg)
{
	RunArgs    *a = arg;

	a->ok = sift_par_run(&a->cfg, a->ops != NULL ? a->ops : &ring_ops, a->c, a->ar, NULL);
}

static RunArgs
ring_init(RingCtx *c, uint32 nthreads, uint32 slots)
{
	RunArgs		a = {.c = c,.cfg = {.mode = SIFT_PAR_RING,.nthreads = nthreads,.ring_slots = slots}};

	memset(c, 0, sizeof(*c));
	c->leader = pthread_self();
	c->slots = slots > 0 ? slots : Min(2 * nthreads, SIFT_PAR_RING_MAX);
	c->nitems = ITEMS;
	c->fail_at = c->cancel_at = c->cancel_in_may_feed_at = UINT32_MAX;
	for (int s = 0; s < SIFT_PAR_RING_MAX; s++)
		c->slot_owner[s] = -1;
	a.ar = sift_par_arenas_create(CurrentMemoryContext, nthreads, 64 << 10, 64 << 10, 64 << 10);
	return a;
}

static RunArgs
setup(RingCtx *c, uint32 nthreads, uint32 slots)
{
	pgshim_reset();
	return ring_init(c, nthreads, slots);
}

/* what an operator's feed_slot may do on the leader: run a ring of its own, on the threads the pool has left for it */
static void
ring_nested(RingCtx *inner)
{
	const int	nthreads = sift_par_pool_threads();
	RunArgs		b = ring_init(inner, nthreads, 0);

	SIFT_CHECK(nthreads == 1);
	inner->nitems = 50;
	run_it(&b);
	SIFT_CHECK(b.ok && inner->finished && inner->fed == 50);
}

static uint64
sum_of(uint32 n, uint32 leave_every)
{
	uint64		sum = 0;

	for (uint32 i = 0; i < n; i++)
		sum += value(i);
	(void) leave_every;
	return sum;
}

/* every item is folded by a thread or replayed by the leader, exactly once, and every slot is passed on once, in order */
static void
check_all(const RingCtx *c, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
	{
		SIFT_CHECK(atomic_load(&c->folded[i]) + atomic_load(&c->replayed[i]) == 1);
		SIFT_CHECK(atomic_load(&c->released[i]) == 1);
		if (c->leave_every > 0 && i % c->leave_every == 0)
			SIFT_CHECK(atomic_load(&c->replayed[i]) == 1);
	}
}

static void
test_ring_sums(void)
{
	static RingCtx c;

	for (uint32 w = 1; w <= 4; w++)
		for (uint32 slots = 0; slots <= 9; slots += (slots == 0 ? 1 : slots == 1 ? 1 : 4))
			for (uint32 leave = 0; leave <= 50; leave += 25)
			{
				RunArgs		a = setup(&c, w, slots);
				uint64		got;

				c.leave_every = leave;
				run_it(&a);
				got = atomic_load(&c.tsum) + c.sum;
				SIFT_CHECK(a.ok && c.finished && !c.drained && !sift_par_pool_busy());
				SIFT_CHECK(c.fed == ITEMS && got == sum_of(ITEMS, leave));
				check_all(&c, ITEMS);
				for (uint32 t = w; t < SIFT_PAR_MAX_THREADS; t++)
					SIFT_CHECK(!atomic_load(&c.ran[t]));
				SIFT_CHECK(atomic_load(&c.ran[0]) + atomic_load(&c.ran[1]) + atomic_load(&c.ran[2]) + atomic_load(&c.ran[3]) == ITEMS);
			}
}

static void
test_ring_empty(void)
{
	static RingCtx c;

	for (uint32 w = 1; w <= 4; w++)
	{
		RunArgs		a = setup(&c, w, 0);

		c.nitems = 0;
		run_it(&a);
		SIFT_CHECK(a.ok && c.finished && c.fed == 0 && !sift_par_pool_busy());
	}
}

/* a failure of an item: the run raises it, every slot that was made is passed on, and the pool is idle */
static void
test_ring_failure(void)
{
	static RingCtx c;

	for (uint32 w = 1; w <= 4; w++)
	{
		RunArgs		a = setup(&c, w, 0);

		c.fail_at = ITEMS / 3;
		c.fail_code = SIFT_ERR_CORRUPT;
		SIFT_CHECK(!pgshim_try(run_it, &a) && pgshim_sqlstate != 0 && !c.finished && !sift_par_pool_busy());
		for (uint32 i = 0; i < c.fed; i++)
			SIFT_CHECK(atomic_load(&c.released[i]) == 1 && atomic_load(&c.replayed[i]) == 0);
		SIFT_CHECK(c.fed >= c.fail_at && c.fed < ITEMS);
	}
}

/* the memory of a thread: the items the threads did not fold are replayed on the leader, the slots passed on, the operator drained, and the run says false */
static void
test_ring_overflow(void)
{
	static RingCtx c;

	for (uint32 w = 1; w <= 4; w++)
	{
		RunArgs		a = setup(&c, w, 0);
		uint64		got;

		c.fail_at = ITEMS / 2;
		c.fail_code = SIFT_ERR_WORK_MEM;
		run_it(&a);
		got = atomic_load(&c.tsum) + c.sum;
		SIFT_CHECK(!a.ok && c.drained && !c.finished && !sift_par_pool_busy());
		SIFT_CHECK(c.fed >= c.fail_at && c.fed < ITEMS);
		check_all(&c, c.fed);
		SIFT_CHECK(got == sum_of(c.fed, 0));
	}
}

static void
test_ring_throttle(void)
{
	static RingCtx c;

	for (uint32 w = 2; w <= 4; w++)
		for (uint32 slots = 0; slots <= 8; slots += 4)
		{
			RunArgs		a = setup(&c, w, slots);

			a.ops = &throttle_ops;
			run_it(&a);
			SIFT_CHECK(a.ok && c.finished && c.fed == ITEMS && c.nreleased == ITEMS && c.max_inflight <= THROTTLE && !sift_par_pool_busy());
			check_all(&c, ITEMS);
		}
	for (uint32 w = 2; w <= 4; w++)
	{
		RunArgs		a = setup(&c, w, 0);

		a.ops = &throttle_ops;
		c.fail_at = ITEMS / 3;
		c.fail_code = SIFT_ERR_CORRUPT;
		SIFT_CHECK(!pgshim_try(run_it, &a) && pgshim_sqlstate != 0 && !c.finished && !sift_par_pool_busy());
		SIFT_CHECK(c.nreleased <= c.fail_at && c.fed > c.nreleased);
	}
}

/* a run started from the callback of a run (the leader is in feed_slot, the pool is busy) runs on the leader alone, and the run around it goes on */
static void
test_ring_nested(void)
{
	static RingCtx c,
				inner;

	for (uint32 w = 2; w <= 4; w++)
	{
		RunArgs		a = setup(&c, w, 0);

		c.inner = &inner;
		c.inner_at = ITEMS / 3;
		run_it(&a);
		SIFT_CHECK(a.ok && c.finished && c.fed == ITEMS && !sift_par_pool_busy());
		check_all(&c, ITEMS);
	}
}

#ifndef __SANITIZE_THREAD__
/* the statement is cancelled inside may_feed: the leader does not hold the lock of the ring while it is in the operator, so the pool is stopped and the run unwinds */
static void
test_ring_may_feed_cancel(void)
{
	static RingCtx c;

	alarm(60);
	for (uint32 w = 2; w <= 4; w++)
	{
		RunArgs		a = setup(&c, w, 0);
		bool		caught = false;

		a.ops = &may_feed_cancel_ops;
		c.cancel_in_may_feed_at = 700;
		PG_TRY();
		{
			run_it(&a);
		}
		PG_CATCH();
		{
			caught = true;
		}
		PG_END_TRY();
		SIFT_CHECK(caught && !c.finished && !sift_par_pool_busy());
		for (uint32 i = 0; i < c.fed; i++)
			SIFT_CHECK(atomic_load(&c.released[i]) == 1);
	}
	alarm(0);
}

typedef struct Canceller
{
	RingCtx    *c;
	bool		taken;
} Canceller;

/* once a helper holds an item that does not end, cancels the statement and says whether the leader took the cancel before the item ended */
static void *
canceller_main(void *p)
{
	Canceller  *k = p;
	const int	before = par_host_cancels_taken;

	while (!atomic_load(&k->c->hang_taken))
		usleep(1000);
	usleep(200000);
	InterruptPending = QueryCancelPending = true;
	for (int i = 0; i < 3000 && par_host_cancels_taken == before; i++)
		usleep(1000);
	k->taken = par_host_cancels_taken > before;
	atomic_store(&k->c->hang_release, 1);
	return NULL;
}

/* a cancel reaches the leader while it waits for a helper that is in the middle of an item: the leader does not wait for the item to end */
static void
test_ring_leader_cancel(void)
{
	static RingCtx c;

	for (uint32 w = 2; w <= 4; w += 2)
	{
		RunArgs		a = setup(&c, w, 0);
		Canceller	k = {.c = &c};
		pthread_t	tid;
		bool		caught = false;

		atomic_store(&c.hang, 1);
		SIFT_CHECK(pthread_create(&tid, NULL, canceller_main, &k) == 0);
		PG_TRY();
		{
			run_it(&a);
		}
		PG_CATCH();
		{
			caught = true;
		}
		PG_END_TRY();
		SIFT_CHECK(pthread_join(tid, NULL) == 0);
		SIFT_CHECK(caught && k.taken && !c.finished && !sift_par_pool_busy());
	}
}

/* a cancel of the leader in the middle of a run: the pool is stopped and waited for, every slot that was made is passed on */
static void
test_ring_cancel(void)
{
	static RingCtx c;

	for (uint32 w = 1; w <= 4; w++)
		for (int round = 0; round < 20; round++)
		{
			RunArgs		a = setup(&c, w, 0);
			bool		caught = false;

			c.cancel_at = 100 + 300 * round;
			PG_TRY();
			{
				run_it(&a);
			}
			PG_CATCH();
			{
				caught = true;
			}
			PG_END_TRY();
			SIFT_CHECK(caught && !c.finished && !sift_par_pool_busy());
			for (uint32 i = 0; i < c.fed; i++)
				SIFT_CHECK(atomic_load(&c.released[i]) == 1);
		}
}
#endif

int
main(void)
{
	pgshim_keep();
	test_ring_sums();
	test_ring_empty();
	test_ring_failure();
	test_ring_overflow();
	test_ring_throttle();
	if (unit_wants("ring_nested"))
		test_ring_nested();
#ifndef __SANITIZE_THREAD__
	test_ring_cancel();
	if (unit_wants("ring_may_feed_cancel"))
		test_ring_may_feed_cancel();
	if (unit_wants("ring_leader_cancel"))
		test_ring_leader_cancel();
#endif
	par_host_proc_exit();
	puts("par_ring: ok");
	return 0;
}
