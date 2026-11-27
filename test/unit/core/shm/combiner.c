/*
 * test/unit/core/shm/combiner.c - the combiner of core/shm/combiner.c when an apply() fails or its backend exits: a parked leader whose apply() fails gives its followers RETRY, its slot back and
 * the error to its writer; one whose request another leader detached applies nothing and waits for that leader to give it its state; and the followers of a leader that exits in apply() are given
 * RETRY and finish. A leader applies the one batch that holds its own request, never one of the requests that arrived while it applied (an error of those is not its own), and puts the requests it
 * did not apply back on the stack with the oldest woken to lead them; writers of two groups that share a queue have every request applied once, in batches of one group. A parked leader that
 * exits in apply() gives its slot back, and a leader that exits after it handed requests back to the stack wakes the one it told to lead them.
 */
#define SIFT_SHM_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include <errno.h>
#include <time.h>

#include "test/common/bench.h"

#include "core/shm/combiner.c"

enum { T0 = 1, F = 2, HOLD_MS = 300 };

typedef struct TestSlot
{
	SiftCombHdr hdr;
	uint32		who;
	uint32		grp;
} TestSlot;

sem_t		sift_shm_sems[SIFT_SHM_MAX_PROCS];
__thread int sift_shm_procno;
__thread sigjmp_buf *sift_shm_jmp;
static __thread void (*exit_fn) (int, Datum);

static SiftCombPool pool;
static SiftCombQueue queue;
static TestSlot slots[SIFT_COMB_MAX_SLOTS];
static const SiftCombArena arena = {.pool = &pool,.slots = (char *) slots,.stride = sizeof(TestSlot)};
static int	scenario;
static int	t0_round;
static pg_atomic_uint32 f_done;
static bool f_threw;
static int32 f_idx = -1;
static int32 manual[3];
static int	nmanual;
static pthread_t f_thread;
static pg_atomic_uint32 wakes[SIFT_SHM_MAX_PROCS];

void
sift_shm_before_exit(void (*fn) (int, Datum))
{
	exit_fn = fn;
}

static bool
slot_free(int32 idx)
{
	return (pg_atomic_read_u64(&pool.shard[idx / SIFT_COMB_POOL_SLOTS].free_mask) >> (idx % SIFT_COMB_POOL_SLOTS)) & 1;
}

static void
sleep_ms(int ms)
{
	struct timespec ts = {.tv_sec = 0,.tv_nsec = ms * 1000000L};

	nanosleep(&ts, NULL);
}

void
sift_shm_wake(int procno)
{
	pg_atomic_fetch_add_u32(&wakes[procno], 1);
	sem_post(&sift_shm_sems[procno]);
}

static bool
same_group(const SiftCombHdr *lead, const SiftCombHdr *other)
{
	return true;
}

/* A request of another writer, on the stack as a writer that found the lock busy puts it (its writer does not lead) */
static void
push_manual(void)
{
	int32		idx = pool_claim(&pool);
	TestSlot   *s = &slots[idx];

	SIFT_CHECK(idx >= 0);
	s->hdr.procno = 0;
	s->who = 0;
	pg_atomic_write_u32(&s->hdr.state, SIFT_COMB_WAITING);
	manual[nmanual++] = idx;
	(void) push(&queue, &s->hdr, (uint32) idx);
}

static void *
f_main(void *arg)
{
	TestSlot	req = {.who = F};

	sift_shm_procno = F;
	SIFT_SHM_TRY();
	{
		sift_comb_submit(&arena, &queue, (const SiftCombOps *) arg, NULL, &req.hdr, sizeof(req));
	}
	SIFT_SHM_CATCH();
	{
		f_threw = true;
	}
	SIFT_SHM_END_TRY();
	pg_atomic_write_u32(&f_done, 1);
	return NULL;
}

static void
start_f(const SiftCombOps *ops)
{
	SIFT_CHECK(pthread_create(&f_thread, NULL, f_main, (void *) ops) == 0);
	while (pg_atomic_read_u32(&queue.wait_head) == SIFT_COMB_NONE)
		sched_yield();
	f_idx = (int32) pg_atomic_read_u32(&queue.wait_head);
}

static const SiftCombOps ops_exit;

/* Scenarios 1 and 8: the apply() of F fails; scenario 9: T0 applies its own request while another lands on the stack, and an apply() of that one would fail */
static void
apply_fail(void *ctx, SiftCombHdr *const *batch, uint32 n)
{
	if (sift_shm_procno == F || t0_round++ > 0)
		SIFT_SHM_RETHROW();
	if (scenario == 9)
		push_manual();
}

