/*
 * bm25/maint.h - promotion, merge, recycle, orphan sweep and the async
 * maintenance worker (DESIGN.md (c), (i)). Every entry point takes the
 * maintenance lock (LockPage on SIFT_MAINT_LOCK_BLKNO): promotion and merge
 * steps hold it in RowExclusiveLock and claim the segments they replace, so
 * steps on other segments run side by side; wait = false uses ConditionalLockPage and
 * returns false when busy. VACUUM and the SQL functions hold it exclusive.
 *
 * The control flow is written once (sift_mops_*, below) and parametrized by SiftMaintOps: the BM25 index
 * (bm25/maint.c) and the sift_col index (col/maint.c) each supply what a segment is read and written with.
 */
#ifndef SIFT_MAINT_H
#define SIFT_MAINT_H

#include "access/genam.h"
#include "datatype/timestamp.h"
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

#include "core/store.h"

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

/* What a merge of in[0..n) replaces: their generations, their extents (to release) and their bytes */
typedef struct SiftMaintInputs
{
	uint64	   *gens;
	SiftExtent *release;
	int			nrelease;
	uint64		bytes;
} SiftMaintInputs;

extern SiftMaintInputs sift_maint_inputs(Relation index, const SiftSegEntry *in, int n);

/*
 * The directory switch that ends a promotion or a merge, in one place: old[0..nold) is replaced by neu (NULL: by nothing),
 * release[0..nrelease) is released, and the step is logged (what, docs; the bytes are those of neu)
 */
extern void sift_maint_replace(Relation index, const uint64 *old, int nold, const SiftSegEntry *neu, SiftExtent *release,
							   int nrelease, const char *what, TimestampTz start, int64 docs);

/* Registers a dynamic background worker running function in pg_sift for this index */
extern bool sift_maint_request_worker(Relation index, const char *function);

/* The common start of a worker: context of its errors, signals, connection to the database of the request; returns the index OID */
extern Oid	sift_maint_worker_connect(Datum main_arg, ErrorContextCallback *errctx);

/* The outcome of one maintenance step */
typedef enum SiftStepResult
{
	SIFT_STEP_IDLE,				/* nothing to do */
	SIFT_STEP_DONE,
	SIFT_STEP_FAILED			/* rolled back, its blocks returned, reported and counted */
} SiftStepResult;

/* Who asks for a merge: any (explicit, or for room in a full directory), the background (worker), VACUUM */
typedef enum SiftMergeGoal
{
	SIFT_GOAL_ANY,
	SIFT_GOAL_BACKGROUND,
	SIFT_GOAL_VACUUM
} SiftMergeGoal;

/*
 * The policy of one kind of index. The steps are the whole difference between the kinds: promote_next promotes the oldest sealed
 * segment and merge_next merges the segments the merge policy picks (both one step, STEP_IDLE when there is none, busy: the generation
 * of a segment another step has claimed, 0 if none; propagate: an ERROR goes on to the caller instead of being counted for the backoff).
 */
typedef struct SiftMaintOps
{
	const char *name;			/* of the index kind in messages: "sift", "sift_col" */
	const char *worker;			/* function of pg_sift that is the background worker */
	LOCKMODE	step_lock;			/* the maintenance lock taken for room, promotion and merge steps */
	bool		vacuum_to_fixed_point;	/* VACUUM merges until the policy asks for no more, else one merge */
	bool		vacuum_budget;	/* ... but stops starting merges once sift.vacuum_rewrite_limit megabytes were rewritten (the first merge always runs) */
	bool		force_seals;	/* a forced SQL merge first seals the open write buffer, so its rows are in the one segment it makes */
	bool		(*is_index) (Relation index);
	uint64		(*seal) (Relation index, bool force);
	SiftStepResult (*promote_next) (Relation index, bool propagate, uint64 *busy);
	SiftStepResult (*merge_next) (Relation index, bool force, SiftMergeGoal goal, bool propagate, uint64 *busy);
	void		(*bulkdelete_immutable) (Relation index, const SiftSegEntry *seg, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats);
	void		(*bulkdelete_wbuf) (Relation index, const SiftSegEntry *seg, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats);
	bool		(*vacuum_truncate) (Relation index);	/* NULL: the file is not truncated by VACUUM */
	uint64		(*vacuum_seal) (Relation index);	/* NULL: VACUUM promotes sealed write buffers only; else it first seals an open one this worth sealing */
	void		(*vacuum_refresh) (Relation index);	/* NULL: nothing to refresh after VACUUM's merges (the maintenance lock is held) */
	void		(*vacuum_skipped) (Relation index, Relation heap);	/* NULL: nothing; VACUUM skipped the vacuuming of the indexes (no ambulkdelete), so rows the heap dropped may be live in the index (the maintenance lock is held) */
	const char *(*rewrite_due) (Relation index, bool automatic);	/* NULL: the kind has no rewrite rule; else why the index is worth a rewrite of all its segments now, or NULL (the maintenance lock is held) */
	void		(*vacuum_done) (Relation index);	/* NULL: nothing; called at the end of VACUUM's cleanup, the maintenance lock released */
} SiftMaintOps;

