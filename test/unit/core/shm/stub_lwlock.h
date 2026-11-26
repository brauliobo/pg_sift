/*
 * test/unit/core/shm/stub_lwlock.h - the LWLocks of the server for the tests of the shared tables that include their source: a spin on the state word, a thread that takes a lock it holds fails
 * the test (a nested writer that the test runs where the code under test holds no lock must find none held), and a table segment that sift_dsm_try / sift_dsm_attach give from the heap.
 */
#ifndef SIFT_TEST_UNIT_STUB_LWLOCK_H
#define SIFT_TEST_UNIT_STUB_LWLOCK_H

#include <sched.h>

#include "test/common/check.h"

ProcNumber	MyProcNumber = 0;

static __thread LWLock *stub_held[4];
static __thread int stub_nheld;

void
LWLockInitialize(LWLock *lock, int tranche_id)
{
	pg_atomic_init_u32(&lock->state, 0);
}

bool
LWLockConditionalAcquire(LWLock *lock, LWLockMode mode)
{
	uint32		free = 0;

	if (!pg_atomic_compare_exchange_u32(&lock->state, &free, 1))
		return false;
	stub_held[stub_nheld++] = lock;
	return true;
}

bool
LWLockAcquire(LWLock *lock, LWLockMode mode)
{
	for (int i = 0; i < stub_nheld; i++)
		SIFT_CHECKF(stub_held[i] != lock, "a thread waits for a lock it holds");
	while (!LWLockConditionalAcquire(lock, mode))
		sched_yield();
	return true;
}

void
LWLockRelease(LWLock *lock)
{
	stub_nheld--;
	pg_atomic_write_u32(&lock->state, 0);
}

#if PG_VERSION_NUM < 190000
int
LWLockNewTrancheId(void)
{
	return 100;
}

void
LWLockRegisterTranche(int tranche_id, const char *tranche_name)
{
}
#endif

static void *
stub_segment(Size size, SiftDsmInit init)
{
	void	   *raw = aligned_alloc(PG_CACHE_LINE_SIZE, TYPEALIGN(PG_CACHE_LINE_SIZE, size));

	SIFT_DSM_INIT_CALL(init, raw);
	return raw;
}

#endif							/* SIFT_TEST_UNIT_STUB_LWLOCK_H */
