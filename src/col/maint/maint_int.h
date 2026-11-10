/*
 * col/maint/maint_int.h - what the files of the maintenance share
 * the statics one file of the maintenance calls from another
 */
#ifndef SIFT_COL_MAINT_INT_H
#define SIFT_COL_MAINT_INT_H

#include "col/build.h"
#include "col/conn.h"
#include "col/scope.h"
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
	bool		stale[SIFT_MAX_SEGMENTS];	/* a segment of a connected index whose found flags a rewrite should fill again (sift_conn_stale) */
	bool		kover[SIFT_MAX_SEGMENTS];	/* a partition-sized segment of a key-cut index whose key range overlaps another one's: it is no partition of the key */
	int			nkover;
	int64		klo[SIFT_MAX_SEGMENTS],		/* the range of the cut leaf over the units of each segment of a key-cut index in partitions (klo > khi: no value) */
				khi[SIFT_MAX_SEGMENTS];
} RewriteView;

extern int maint_merge_workers;
extern uint64 maint_pass_bytes;
extern bool maint_after_build;
extern bool maint_redecided;

extern const SiftMaintOps maint_col_ops;

#define MAINT_MEM_RATIO	2.1	/* the memory a writer estimates (sift_colw_memory) for each byte of segment, usually: a partition is under half of the memory a writer may estimate */
extern void	maint_use_partitions(Relation index, const SiftDirSnapshot *snap);
extern int	maint_part_kb_for(uint64 bytes, double mem_ratio);

extern bool	maint_is_partition(const SiftSegEntry *e);
extern SbSpans	maint_seg_spans(SiftColSegment *seg);
extern bool	maint_share_superblock(const SbSpans *a, const SbSpans *b, int allowed);

/* The overlays of a key-cut index in partitions: what a merge of the small ones into one would rewrite and spare the reads (maint_overlay_set) */
typedef struct OverlayEv
{
	int			n;				/* the overlays taken */
	bool		forced;			/* more overlays alive than the reads are to open: taken whatever their sizes */
	uint64		rows;			/* their rows: what the merge rewrites */
	uint64		units;			/* their units: the rows of one is what a read of a key in it scans */
	double		sum;			/* the width of the key ranges of their units, each overlay's added: the units a read of a random key opens in them */
	double		cover;			/* the width of the key ranges of their units in all: the units it would open in the merged one */
} OverlayEv;

extern bool	maint_overlay_set(Relation index, RewriteView *v, bool *take, OverlayEv *ev);
/* A segment is an overlay when it holds under half of the rows of the largest of the index (the size rule of maint_overlay_set): it is written lean, and gets no net sidecar */
#define maint_is_overlay(rows, largest_rows)	((uint64) (rows) * 2 < (uint64) (largest_rows))
/*
 * The overlays of an index whose order arrays are the choice of the build (order_array_columns 'auto: ..'; one that names them keeps them everywhere, a group walk reads every segment's) keep none (they span the rows of the whole index, as the point directories do) while all of them together, with the one in hand, hold at most
 * a tenth of the rows of the index (the arrays of the larger segments cover at least 0.9 of the values, which the order planner asks, agg/order/shape.c) and no more rows than a walk takes
 * from them in memory (SIFT_CGORD_FREE_CAP of the tightest leaf with an array): past it the walk would read the units, 20 to 70 times the instructions. The rows of the overlays
 * of seg[], the segments of skip (those the step replaces) left out; the rule of maint_is_overlay.
 */
extern void maint_set_unordered(SiftCPaggSpec *spec, const SiftColSchema *schema, uint64 overlay_rows, uint64 index_rows);
extern uint64 maint_overlay_rows(const SiftSegEntry *seg, int n, const SiftSegEntry *skip, int nskip);
/* set by a rule that found the overlays due: the rewrite then takes them alone */
extern bool	maint_overlay_asked;
extern int	maint_overlay_steps;

extern void	maint_throttle(SiftWriteTarget *wt);

/* col/maint/scope.c: the fill and the scope segments of a promotion */
struct SiftScopeFill;
extern SiftColFill *maint_scope_fill_begin(Relation index, MemoryContext cx, uint64 rows, struct SiftScopeFill **sf);
extern int	maint_scope_write(Relation index, SiftWriteTarget *wt, struct SiftScopeFill *sf, SiftColFill *fill, SiftColBatch *const *batches, int nb,
							  uint64 index_rows, int format, MemoryContext cx, SiftSegEntry *out);

extern bool maint_scope_refill(Relation index);
extern bool maint_scope_refill_due(Relation index);
extern bool maint_scope_merge_due(Relation index, bool force);

extern uint32	maint_floor_rows(Relation index);

extern bool	maint_rows_sparse(uint64 rows, uint64 sbs, uint32 unit);

extern bool	maint_unit_floor_applies(uint32 unit, uint64 rows, uint64 sbs);

extern SiftStepResult	maint_promote_next(Relation index, bool propagate, uint64 *busy);

extern uint32	maint_dirty_units(SiftColSegment *seg);

extern void	maint_net_refresh(Relation index);

extern RewriteView *	maint_view_open(Relation index, const SiftDirSnapshot *snap, MemoryContext cx);
extern void	maint_mark_key_overlap(Relation index, RewriteView *v);
extern bool	maint_key_meets(const RewriteView *v, int i);
extern void	maint_seg_key_range(SiftColSegment *seg, int leaf, int64 *lo, int64 *hi);

extern SiftStepResult	maint_merge_next(Relation index, bool force, SiftMergeGoal goal, bool propagate, uint64 *busy);

extern bool	maint_is_col(Relation index);

extern void	maint_bulkdelete_immutable(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats);

extern void	maint_vacuum_log(Relation index);
extern void	maint_bulkdelete_tomb(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats);
extern void	maint_bulkdelete_wbuf(Relation index, const SiftSegEntry *e, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats);

extern void	maint_reap_overflow(Relation index, Relation heap);
extern void	maint_roots_verify(Relation index, Relation heap);

extern bool	maint_vacuum_truncates(Relation index);

extern bool	maint_space_deferred(Relation index);


extern XLogRecPtr	maint_header_lsn(Relation index, const SiftSegEntry *e);

extern bool	maint_wal_budget_spent(const XLogRecPtr *lsn, const uint64 *sbytes, int n, uint64 bytes);

extern const char *	maint_rewrite_check(Relation index, bool automatic, int *gates);

extern const char *	maint_rewrite_due(Relation index, bool automatic);

extern bool	maint_rewrite_all(const char *why);

/* the reads of a backend owe the rewrite (maint_read_due); the worker started for it takes the rewrite as due */
extern bool	maint_read_asked;
extern bool	maint_read_due(Relation index, double owed);
/* the backends that read the open write buffer decoded rows that cost more than its promotion (the worker started for it seals and promotes) */
extern bool	maint_wbuf_read_due(Relation index);

extern void	maint_rewrite_request(Relation index);

#endif