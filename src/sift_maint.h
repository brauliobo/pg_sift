/*
 * sift_maint.h - promotion, merge, recycle, orphan sweep and the async
 * maintenance worker (DESIGN.md (c), (i)). Every entry point takes the
 * maintenance lock (LockPage on SIFT_MAINT_LOCK_BLKNO): promotion and merge
 * steps hold it in RowExclusiveLock and claim the segments they replace, so
 * steps on other segments run side by side; wait = false uses ConditionalLockPage and
 * returns false when busy. VACUUM and the SQL functions hold it exclusive.
 */
#ifndef SIFT_MAINT_H
#define SIFT_MAINT_H

#include "utils/rel.h"

#ifdef SIFT_TESTING
/* Testing knob (sift.test_merge_max_kb, 0 = off): bytes of the segments one merge may read, in kB */
extern PGDLLIMPORT int sift_test_merge_max_kb;
/* Testing knob (sift.test_merge_sleep_ms, 0 = off): a merge of the background worker sleeps this long, its segments claimed */
extern PGDLLIMPORT int sift_test_merge_sleep_ms;
/* Testing knob (sift.test_room_segments, 0 = off): directory entries from which inserters merge */
extern PGDLLIMPORT int sift_test_room_segments;
extern PGDLLIMPORT int64 sift_maint_wait_max_us;
#endif

#include "sift_store.h"

/* The maintenance lock (LockPage on SIFT_MAINT_LOCK_BLKNO), shared by the BM25 and the column maintenance: wait = false returns false when busy */
extern bool sift_maint_lock(Relation index, bool wait);
extern void sift_maint_unlock(Relation index);

/*
 * SiftDirMutator that replaces segments in the directory: the generations old[0..nold) are dropped (their totals leave the metapage) and neu, if any,
 * takes the place of the first of them (its totals enter it)
 */
typedef struct SiftMaintSwitch
{
	const uint64 *old;			/* generations replaced */
	int			nold;
	const SiftSegEntry *neu;	/* the replacement or NULL */
} SiftMaintSwitch;

extern void sift_maint_switch_segments(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg);

/* Registers a dynamic background worker running function in pg_sift for this index (sift_maint_request_async is the BM25 one) */
extern bool sift_maint_request_worker(Relation index, const char *function);

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

/*
 * Truncation of the free blocks at the end of the file (sift_store.h P7), if they are worth it and
 * AccessExclusiveLock on the index can be had without waiting. Holds the maintenance lock exclusively.
 * Reports at elevel what it did.
 */
extern void sift_maint_truncate(Relation index, int elevel);

/* Launches the dynamic background worker for this index (no preload needed) */
extern bool sift_maint_request_async(Relation index);

/* Worker entry point, resolved by name from pg_sift.so; main_arg = index OID */
extern PGDLLEXPORT void sift_maint_worker_main(Datum main_arg);

#endif							/* SIFT_MAINT_H */
