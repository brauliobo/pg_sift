/*
 * test/unit/core/f2/pace.c - the ring of the standby-safe horizon (core/f2/pace.c) as a seqlock: the writer makes the version odd, then stores the slots with release (or after a release
 * fence), so that a reader on a weakly ordered CPU (ARM64) that sees a new slot also sees the odd version and discards its copy. The atomics of the source are logged in the order the writer issues them, and a reader
 * that copies the ring while a writer records never gets a sample whose time and horizon are of two records.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#include <pthread.h>

#include "test/common/bench.h"

#include "core/f2/pace.h"

static const void *version_at;
static bool odd,
			fenced;
static int	unfenced_stores;

static bool
logged_cas(const uint32 *p, bool ok)
{
	odd = ok && (*p & 1) != 0;
	fenced = false;
	return ok;
}

static void
logged_fence(int order)
{
	fenced |= order == __ATOMIC_RELEASE || order == __ATOMIC_ACQ_REL || order == __ATOMIC_SEQ_CST;
}

static void
logged_store(const void *p, int order)
{
	if (p == version_at)
		odd = false;
	else if (odd && !fenced && order != __ATOMIC_RELEASE && order != __ATOMIC_SEQ_CST)
		unfenced_stores++;
}

#define __atomic_compare_exchange_n(p, e, d, w, s, f)	logged_cas((p), __atomic_compare_exchange_n(p, e, d, w, s, f))
#define __atomic_thread_fence(o)	(logged_fence(o), __atomic_thread_fence(o))
#define __atomic_store_n(p, v, o)	(logged_store(p, o), __atomic_store_n(p, v, o))

#include "core/f2/pace.c"

#undef __atomic_compare_exchange_n
#undef __atomic_thread_fence
#undef __atomic_store_n

#define STEP_US		((TimestampTz) SIFT_HORIZON_SAMPLE_SECS * USECS_PER_SEC)
#define RECORDS		200000

/* The writer stores no slot of the ring after the odd version without release */
static void
test_slots_are_stored_after_a_release_fence(void)
{
	SiftStandbyHorizon r;

	sift_horizon_ring_init(&r);
	version_at = &r.version;
	for (int i = 1; i <= 3 * SIFT_HORIZON_RING_SLOTS; i++)
		SIFT_CHECKF(sift_horizon_ring_record(&r, i * STEP_US, FullTransactionIdFromU64(i)), "record %d is taken", i);
	SIFT_CHECKF(!sift_horizon_ring_record(&r, 3 * SIFT_HORIZON_RING_SLOTS * STEP_US + 1, FullTransactionIdFromU64(1)), "a record sooner than the sample period is refused");
	SIFT_CHECKF(unfenced_stores == 0, "%d stores of a slot were not ordered after the odd version", unfenced_stores);
	SIFT_CHECKF(r.version % 2 == 0 && r.head == 3 * SIFT_HORIZON_RING_SLOTS, "the ring is even and holds every record (version %u, head %u)", r.version, r.head);
}

static SiftStandbyHorizon shared;
static bool writing;

static void *
writer(void *arg)
{
	for (int i = 1; i <= RECORDS; i++)
		while (!sift_horizon_ring_record(&shared, i * STEP_US, FullTransactionIdFromU64(i)))
			;
	__atomic_store_n(&writing, false, __ATOMIC_RELEASE);
	return NULL;
}

/* A copy of the ring that the version did not discard has each sample whole: the horizon recorded with its time */
static void
test_a_copy_has_no_torn_sample(void)
{
	pthread_t	t;
	int			copies = 0,
				torn = 0;

	sift_horizon_ring_init(&shared);
	version_at = NULL;
	__atomic_store_n(&writing, true, __ATOMIC_RELAXED);
	SIFT_CHECKF(pthread_create(&t, NULL, writer, NULL) == 0, "the writer starts");
	while (__atomic_load_n(&writing, __ATOMIC_ACQUIRE))
	{
		SiftStandbyHorizon c;

		if (!ring_copy(&shared, &c))
			continue;
		copies++;
		for (int i = 0; i < SIFT_HORIZON_RING_SLOTS; i++)
			torn += c.slot[i].ts != (TimestampTz) c.slot[i].horizon * STEP_US;
	}
	pthread_join(t, NULL);
	SIFT_CHECKF(copies > 0 && torn == 0, "%d samples of %d copies were torn", torn, copies);
}

int
main(void)
{
	test_slots_are_stored_after_a_release_fence();
	test_a_copy_has_no_torn_sample();
	return sift_check_summary();
}
