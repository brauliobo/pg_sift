/*
 * col/maint/maint_int.h - what the files of the maintenance share
 * the statics one file of the maintenance calls from another
 */
#ifndef SIFT_COL_MAINT_INT_H
#define SIFT_COL_MAINT_INT_H

#include "col/build.h"
#include "bm25/maint.h"

/*
 * col/maint/maint.c - promotion, merge, VACUUM, the maintenance worker and the SQL functions of sift_col indexes.
 * The lock, the directory switch, the room, back-pressure, VACUUM and SQL control flow and the sweep are those of bm25/maint.c
 * (sift_mops_*); what this file supplies in col_ops is a column segment: how a sealed one is promoted and how segments are merged.
 *
 * Intentional differences from the BM25 maintenance (bm25/maint.c), kept so the behaviour of sift_col stays what it was:
 * - a step holds the maintenance lock exclusively, not in RowExclusiveLock with claims on its segments: one step of the index at a time;
 * - a step is not run in a subtransaction: an ERROR goes on to the caller (a worker records it, worker_failed, for the backoff of the shared
 *   flow and sift.workers; the other callers see it) and there is no segment another step could have claimed (busy stays 0);
 * - the policy has no goals: background, VACUUM and explicit merges pick alike; VACUUM merges until the policy asks for none or the rewrite limit is reached;
 * - the worker runs in one transaction until nothing is left to do; VACUUM merges to a fixed point only up to sift.vacuum_rewrite_limit.
 */

#define corrupt SIFT_CORRUPT

#define MAX_PARTS		64		/* segments one merge cuts its output into */

#define PART_DIVISOR	48		/* the memory bound leaves a key-cut index (partitions of three quarters of the size) under MAX_PARTS of them */

extern int maint_part_kb;

/* The superblocks a segment holds rows of (sift_cseg_sb_spans) */
typedef struct SbSpans
{
	uint32		n;
	const SiftCSbSpan *r;
	bool		by_tid;			/* the units are cut by TID range (sift.col_unit_floor) */
} SbSpans;

/*
 * The immutable segments of a directory, open, and which of them are overlays: those under half of sift.col_auto_merge_partition_kb, and any that holds a unit
 * of a superblock another holds with more rows per unit (a promotion of rows into the holes of old pages is small in rows but has a unit in each superblock it
 * reaches, and its fixed bytes can make it as large as a partition). Without partitions every segment is an overlay.
 */
typedef struct RewriteView
{
	int			n;
	SiftSegEntry imm[SIFT_MAX_SEGMENTS];
	SiftColSegment *seg[SIFT_MAX_SEGMENTS];
	SbSpans		span[SIFT_MAX_SEGMENTS];
	bool		over[SIFT_MAX_SEGMENTS];
} RewriteView;

extern int maint_merge_workers;

extern const SiftMaintOps maint_col_ops;

extern void	maint_use_partitions(const SiftDirSnapshot *snap);

extern bool	maint_is_partition(const SiftSegEntry *e);

extern void	maint_throttle(SiftWriteTarget *wt);

extern uint32	maint_floor_rows(Relation index);

extern bool	maint_rows_sparse(uint64 rows, uint64 sbs, uint32 unit);

extern bool	maint_unit_floor_applies(uint32 unit, uint64 rows, uint64 sbs);

extern SiftStepResult	maint_promote_next(Relation index, bool propagate, uint64 *busy);

extern uint32	maint_dirty_units(SiftColSegment *seg);

extern RewriteView *	maint_view_open(Relation index, const SiftDirSnapshot *snap, MemoryContext cx);

extern SiftStepResult	maint_merge_next(Relation index, bool force, SiftMergeGoal goal, bool propagate, uint64 *busy);

extern bool	maint_is_col(Relation index);

extern void	maint_bulkdelete_immutable(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats);

extern void	maint_bulkdelete_wbuf(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats);

extern void	maint_reap_overflow(Relation index, Relation heap);

extern bool	maint_vacuum_truncates(Relation index);

extern bool	maint_space_deferred(Relation index);

extern XLogRecPtr	maint_header_lsn(Relation index, const SiftSegEntry *e);

extern bool	maint_wal_budget_spent(const XLogRecPtr *lsn, const uint64 *sbytes, int n, uint64 bytes);

extern const char *	maint_rewrite_check(Relation index, bool automatic, int *gates);

extern const char *	maint_rewrite_due(Relation index, bool automatic);

extern bool	maint_rewrite_all(const char *why);

extern void	maint_rewrite_request(Relation index);

#endif