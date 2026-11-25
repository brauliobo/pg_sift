/*
 * shm_port.h - the five host services the shared-memory write layer needs: a lock, a wake-up, a wait, interrupt holding and
 * error cleanup. The server build maps them to LWLock and latches; SIFT_SHM_STANDALONE maps them to pthreads so the same
 * combiner code runs under a multi-threaded sanitizer test without a server.
 */
#ifndef SIFT_SHM_PORT_H
#define SIFT_SHM_PORT_H

#include "postgres.h"

#include "port/atomics.h"
#include "port/pg_bitutils.h"

#ifdef SIFT_SHM_STANDALONE

#include <pthread.h>
#include <semaphore.h>

#define SIFT_SHM_MAX_PROCS	128

typedef pthread_mutex_t SiftShmLock;

extern sem_t sift_shm_sems[SIFT_SHM_MAX_PROCS];
extern __thread int sift_shm_procno;

#define SIFT_SHM_BACKEND_LOCAL	static __thread


static inline int
sift_shm_my_procno(void)
{
	return sift_shm_procno;
}

static inline void
sift_shm_lock_init(SiftShmLock *lock, int tranche_id)
{
	(void) tranche_id;
	pthread_mutex_init(lock, NULL);
}

static inline void
sift_shm_lock(SiftShmLock *lock)
{
	pthread_mutex_lock(lock);
}

static inline bool
sift_shm_lock_try(SiftShmLock *lock)
{
	return pthread_mutex_trylock(lock) == 0;
}

static inline void
sift_shm_unlock(SiftShmLock *lock)
{
	pthread_mutex_unlock(lock);
}

static inline void
sift_shm_wake(int procno)
{
	sem_post(&sift_shm_sems[procno]);
}

static inline void
sift_shm_wait(void)
{
	while (sem_wait(&sift_shm_sems[sift_shm_procno]) != 0)
		;
}

static inline void
sift_shm_wait_done(void)
{
}

#define SIFT_SHM_HOLD_INTERRUPTS()
#define SIFT_SHM_RESUME_INTERRUPTS()
#define SIFT_SHM_TRY()		do { if (true) {
#define SIFT_SHM_CATCH()	} else {
#define SIFT_SHM_END_TRY()	} } while (0)
#define SIFT_SHM_RETHROW()	abort()

#else

#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procnumber.h"
#include "utils/wait_event.h"

typedef LWLock SiftShmLock;

#define SIFT_SHM_BACKEND_LOCAL	static

static inline int
sift_shm_my_procno(void)
{
	return MyProcNumber;
}

static inline void
sift_shm_lock_init(SiftShmLock *lock, int tranche_id)
{
	LWLockInitialize(lock, tranche_id);
}

static inline void
sift_shm_lock(SiftShmLock *lock)
{
	LWLockAcquire(lock, LW_EXCLUSIVE);
}

static inline bool
sift_shm_lock_try(SiftShmLock *lock)
{
	return LWLockConditionalAcquire(lock, LW_EXCLUSIVE);
}

static inline void
sift_shm_unlock(SiftShmLock *lock)
{
	LWLockRelease(lock);
}

static inline void
sift_shm_wake(int procno)
{
	SetLatch(&GetPGProcByNumber(procno)->procLatch);
}

static inline void
sift_shm_wait(void)
{
	static uint32 wait_event;

	if (wait_event == 0)
		wait_event = WaitEventExtensionNew("SiftShmCombiner");
	(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0, wait_event);
	ResetLatch(MyLatch);
}

/* ResetLatch above may have swallowed a wake-up meant for the interrupt handlers: hand it back */
static inline void
sift_shm_wait_done(void)
{
	SetLatch(MyLatch);
}

#define SIFT_SHM_HOLD_INTERRUPTS()		HOLD_INTERRUPTS()
#define SIFT_SHM_RESUME_INTERRUPTS()	RESUME_INTERRUPTS()
#define SIFT_SHM_TRY()					PG_TRY()
#define SIFT_SHM_CATCH()				PG_CATCH()
#define SIFT_SHM_END_TRY()				PG_END_TRY()
#define SIFT_SHM_RETHROW()				PG_RE_THROW()

#endif							/* SIFT_SHM_STANDALONE */

#endif							/* SIFT_SHM_PORT_H */