/* Scenario 2: T0 detaches F (a parked leader) and its backend exits in apply(): its exit callbacks run and its locks are released (LWLockReleaseAll) */
static void
apply_exit(void *ctx, SiftCombHdr *const *batch, uint32 n)
{
	if (sift_shm_procno != T0)
		return;
	if (exit_fn != NULL)
		exit_fn(1, (Datum) 0);
	sift_shm_unlock(&queue.lock);
	pthread_exit(NULL);
}

static const SiftCombOps ops_fail = {.same_group = same_group,.apply = apply_fail,.max_batch = 8};

static bool
t0_submit(const SiftCombOps *ops)
{
	TestSlot	req = {.who = T0};
	bool		threw = false;

	sift_shm_procno = T0;
	SIFT_SHM_TRY();
	{
		sift_comb_submit(&arena, &queue, ops, NULL, &req.hdr, sizeof(req));
	}
	SIFT_SHM_CATCH();
	{
		threw = true;
	}
	SIFT_SHM_END_TRY();
	return threw;
}

static const SiftCombOps ops_exit = {.same_group = same_group,.apply = apply_exit,.max_batch = 8};

/* T0 takes the lock, F parks on the empty stack, and T0 leads as sift_comb_submit does once it holds the lock */
static void *
t0_exit_main(void *arg)
{
	TestSlot	req = {.who = T0};

	sift_shm_procno = T0;
	sift_shm_before_exit(lead_exit);
	SIFT_CHECK(sift_shm_lock_try(&queue.lock));
	start_f(&ops_exit);
	leader_locked(&arena, &queue, &ops_exit, NULL, &req.hdr);
	return NULL;
}

static void
reset(int which)
{
	for (int i = 0; i < SIFT_SHM_MAX_PROCS; i++)
		sem_init(&sift_shm_sems[i], 0, 0);
	sift_comb_pool_init(&pool);
	sift_comb_queue_init(&queue, 0);
	memset(slots, 0, sizeof(slots));
	scenario = which;
	t0_round = 0;
	f_threw = false;
	pg_atomic_init_u32(&f_done, 0);
	nmanual = 0;
	f_idx = -1;
	for (int i = 0; i < SIFT_SHM_MAX_PROCS; i++)
		pg_atomic_init_u32(&wakes[i], 0);
}

/* F finishes within a second; one that waits for a state nobody will write is let go (RETRY) so that the test ends */
static bool
join_f(void)
{
	struct timespec dl;
	int			rc;

	clock_gettime(CLOCK_REALTIME, &dl);
	dl.tv_sec += 1;
	if ((rc = pthread_timedjoin_np(f_thread, NULL, &dl)) == ETIMEDOUT)
	{
		pg_atomic_write_u32(&slots[f_idx].hdr.state, SIFT_COMB_RETRY);
		sem_post(&sift_shm_sems[F]);
		SIFT_CHECK(pthread_join(f_thread, NULL) == 0);
	}
	return rc == 0;
}

/* Scenario 1: F parks on an empty stack while the lock is held, a request lands on the stack after it, and the apply() of F fails once F leads them */
static void
test_failed_parked_leader(void)
{
	reset(1);
	SIFT_CHECK(sift_shm_lock_try(&queue.lock));
	start_f(&ops_fail);
	push_manual();
	sift_shm_unlock(&queue.lock);
	SIFT_CHECK(join_f());
	SIFT_CHECKF(f_threw, "the error of the batch of a parked leader reaches its writer");
	SIFT_CHECKF(pg_atomic_read_u32(&slots[manual[0]].hdr.state) == SIFT_COMB_RETRY, "the request the failed apply() held is given RETRY");
	SIFT_CHECKF(slot_free(f_idx) && sift_shm_lock_try(&queue.lock), "the slot of the failed leader is free at the end and the queue lock is not held");
}

/*
 * Scenario 8: a leader detached the request of F (a parked leader) and let the lock go, and has not written its state yet; F takes the lock and finds another request on the stack. F applies
 * nothing (its own request is not there), puts that request back for its writer to lead, and keeps its slot until the leader that holds its request writes its state
 */
static void *
f_parked_main(void *arg)
{
	sift_shm_procno = F;
	SIFT_SHM_TRY();
	{
		leader_parked(&arena, &queue, &ops_fail, NULL, &slots[f_idx].hdr, (uint32) f_idx);
	}
	SIFT_SHM_CATCH();
	{
		f_threw = true;
	}
	SIFT_SHM_END_TRY();
	pg_atomic_write_u32(&f_done, 1);
	return NULL;
}

