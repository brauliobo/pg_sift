/*
 * test/unit/core/maint/sched.c - the queue of core/maint/sched.c on a fake host: one busy database gets as many workers as it has runnable requests (up to the cap), a request that
 * coalesces into a stranded one (no worker is there for it) still starts one, a request put back for a busy lock keeps the worker of its database, and the queue holds a request for every
 * partition of a large server.
 */
#define SIFT_SHM_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "core/maint/sched.c"
#include "core/maint/db.c"
#include "core/maint/sched_reap.c"

sem_t		sift_shm_sems[SIFT_SHM_MAX_PROCS];
__thread int sift_shm_procno;
__thread sigjmp_buf *sift_shm_jmp;

static TimestampTz clock_now = 1000000;
static int	spawns;
static int	starved;
static bool spawn_ok = true;
static SiftMaintShmem *m;

void
sift_horizon_ring_init(SiftStandbyHorizon *r)
{
	memset(r, 0, sizeof(*r));
}

void
sift_shm_wake(int procno)
{
}

void
sift_shm_before_exit(void (*fn) (int, Datum))
{
}

static TimestampTz host_now(void) { return clock_now; }
static int	host_pid(void) { return 4242; }
static bool host_alive(int pid) { return true; }
static bool host_spawn(Oid db, int worker) { spawns++; return spawn_ok; }
static void host_wake(int procno) { }
static bool host_defer_cold(void) { return false; }
static void host_starved(SiftPacerTier tier, TimestampTz now) { starved++; }
static bool host_yield(SiftPacerTier tier, TimestampTz now) { return false; }
static int	host_bgw(void) { return 0; }

static const SiftMaintHost host = {
	.now = host_now,.self_pid = host_pid,.pid_alive = host_alive,.spawn = host_spawn,.wake = host_wake,.defer_cold = host_defer_cold,
	.note_starved = host_starved,.should_yield = host_yield,.bgworkers_in_use = host_bgw
};

static void
fresh(uint32 max_workers)
{
	free(m);
	SIFT_CHECK(posix_memalign((void **) &m, PG_CACHE_LINE_SIZE, sizeof(SiftMaintShmem)) == 0);
	sift_maint_shmem_init(m, 1, max_workers, 1000);
	spawns = starved = 0;
	spawn_ok = true;
}

static SiftMaintReq
req(Oid db, Oid rel, SiftMaintKind kind)
{
	return (SiftMaintReq) {.db = db,.rel = rel,.kind = kind,.tier = SIFT_MAINT_TIER_DEFAULT};
}

static SiftMaintEnq
enq(Oid db, Oid rel, SiftMaintKind kind)
{
	SiftMaintReq r = req(db, rel, kind);

	return sift_maint_enqueue(m, &host, &r);
}

static int
kick_all(void)
{
	int			n = 0;

	while (sift_maint_kick(m, &host) == SIFT_KICK_SPAWNED)
		n++;
	return n;
}

/* One database with four tables to flush and sift.max_maint_workers = 4: four workers, not one; two tables: two */
static void
test_busy_db(void)
{
	fresh(4);
	for (Oid rel = 1; rel <= 4; rel++)
		(void) enq(5, rel, SIFT_MK_SLAB_FLUSH);
	SIFT_CHECKF(kick_all() == 4, "a busy database gets a worker for each runnable request up to the cap: %d", spawns);
	fresh(4);
	for (Oid rel = 1; rel <= 2; rel++)
		(void) enq(5, rel, SIFT_MK_SLAB_FLUSH);
	SIFT_CHECKF(kick_all() == 2, "never more workers than runnable requests: %d", spawns);
	fresh(2);
	for (Oid rel = 1; rel <= 4; rel++)
		(void) enq(5, rel, SIFT_MK_SLAB_FLUSH);
	SIFT_CHECKF(kick_all() == 2 && starved == 0, "a served database at the cap starves no tier: %d workers, %d starved", spawns, starved);
}

/* A held database gets no worker, busy or not */
static void
test_held_db(void)
{
	fresh(4);
	for (Oid rel = 1; rel <= 3; rel++)
		(void) enq(6, rel, SIFT_MK_SLAB_FLUSH);
	sift_maint_db_hold(m, 6);
	SIFT_CHECKF(kick_all() == 0 && spawns == 0, "a held database gets no worker: %d", spawns);
	sift_maint_db_release(m, 6);
	SIFT_CHECKF(kick_all() == 3, "its workers start once it is let go: %d", spawns);
}

/* A hold is counted: two statements that copy one template hold it until both end, and a connection or a refused one of the database never lets it go */
static void
test_hold_count(void)
{
	fresh(4);
	sift_maint_db_hold(m, 6);
	sift_maint_db_hold(m, 6);
	sift_maint_db_release(m, 6);
	SIFT_CHECKF(sift_maint_db_held(m, 6, clock_now), "the second hold stands when the first ends");
	sift_maint_db_connected(m, 6);
	SIFT_CHECKF(sift_maint_db_held(m, 6, clock_now), "a connection of the database keeps the hold");
	(void) sift_maint_db_connect_failed(m, clock_now, 6);
	clock_now += SIFT_MAINT_CONNECT_BACKOFF_MAX_US + 1;
	SIFT_CHECKF(sift_maint_db_held(m, 6, clock_now), "a refused connection keeps the hold past its backoff");
	for (Oid db = 100; db < 100 + SIFT_MAINT_DBSTATE; db++)
		(void) sift_maint_db_connect_failed(m, clock_now, db);
	SIFT_CHECKF(sift_maint_db_held(m, 6, clock_now), "the backoffs of other databases never take the entry of a hold");
	sift_maint_db_release(m, 6);
	SIFT_CHECKF(!sift_maint_db_held(m, 6, clock_now), "the last hold that ends lets the database go");
}

