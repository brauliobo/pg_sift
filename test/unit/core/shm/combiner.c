/*
 * test/unit/core/shm/combiner.c - the combiner of core/shm/combiner.c when an apply() fails or its backend exits: a parked leader whose apply() fails keeps its slot until the leader that detached
 * it gave it its state (the slot is not handed to another writer while that leader still writes it), and the followers of a leader that exits in apply() are given RETRY and finish.
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
static bool held_once;
static bool f_slot_given_back;
static int32 f_idx = -1;
static int32 manual[3];
static int	nmanual;
static pthread_t f_thread;

void
sift_shm_before_exit(void (*fn) (int, Datum))
{
	exit_fn = fn;
}

static bool
slot_free(int32 idx)
{
	return (pg_atomic_read_u64(&pool.free_mask[idx / SIFT_COMB_POOL_SLOTS]) >> (idx % SIFT_COMB_POOL_SLOTS)) & 1;
}

static void
sleep_ms(int ms)
{
	struct timespec ts = {.tv_sec = 0,.tv_nsec = ms * 1000000L};

	nanosleep(&ts, NULL);
}

/* The first wake-up of the first leader waits for the parked leader to give up: what it finds is whether that one gave its slot back before it had its state */
void
sift_shm_wake(int procno)
{
	if (sift_shm_procno == T0 && scenario == 1 && !held_once)
	{
		held_once = true;
		for (int i = 0; i < HOLD_MS && pg_atomic_read_u32(&f_done) == 0; i++)
			sleep_ms(1);
		f_slot_given_back = slot_free(f_idx);
	}
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

static const SiftCombOps ops_fail;
static const SiftCombOps ops_exit;

/*
 * Scenario 1: T0 leads four rounds: its own request, a request on the stack, F (which pushed on an empty stack: a parked leader) and another; a last request is left on the stack. F takes the
 * lock while T0 wakes the first of its list, detaches the last request and its apply() fails.
 */
static void
apply_fail(void *ctx, SiftCombHdr *const *batch, uint32 n)
{
	if (sift_shm_procno == F)
		SIFT_SHM_RETHROW();
	switch (t0_round++)
	{
		case 0:
			push_manual();
			break;
		case 1:
			start_f(&ops_fail);
			break;
		case 2:
		case 3:
			push_manual();
			break;
	}
}

/* Scenario 2: T0 detaches F (a parked leader) and its backend exits in apply(): its exit callbacks run and its locks are released (LWLockReleaseAll) */
static void
apply_exit(void *ctx, SiftCombHdr *const *batch, uint32 n)
{
	if (sift_shm_procno != T0)
		return;
	if (t0_round++ == 0)
	{
		start_f(&ops_exit);
		return;
	}
	if (exit_fn != NULL)
		exit_fn(1, (Datum) 0);
	sift_shm_unlock(&queue.lock);
	pthread_exit(NULL);
}

static const SiftCombOps ops_fail = {.same_group = same_group,.apply = apply_fail,.max_batch = 8};
static const SiftCombOps ops_exit = {.same_group = same_group,.apply = apply_exit,.max_batch = 8};

static void *
t0_main(void *arg)
{
	TestSlot	req = {.who = T0};

	sift_shm_procno = T0;
	sift_comb_submit(&arena, &queue, (const SiftCombOps *) arg, NULL, &req.hdr, sizeof(req));
	return NULL;
}

static void
reset(int which)
{
	for (int i = 0; i < SIFT_SHM_MAX_PROCS; i++)
		sem_init(&sift_shm_sems[i], 0, 0);
	sift_comb_pool_init_sharded(&pool);
	sift_comb_queue_init(&queue, 0);
	memset(slots, 0, sizeof(slots));
	scenario = which;
	t0_round = 0;
	held_once = f_slot_given_back = false;
	pg_atomic_init_u32(&f_done, 0);
	nmanual = 0;
	f_idx = -1;
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

static void
test_failed_parked_leader(void)
{
	pthread_t	t0;

	reset(1);
	SIFT_CHECK(pthread_create(&t0, NULL, t0_main, (void *) &ops_fail) == 0);
	SIFT_CHECK(pthread_join(t0, NULL) == 0);
	SIFT_CHECK(join_f());
	SIFT_CHECKF(!f_slot_given_back, "a parked leader whose apply() failed gave its slot back before the leader that detached it wrote its state");
	SIFT_CHECKF(pg_atomic_read_u32(&slots[manual[2]].hdr.state) == SIFT_COMB_RETRY, "the request the failed apply() held is given RETRY");
	SIFT_CHECKF(pg_atomic_read_u32(&slots[manual[0]].hdr.state) == SIFT_COMB_DONE && pg_atomic_read_u32(&slots[manual[1]].hdr.state) == SIFT_COMB_DONE,
				"the requests the first leader applied are DONE");
	SIFT_CHECKF(slot_free(f_idx) && sift_shm_lock_try(&queue.lock), "the slot of the failed leader is free at the end and the queue lock is not held");
}

static void
test_leader_exits(void)
{
	pthread_t	t0;

	reset(2);
	SIFT_CHECK(pthread_create(&t0, NULL, t0_main, (void *) &ops_exit) == 0);
	SIFT_CHECK(pthread_join(t0, NULL) == 0);
	SIFT_CHECKF(join_f(), "a follower of a leader that exited in apply() finishes within a second");
}

int
main(void)
{
	test_failed_parked_leader();
	test_leader_exits();
	return sift_check_summary();
}
