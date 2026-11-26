/*
 * agg/gpu/slot.c - the slots of the GPU worker, each move of one on its generation (agg/gpu/slot.h).
 */
#include "postgres.h"

#include <errno.h>
#include <signal.h>

#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "agg/gpu/slot.h"

#define WAIT_SLICE_MS			5
/* A backend that finds every slot busy waits this long, then gives its job to the CPU: a slot is held for a whole job, often longer */
#define SLOT_WAIT_MS			20

bool
sift_gpu_worker_alive(const SiftGpuShm *shm)
{
	uint32		pid = pg_atomic_read_u32((pg_atomic_uint32 *) &shm->worker_pid);

	return pid != 0 && (kill((pid_t) pid, 0) == 0 || errno != ESRCH);
}

bool
sift_gpu_worker_fresh(SiftGpuShm *shm)
{
	return (uint64) GetCurrentTimestamp() - pg_atomic_read_u64(&shm->heartbeat_us) < (uint64) SIFT_GPU_STALE_US || sift_gpu_worker_alive(shm);
}

bool
sift_gpu_slot_move(SiftGpuSlot *s, uint32 gen, SiftGpuSlotState from, SiftGpuSlotState to)
{
	uint64		w = sift_gpu_slot_word(gen, from);

	return pg_atomic_compare_exchange_u64(&s->word, &w, sift_gpu_slot_word(gen, to));
}

void
sift_gpu_slot_set(SiftGpuSlot *s, uint32 gen, SiftGpuSlotState st)
{
	pg_write_barrier();
	pg_atomic_write_u64(&s->word, sift_gpu_slot_word(gen, st));
}

void
sift_gpu_slot_wake(const SiftGpuSlot *s)
{
	SetLatch(&GetPGProcByNumber(s->owner_procno)->procLatch);
}

void
sift_gpu_slot_finish(SiftGpuSlot *s, uint32 gen, SiftGpuSlotState from)
{
	if (!sift_gpu_slot_move(s, gen, from, SIFT_GPU_S_DONE))
		return;
	sift_gpu_slot_wake(s);
	if (sift_gpu_slot_cancelled(s, gen))
		(void) sift_gpu_slot_move(s, gen, SIFT_GPU_S_DONE, SIFT_GPU_S_FREE);
}

/* The owner of (gen) has exited: owner_gen says the pid is the one of that generation, not of the owner before it */
static bool
owner_gone(const SiftGpuSlot *s, uint32 gen)
{
	int32		pid;

	pg_read_barrier();
	if (s->owner_gen != gen)
		return false;
	pg_read_barrier();
	pid = s->owner_pid;
	return pid != 0 && kill((pid_t) pid, 0) != 0 && errno == ESRCH;
}

static bool
try_claim(SiftGpuSlot *s, uint32 *gen)
{
	uint64		w = sift_gpu_slot_read(s);
	SiftGpuSlotState st = sift_gpu_slot_state(w);

	if ((st == SIFT_GPU_S_CLAIMED || st == SIFT_GPU_S_DONE) && owner_gone(s, sift_gpu_slot_gen(w)))
		(void) sift_gpu_slot_move(s, sift_gpu_slot_gen(w), st, SIFT_GPU_S_FREE);
	w = sift_gpu_slot_word(sift_gpu_slot_gen(w), SIFT_GPU_S_FREE);
	*gen = sift_gpu_slot_gen(w) + 1 == 0 ? 1 : sift_gpu_slot_gen(w) + 1;
	if (!pg_atomic_compare_exchange_u64(&s->word, &w, sift_gpu_slot_word(*gen, SIFT_GPU_S_CLAIMED)))
		return false;
	s->owner_pid = MyProcPid;
	s->owner_procno = MyProcNumber;
	pg_write_barrier();
	s->owner_gen = *gen;
	return true;
}

int
sift_gpu_slot_claim(SiftGpuShm *shm, uint32 *gen)
{
	for (int waited = 0; waited < SLOT_WAIT_MS; waited += WAIT_SLICE_MS)
	{
		for (int i = 0; i < SIFT_GPU_SLOTS; i++)
			if (try_claim(&shm->slot[i], gen))
				return i;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, WAIT_SLICE_MS, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
	return -1;
}

bool
sift_gpu_slot_wait(SiftGpuShm *shm, SiftGpuSlot *s, uint32 gen)
{
	for (;;)
	{
		if (sift_gpu_slot_read(s) == sift_gpu_slot_word(gen, SIFT_GPU_S_DONE))
		{
			pg_read_barrier();
			return true;
		}
		if (pg_atomic_read_u32(&shm->worker_state) != SIFT_GPU_W_READY || !sift_gpu_worker_fresh(shm))
			return false;
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, WAIT_SLICE_MS, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * The worker frees a job it takes or finishes after the cancel; the backend frees what the worker will not move again: a slot it never submitted, a job done, a job no live worker took. A
 * RUNNING job is the worker's to free, even of a worker that looks gone (its exit finishes every job it holds).
 */
void
sift_gpu_slot_abandon(SiftGpuShm *shm, SiftGpuSlot *s, uint32 gen)
{
	uint64		w;
	SiftGpuSlotState st;

	(void) pg_atomic_exchange_u32(&s->cancelled, gen);
	w = sift_gpu_slot_read(s);
	st = sift_gpu_slot_state(w);
	if (sift_gpu_slot_gen(w) != gen)
		return;
	if (st == SIFT_GPU_S_CLAIMED || st == SIFT_GPU_S_DONE || (st == SIFT_GPU_S_SUBMITTED && !sift_gpu_worker_alive(shm)))
		(void) sift_gpu_slot_move(s, gen, st, SIFT_GPU_S_FREE);
	else
		SetLatch(&GetPGProcByNumber(shm->worker_procno)->procLatch);
}