/* Dynamic mode: the first request found no worker slot; the same request again starts one, though it coalesces */
static void
test_coalesced_kick(void)
{
	SiftMaintReq r = req(5, 1, SIFT_MK_SLAB_FLUSH);

	fresh(4);
	spawn_ok = false;
	SIFT_CHECK(sift_maint_request(m, &host, SIFT_MAINT_MODE_DYNAMIC, &r) == SIFT_ENQ_QUEUED);
	SIFT_CHECK(spawns == 1);
	spawn_ok = true;
	(void) sift_maint_request(m, &host, SIFT_MAINT_MODE_DYNAMIC, &r);
	SIFT_CHECKF(spawns == 2, "a coalesced request with no worker for its database starts one: %d spawns", spawns);
	(void) sift_maint_request(m, &host, SIFT_MAINT_MODE_DYNAMIC, &r);
	SIFT_CHECKF(spawns == 2, "and none once one is there: %d spawns", spawns);
}

/* A step that found its lock busy is due again within SIFT_MAINT_DEFER_US: no kick starts a worker for it before then, so the worker of its database waits for it; a failure backed off past that does not hold one */
static void
test_busy_keeps_worker(void)
{
	SiftMaintClaim c;

	fresh(4);
	(void) enq(5, 1, SIFT_MK_COMPACT_PART);
	SIFT_CHECK(sift_maint_claim(m, &host, 5, true, &c));
	SIFT_CHECK(sift_maint_complete(m, &host, &c, SIFT_MSTEP_BUSY));
	SIFT_CHECKF(!sift_maint_has_work(m, &host, 5, true) && sift_maint_kick(m, &host) == SIFT_KICK_NONE, "the busy request is not runnable yet: nothing starts a worker");
	SIFT_CHECKF(sift_maint_due_soon(m, &host, 5) && !sift_maint_due_soon(m, &host, 6), "it is due soon in its database only");
	clock_now += SIFT_MAINT_BUSY_US;
	SIFT_CHECKF(sift_maint_has_work(m, &host, 5, true), "the worker that waited claims it");
	for (int i = 0; i < 12; i++)
	{
		SIFT_CHECK(sift_maint_claim(m, &host, 5, true, &c));
		SIFT_CHECK(sift_maint_complete(m, &host, &c, SIFT_MSTEP_FAILED));
		clock_now += sift_maint_backoff_us(m, m->slot[c.slot].failures);
	}
	SIFT_CHECK(sift_maint_claim(m, &host, 5, true, &c));
	SIFT_CHECK(sift_maint_complete(m, &host, &c, SIFT_MSTEP_FAILED));
	SIFT_CHECKF(!sift_maint_due_soon(m, &host, 5), "a request backed off for its failures keeps no worker");
}

/* A queued tail shrink (P3) asked for again at P0 is an upgrade the launcher is woken for */
static void
test_upgrade(void)
{
	SiftMaintReq r = req(5, 1, SIFT_MK_TAIL_SHRINK);

	fresh(4);
	SIFT_CHECK(sift_maint_enqueue(m, &host, &r) == SIFT_ENQ_QUEUED);
	SIFT_CHECK(sift_maint_enqueue(m, &host, &r) == SIFT_ENQ_COALESCED);
	r.tier = SIFT_TIER_P0;
	SIFT_CHECKF(sift_maint_enqueue(m, &host, &r) == SIFT_ENQ_UPGRADED, "a raised tier is an upgrade");
	SIFT_CHECK(sift_maint_enqueue(m, &host, &r) == SIFT_ENQ_COALESCED);
}

/* 1000 partitions times three kinds fit; the request refused when the queue is full is queued again when a dropped table gives its slots back */
static void
test_capacity_and_drop(void)
{
	int			full = 0;
	SiftMaintReq late = req(7, 99999, SIFT_MK_COMPACT_PART);

	fresh(4);
	for (Oid rel = 1; rel <= 1000; rel++)
		for (int k = SIFT_MK_SLAB_FLUSH; k <= SIFT_MK_COMPACT_PART; k++)
			full += enq(7, rel, k) == SIFT_ENQ_FULL;
	SIFT_CHECKF(full == 0, "3000 requests fit the queue: %d refused", full);
	for (Oid rel = 1001; m->nqueued < SIFT_MAINT_SLOTS; rel++)
		(void) enq(7, rel, SIFT_MK_SLAB_FLUSH);
	SIFT_CHECK(sift_maint_enqueue(m, &host, &late) == SIFT_ENQ_FULL);
	sift_maint_rel_purge(m, 7, 1);
	SIFT_CHECKF(m->nqueued == SIFT_MAINT_SLOTS - 2, "the dropped relation leaves the queue and the refused request comes back: %u queued", m->nqueued);
	SIFT_CHECKF(sift_maint_enqueue(m, &host, &late) == SIFT_ENQ_COALESCED, "the refused request is queued again");
}

int
main(void)
{
	test_busy_db();
	test_held_db();
	test_hold_count();
	test_coalesced_kick();
	test_busy_keeps_worker();
	test_upgrade();
	test_capacity_and_drop();
	free(m);
	return sift_check_summary();
}
