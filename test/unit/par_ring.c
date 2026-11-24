/*
 * par_ring.c - core/par/morsel.h in RING mode: the leader makes the items into a ring of slots, the threads take them, the leader passes the slots on in order and replays on its own
 * what a thread leaves to it (SIFT_ERR_ARENA_FULL, SIFT_ERR_SERIAL), on 1 to 4 threads that the trap shim flags as workers (no palloc, no ereport).
 */
#include "postgres.h"

#include <pthread.h>
#include <stdatomic.h>

#include "core/hash.h"
#include "core/par/morsel.h"
#include "core/par/pool.h"
#include "miscadmin.h"
#include "par_host.h"
#include "pgshim.h"
#include "unit.h"

#define ITEMS		6000

typedef struct RingCtx
{
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
	CHECK(pthread_equal(pthread_self(), c->leader));
}

static bool
ring_feed(void *vc, uint32 item)
{
	RingCtx    *c = vc;
	const uint32 s = item % c->slots;

	on_leader(c);
	CHECK(item == c->fed && c->slot_owner[s] == -1);
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
	CHECK(c->slot_owner[s] == item && (item == 0 ? c->last_released == 0 : c->last_released + 1 == item));
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
	CHECK(c->slot_owner[s] == item);
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
	CHECK(hi == lo + 1);
	CHECK(scratch->used == 0);
	CHECK(c->slot_owner[s] == lo);
	CHECK(c->slot_val[s] == value(lo));
	*done = 0;
	CHECK(sift_arena_alloc(scratch, 64, 8) != NULL);
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
	CHECK(nleft == 0);
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
setup(RingCtx *c, uint32 nthreads, uint32 slots)
{
	RunArgs		a = {.c = c,.cfg = {.mode = SIFT_PAR_RING,.nthreads = nthreads,.ring_slots = slots}};

	pgshim_reset();
	memset(c, 0, sizeof(*c));
	c->leader = pthread_self();
	c->slots = slots > 0 ? slots : Min(2 * nthreads, SIFT_PAR_RING_MAX);
	c->nitems = ITEMS;
	c->fail_at = c->cancel_at = UINT32_MAX;
	for (int s = 0; s < SIFT_PAR_RING_MAX; s++)
		c->slot_owner[s] = -1;
	a.ar = sift_par_arenas_create(CurrentMemoryContext, nthreads, 64 << 10, 64 << 10, 64 << 10);
	return a;
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
		CHECK(atomic_load(&c->folded[i]) + atomic_load(&c->replayed[i]) == 1);
		CHECK(atomic_load(&c->released[i]) == 1);
		if (c->leave_every > 0 && i % c->leave_every == 0)
			CHECK(atomic_load(&c->replayed[i]) == 1);
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
				CHECK(a.ok && c.finished && !c.drained && !sift_par_pool_busy());
				CHECK(c.fed == ITEMS && got == sum_of(ITEMS, leave));
				check_all(&c, ITEMS);
				for (uint32 t = w; t < SIFT_PAR_MAX_THREADS; t++)
					CHECK(!atomic_load(&c.ran[t]));
				CHECK(atomic_load(&c.ran[0]) + atomic_load(&c.ran[1]) + atomic_load(&c.ran[2]) + atomic_load(&c.ran[3]) == ITEMS);
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
		CHECK(a.ok && c.finished && c.fed == 0 && !sift_par_pool_busy());
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
		CHECK(!pgshim_try(run_it, &a) && pgshim_sqlstate != 0 && !c.finished && !sift_par_pool_busy());
		for (uint32 i = 0; i < c.fed; i++)
			CHECK(atomic_load(&c.released[i]) == 1 && atomic_load(&c.replayed[i]) == 0);
		CHECK(c.fed >= c.fail_at && c.fed < ITEMS);
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
		CHECK(!a.ok && c.drained && !c.finished && !sift_par_pool_busy());
		CHECK(c.fed >= c.fail_at && c.fed < ITEMS);
		check_all(&c, c.fed);
		CHECK(got == sum_of(c.fed, 0));
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
			CHECK(a.ok && c.finished && c.fed == ITEMS && c.nreleased == ITEMS && c.max_inflight <= THROTTLE && !sift_par_pool_busy());
			check_all(&c, ITEMS);
		}
	for (uint32 w = 2; w <= 4; w++)
	{
		RunArgs		a = setup(&c, w, 0);

		a.ops = &throttle_ops;
		c.fail_at = ITEMS / 3;
		c.fail_code = SIFT_ERR_CORRUPT;
		CHECK(!pgshim_try(run_it, &a) && pgshim_sqlstate != 0 && !c.finished && !sift_par_pool_busy());
		CHECK(c.nreleased <= c.fail_at && c.fed > c.nreleased);
	}
}

#ifndef __SANITIZE_THREAD__
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
			CHECK(caught && !c.finished && !sift_par_pool_busy());
			for (uint32 i = 0; i < c.fed; i++)
				CHECK(atomic_load(&c.released[i]) == 1);
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
#ifndef __SANITIZE_THREAD__
	test_ring_cancel();
#endif
	par_host_proc_exit();
	puts("par_ring: ok");
	return 0;
}