static void
test_detached_parked_leader(void)
{
	reset(8);
	f_idx = pool_claim(&pool);
	slots[f_idx].hdr.procno = F;
	pg_atomic_write_u32(&slots[f_idx].hdr.state, SIFT_COMB_WAITING);
	push_manual();
	SIFT_CHECK(pthread_create(&f_thread, NULL, f_parked_main, NULL) == 0);
	for (int i = 0; i < HOLD_MS && pg_atomic_read_u32(&slots[manual[0]].hdr.state) != SIFT_COMB_LEAD; i++)
		sleep_ms(1);
	SIFT_CHECKF(pg_atomic_read_u32(&slots[manual[0]].hdr.state) == SIFT_COMB_LEAD && pg_atomic_read_u32(&wakes[0]) == 1,
				"the request on the stack goes back to its writer to lead (state %u)", pg_atomic_read_u32(&slots[manual[0]].hdr.state));
	SIFT_CHECKF(t0_round == 0 && pg_atomic_read_u32(&f_done) == 0, "a parked leader whose request another leader holds applies nothing and waits for its state");
	pg_atomic_write_u32(&slots[f_idx].hdr.state, SIFT_COMB_DONE);
	sem_post(&sift_shm_sems[F]);
	SIFT_CHECK(pthread_join(f_thread, NULL) == 0);
	SIFT_CHECKF(!f_threw && !slot_free(f_idx) && sift_shm_lock_try(&queue.lock), "F has no error, its slot stays its own, and the queue lock is not held");
}

/* Scenario 9: T0 applies its own request; a request lands on the stack meanwhile, whose apply() fails. T0 never applies it: its own batch was applied, and it returns with no error */
static void
test_leader_keeps_applied_batch(void)
{
	reset(9);
	SIFT_CHECKF(!t0_submit(&ops_fail), "a leader whose own batch was applied returns with no error");
	SIFT_CHECKF(t0_round == 1, "the leader applies its own batch only: %d applies", t0_round);
	SIFT_CHECKF(pg_atomic_read_u32(&slots[manual[0]].hdr.state) == SIFT_COMB_WAITING && pg_atomic_read_u32(&queue.wait_head) == (uint32) manual[0],
				"the request that arrived meanwhile is left on the stack for its writer to lead");
	SIFT_CHECKF(sift_shm_lock_try(&queue.lock), "the queue lock is not held");
}

static void
test_leader_exits(void)
{
	pthread_t	t0;

	reset(2);
	SIFT_CHECK(pthread_create(&t0, NULL, t0_exit_main, NULL) == 0);
	SIFT_CHECK(pthread_join(t0, NULL) == 0);
	SIFT_CHECKF(join_f(), "a follower of a leader that exited in apply() finishes within a second");
}

static bool
same_grp(const SiftCombHdr *lead, const SiftCombHdr *other)
{
	return ((const TestSlot *) lead)->grp == ((const TestSlot *) other)->grp;
}

enum { GRP_A = 1, GRP_B = 2, NGRP = 3, MANUAL_PROC = 10 };

static int32 grp_slot[NGRP][3];
static int	grp_applies[NGRP];
static int	grp_items[NGRP];

/* A request of group grp on the stack whose writer (procno MANUAL_PROC + its number) waits and does not lead */
static int32
push_grp(uint32 grp, int n)
{
	int32		idx = pool_claim(&pool);
	TestSlot   *s = &slots[idx];

	s->hdr.procno = MANUAL_PROC + n;
	s->grp = grp;
	pg_atomic_write_u32(&s->hdr.state, SIFT_COMB_WAITING);
	(void) push(&queue, &s->hdr, (uint32) idx);
	return idx;
}

/* Scenario 3: three requests of group A and three of B are on the stack when T0 (group A) takes the lock, with batches of two */
static void
apply_groups(void *ctx, SiftCombHdr *const *batch, uint32 n)
{
	uint32		grp = ((TestSlot *) batch[0])->grp;

	for (uint32 i = 0; i < n; i++)
		SIFT_CHECKF(((TestSlot *) batch[i])->grp == grp, "a batch holds requests of two groups");
	grp_applies[grp]++;
	grp_items[grp] += n;
}

static const SiftCombOps ops_groups = {.same_group = same_grp,.apply = apply_groups,.max_batch = 2};

