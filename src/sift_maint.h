/*
 * sift_maint.h - promotion, merge, recycle, orphan sweep and the async
 * maintenance worker. Every entry point takes the
 * maintenance lock (LockPage on SIFT_MAINT_LOCK_BLKNO): wait = false uses
 * ConditionalLockPage and returns false when busy.
 */
#ifndef SIFT_MAINT_H
#define SIFT_MAINT_H

#include "utils/rel.h"

/* Called by aminsert after an append crossed mutable_limit */
extern void sift_maint_after_seal(Relation index);

/* Promote every sealed mutable segment; returns the number promoted */
extern int	sift_maint_promote(Relation index, bool wait);

/*
 * Waits for the maintenance lock and merges until the directory has room for
 * new segments; false when it cannot get any. Called without index locks held.
 */
extern bool sift_maint_make_room(Relation index);

/* One tiered merge step if the policy asks; returns true if merged */
extern bool sift_maint_merge_step(Relation index, bool wait);

/* Returns blocks returned to the free list */
extern int64 sift_maint_orphan_sweep(Relation index);

/* Launches the dynamic background worker for this index (no preload needed) */
extern bool sift_maint_request_async(Relation index);

/* Worker entry point, resolved by name from pg_sift.so; main_arg = index OID */
extern PGDLLEXPORT void sift_maint_worker_main(Datum main_arg);

#endif							/* SIFT_MAINT_H */
