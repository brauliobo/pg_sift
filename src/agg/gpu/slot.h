/*
 * agg/gpu/slot.h - the life of a slot of the GPU worker (agg/gpu/shm.h): its word is its generation and its state, so that a backend, a worker or a reaper that acts on a slot it read before acts
 * only on the generation it read; a slot that was freed and claimed again in between is someone else's (Spec 111 S111-06/07).
 *
 * A claim makes a new generation; the owner then writes its pid and, after it, the generation that pid is of (owner_gen), so that no other backend reads the pid of an earlier owner as the one of
 * the slot. The job goes into the slot before SUBMITTED, the answer before DONE: every move is a full barrier, and a waiter that sees DONE reads the answer after a read barrier. A backend that
 * leaves marks its generation cancelled, then looks at the state; the worker that finishes a job sets DONE, then looks at cancelled: one of the two sees the other and frees the slot.
 */
#ifndef SIFT_AGG_GPU_SLOT_H
#define SIFT_AGG_GPU_SLOT_H

#include "agg/gpu/shm.h"

static inline uint64
sift_gpu_slot_word(uint32 gen, SiftGpuSlotState st)
{
	return (uint64) gen << 32 | st;
}

static inline uint32
sift_gpu_slot_gen(uint64 word)
{
	return (uint32) (word >> 32);
}

static inline SiftGpuSlotState
sift_gpu_slot_state(uint64 word)
{
	return (SiftGpuSlotState) (word & UINT32_MAX);
}

static inline uint64
sift_gpu_slot_read(const SiftGpuSlot *s)
{
	return pg_atomic_read_u64((pg_atomic_uint64 *) &s->word);
}

static inline bool
sift_gpu_slot_cancelled(const SiftGpuSlot *s, uint32 gen)
{
	return pg_atomic_read_u32((pg_atomic_uint32 *) &s->cancelled) == gen;
}

extern bool sift_gpu_worker_alive(const SiftGpuShm *shm);
extern bool sift_gpu_worker_fresh(SiftGpuShm *shm);

/* The move of the slot from (gen, from) to (gen, to); false when it is no longer there */
extern bool sift_gpu_slot_move(SiftGpuSlot *s, uint32 gen, SiftGpuSlotState from, SiftGpuSlotState to);
/* The owner's own moves (CLAIMED to SUBMITTED, DONE to FREE): nobody else moves the slot of a live owner there */
extern void sift_gpu_slot_set(SiftGpuSlot *s, uint32 gen, SiftGpuSlotState st);

/* ---- the worker ---- */
/* The job of (gen, from) is over, its answer in the slot: the owner is woken, and the slot freed when the owner left */
extern void sift_gpu_slot_finish(SiftGpuSlot *s, uint32 gen, SiftGpuSlotState from);

/* ---- a backend ---- */
/* A free slot claimed for this backend, its generation in *gen; -1 when every slot stays busy for a short wait */
extern int	sift_gpu_slot_claim(SiftGpuShm *shm, uint32 *gen);
/* True when the worker answered the job of (gen); false when it is gone */
extern bool sift_gpu_slot_wait(SiftGpuShm *shm, SiftGpuSlot *s, uint32 gen);
/* The backend leaves the slot of (gen) (an error, a cancel): the worker drops the job and frees it; one that never submitted, or whose worker is gone, frees it itself */
extern void sift_gpu_slot_abandon(SiftGpuShm *shm, SiftGpuSlot *s, uint32 gen);

#endif							/* SIFT_AGG_GPU_SLOT_H */