static void
test_leftover_groups(void)
{
	TestSlot	req = {.who = T0,.grp = GRP_A};
	uint32		woken = 0,
				stacked = 0;
	int32		left[5];

	reset(3);
	memset(grp_applies, 0, sizeof(grp_applies));
	memset(grp_items, 0, sizeof(grp_items));
	for (int i = 0; i < 3; i++)
	{
		grp_slot[GRP_A][i] = push_grp(GRP_A, 2 * i);
		grp_slot[GRP_B][i] = push_grp(GRP_B, 2 * i + 1);
	}
	left[0] = grp_slot[GRP_B][0];
	for (int i = 1; i < 3; i++)
	{
		left[2 * i - 1] = grp_slot[GRP_A][i];
		left[2 * i] = grp_slot[GRP_B][i];
	}
	sift_shm_procno = T0;
	sift_comb_submit(&arena, &queue, &ops_groups, NULL, &req.hdr, sizeof(req));
	SIFT_CHECKF(pg_atomic_read_u32(&slots[grp_slot[GRP_A][0]].hdr.state) == SIFT_COMB_DONE, "the oldest request of the leader's group is applied with its own");
	SIFT_CHECKF(grp_items[GRP_A] == 2 && grp_applies[GRP_A] == 1, "the leader applies one batch of two (%d in %d)", grp_items[GRP_A], grp_applies[GRP_A]);
	SIFT_CHECKF(grp_items[GRP_B] == 0, "the leader applies no request of another group with its context");
	for (int i = 1; i < 6; i++)
		woken += pg_atomic_read_u32(&wakes[MANUAL_PROC + i]);
	SIFT_CHECKF(woken == 1 && pg_atomic_read_u32(&wakes[MANUAL_PROC + 1]) == 1, "the writer of the oldest request left is woken, and no other (%u)", woken);
	SIFT_CHECKF(pg_atomic_read_u32(&slots[grp_slot[GRP_B][0]].hdr.state) == SIFT_COMB_LEAD, "the oldest request left is woken to lead the rest");
	for (uint32 i = pg_atomic_read_u32(&queue.wait_head); i != SIFT_COMB_NONE; i = slots[i].hdr.next)
		SIFT_CHECKF((int32) i == left[4 - stacked++], "the requests left are back on the stack, oldest at the bottom");
	SIFT_CHECKF(stacked == 5, "the 5 requests left are on the stack (%u)", stacked);
}

enum { STRESS_THREADS = 16, STRESS_ITERS = 400 };

static pg_atomic_uint32 stress_applied[STRESS_THREADS + 1];
static pg_atomic_uint32 stress_batches;

static void
apply_stress(void *ctx, SiftCombHdr *const *batch, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
	{
		SIFT_CHECKF(same_grp(batch[0], batch[i]), "a batch holds requests of two groups");
		pg_atomic_fetch_add_u32(&stress_applied[((TestSlot *) batch[i])->who], 1);
	}
	pg_atomic_fetch_add_u32(&stress_batches, 1);
	for (int i = 0; i < 50; i++)
		pg_spin_delay();
}

static const SiftCombOps ops_stress = {.same_group = same_grp,.apply = apply_stress,.max_batch = 3};

static void *
stress_main(void *arg)
{
	int			who = (int) (intptr_t) arg;

	sift_shm_procno = who;
	for (int i = 0; i < STRESS_ITERS; i++)
	{
		TestSlot	req = {.who = (uint32) who,.grp = (uint32) (who % 2)};

		sift_comb_submit(&arena, &queue, &ops_stress, NULL, &req.hdr, sizeof(req));
	}
	return NULL;
}

/* Scenario 4: writers of two groups on one queue: each request is applied once, and none waits for ever */
static void
test_two_groups_stress(void)
{
	pthread_t	th[STRESS_THREADS];

	reset(4);
	pg_atomic_init_u32(&stress_batches, 0);
	for (int i = 1; i <= STRESS_THREADS; i++)
		pg_atomic_init_u32(&stress_applied[i], 0);
	for (int i = 0; i < STRESS_THREADS; i++)
		SIFT_CHECK(pthread_create(&th[i], NULL, stress_main, (void *) (intptr_t) (i + 1)) == 0);
	for (int i = 0; i < STRESS_THREADS; i++)
		SIFT_CHECK(pthread_join(th[i], NULL) == 0);
	for (int i = 1; i <= STRESS_THREADS; i++)
		SIFT_CHECKF(pg_atomic_read_u32(&stress_applied[i]) == STRESS_ITERS, "writer %d had %u of %d requests applied", i, pg_atomic_read_u32(&stress_applied[i]), STRESS_ITERS);
	SIFT_CHECKF(pg_atomic_read_u32(&queue.wait_head) == SIFT_COMB_NONE && sift_shm_lock_try(&queue.lock), "the queue is empty and free at the end");
	for (int i = 0; i < SIFT_COMB_MAX_SLOTS; i++)
		SIFT_CHECKF(slot_free(i), "slot %d of the pool is free at the end", i);
	printf("two groups: %u batches for %d requests\n", pg_atomic_read_u32(&stress_batches), STRESS_THREADS * STRESS_ITERS);
}