/* Called by aminsert after an append crossed write_buffer_limit */
extern void sift_mops_after_seal(const SiftMaintOps *ops, Relation index);
extern void sift_maint_after_seal(Relation index);

/* Promote every sealed write buffer; returns the number promoted */
extern int	sift_mops_promote(const SiftMaintOps *ops, Relation index, bool wait);

/*
 * Waits for the maintenance lock and merges until the directory has room for
 * new segments; false when it cannot get any. Called without index locks held.
 */
extern bool sift_mops_make_room(const SiftMaintOps *ops, Relation index);
extern bool sift_maint_make_room(Relation index);

/* One tiered merge step if the policy asks; returns true if merged */
extern bool sift_mops_merge_step(const SiftMaintOps *ops, Relation index, bool wait);

/* The VACUUM callbacks, and the SQL functions promote and merge (the lock and the index are the caller's: sift_mops_open_maintain) */
extern IndexBulkDeleteResult *sift_mops_bulkdelete(const SiftMaintOps *ops, IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
												   IndexBulkDeleteCallback callback, void *callback_state);
extern IndexBulkDeleteResult *sift_mops_vacuumcleanup(const SiftMaintOps *ops, IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
extern Relation sift_mops_open(const SiftMaintOps *ops, Oid indexoid, LOCKMODE lock);
extern Relation sift_mops_open_maintain(const SiftMaintOps *ops, Oid indexoid);
extern int	sift_mops_sql_promote(const SiftMaintOps *ops, Relation index);
extern int	sift_mops_sql_merge(const SiftMaintOps *ops, Relation index, bool force);

/*
 * The rewrite of an index that churn left scattered (ops->rewrite_due): seals the open write buffer, promotes, and merges every segment into
 * one. The maintenance lock is not waited for: when a VACUUM, a merge or the worker holds it, nothing is done. automatic: the call is the
 * server's own (the limit on the bytes rewritten applies). Returns the merges made, -1 when nothing was due or the lock was busy.
 */
extern int	sift_mops_rewrite(const SiftMaintOps *ops, Relation index, bool automatic);

/* Like sift_maint_request_worker, but a request is not dropped for coming within the naptime of another: the worker itself leaves when it has nothing to do */
extern bool sift_maint_register_worker(Relation index, const char *function);

/* Returns blocks returned to the free list */
extern int64 sift_maint_orphan_sweep(Relation index);

/*
 * Truncation of the free blocks at the end of the file (core/store.h P7), if they are worth it and
 * AccessExclusiveLock on the index can be had without waiting. Holds the maintenance lock exclusively.
 * Reports at elevel what it did.
 */
extern void sift_maint_truncate(Relation index, int elevel);

/*
 * VACUUM's space step (core/store.h P7): the free blocks of the relation are worth giving back (a heap's thresholds), AccessExclusiveLock is
 * free right now, and then, with that lock keeping every reader and writer out and every released extent reusable at once
 * (sift_store_space_exclusive), the segments that end the file are written again into the free blocks below them (at most sift.vacuum_rewrite_limit megabytes in all, so the writes under the lock are bounded) and the free end of the file is cut.
 * The caller holds the maintenance lock exclusively. Reports at elevel what it did.
 */
extern void sift_maint_space(Relation rel, SiftRelocatePage relocate_page, int elevel);

/* Worker entry point, resolved by name from pg_sift.so; main_arg = index OID */
extern PGDLLEXPORT void sift_maint_worker_main(Datum main_arg);

#endif							/* SIFT_MAINT_H */