/* Each shard of the free mask of the pool is a cache line of its own */
static void
test_pool_lines(void)
{
	for (int s = 1; s < SIFT_COMB_SHARDS; s++)
		SIFT_CHECKF((char *) &pool.shard[s].free_mask - (char *) &pool.shard[s - 1].free_mask >= PG_CACHE_LINE_SIZE, "shards %d and %d of the pool share a cache line", s - 1, s);
}


enum { P = 3, HANDOFF = 5 };

/* Scenario 5: P parks while the lock is held and leads once it is let go; its backend exits in apply() */
static void
apply_parked_exit(void *ctx, SiftCombHdr *const *batch, uint32 n)
{
	if (exit_fn != NULL)
		exit_fn(1, (Datum) 0);
	sift_shm_unlock(&queue.lock);
	pthread_exit(NULL);
}

static const SiftCombOps ops_parked_exit = {.same_group = same_group,.apply = apply_parked_exit,.max_batch = 8};

static void *
p_main(void *arg)
{
	TestSlot	req = {.who = P};

	sift_shm_procno = P;
	sift_comb_submit(&arena, &queue, &ops_parked_exit, NULL, &req.hdr, sizeof(req));
	return NULL;
}

static void
test_parked_leader_exits(void)
{
	pthread_t	p;

	reset(5);
	SIFT_CHECK(sift_shm_lock_try(&queue.lock));
	SIFT_CHECK(pthread_create(&p, NULL, p_main, NULL) == 0);
	while (pg_atomic_read_u32(&queue.wait_head) == SIFT_COMB_NONE)
		sched_yield();
	sift_shm_unlock(&queue.lock);
	SIFT_CHECK(pthread_join(p, NULL) == 0);
	for (int i = 0; i < SIFT_COMB_MAX_SLOTS; i++)
		SIFT_CHECKF(slot_free(i), "slot %d of the pool is held for ever by a parked leader that exited", i);
}

/* A leader that put every request it detached back on the stack and exits before it resolved wakes the writer it told to lead them */
static void
test_exit_wakes_handoff(void)
{
	reset(6);
	lead_state = (CombLead) {.arena = arena,.own = SIFT_COMB_NONE,.handoff = HANDOFF,.n = 0};
	lead_exit(1, (Datum) 0);
	SIFT_CHECKF(pg_atomic_read_u32(&wakes[HANDOFF]) == 1, "the writer told to lead the requests put back is never woken");
}

/* A backend that never led (its exit callback is registered by its first submit) exits: nothing to resolve, no slot to give back, nobody to wake */
static void *
fresh_exit_main(void *arg)
{
	sift_shm_procno = P;
	lead_exit(1, (Datum) 0);
	return NULL;
}

static void
test_fresh_backend_exits(void)
{
	pthread_t	th;
	uint32		woken = 0;

	reset(7);
	SIFT_CHECK(pthread_create(&th, NULL, fresh_exit_main, NULL) == 0);
	SIFT_CHECK(pthread_join(th, NULL) == 0);
	for (int i = 0; i < SIFT_SHM_MAX_PROCS; i++)
		woken += pg_atomic_read_u32(&wakes[i]);
	SIFT_CHECKF(woken == 0, "the exit of a backend that never led wakes %u writers", woken);
	for (int i = 0; i < SIFT_COMB_MAX_SLOTS; i++)
		SIFT_CHECKF(slot_free(i), "slot %d of the pool changed at the exit of a backend that never led", i);
}

int
main(void)
{
#ifndef __SANITIZE_THREAD__
	/* ThreadSanitizer cannot follow the longjmp of an error out of apply() */
	test_failed_parked_leader();
#endif
	test_detached_parked_leader();
	test_leader_keeps_applied_batch();
	test_leader_exits();
	test_leftover_groups();
	test_two_groups_stress();
	test_pool_lines();
	test_parked_leader_exits();
	test_exit_wakes_handoff();
	test_fresh_backend_exits();
	return sift_check_summary();
}
