/*
 * core/maint/int.h - shared between the maintenance files (maint.c, maint_cut.c, maint_vacuum.c, space.c and the steps of a kind, bm25/step.c).
 */
#ifndef SIFT_MAINT_INT_H
#define SIFT_MAINT_INT_H

#include "core/maint/maint.h"

/* inputs of one merge: its output (runs grow with a stream, core/writer.c) stays well within the extents a header page lists */
#define MERGE_MAX_BYTES		(UINT64CONST(16) << 30)

#ifdef SIFT_TESTING
#define merge_max_bytes()	(sift_test_merge_max_kb > 0 ? (uint64) sift_test_merge_max_kb * 1024 : MERGE_MAX_BYTES)
#else
#define merge_max_bytes()	MERGE_MAX_BYTES
#endif

#define ROOM_SPARE			16		/* directory entries below the limit from which inserters merge */
#define ROOM_SEGMENTS		(SIFT_DIR_POLICY_MAX - ROOM_SPARE)
#define SELF_PROMOTE_SEALED	4		/* sealed segments from which an inserter promotes them itself */

typedef void (*StepFn) (Relation index, SiftWriteTarget *wt, const void *arg);

/* the claims, steps and backoff of step.c and maint.c that space.c and the worker also use */
extern uint64 maint_written_bytes(void);
extern bool maint_lock_mode(Relation index, bool wait, LOCKMODE mode);
extern void maint_unlock_mode(Relation index, LOCKMODE mode);
extern bool claim(Relation index, uint64 gen, bool wait);
extern void unclaim(Relation index, uint64 gen);
extern SiftStepResult run_step(Relation index, bool propagate, StepFn fn, const void *arg);
/* The relocation of one segment (core/space/move.c), run as a step by the space step (core/space/space.c) */
typedef struct SpaceMove
{
	const SiftSegEntry *entry;
	SiftExtent *release;
	int			nrelease;
	BlockNumber limit;
	uint64		budget;
	uint64		written;
	BlockNumber header;
	SiftRelocatePage relocate_page;
	uint64		recut;			/* testing: the units the move treats as cut again */
} SpaceMove;
extern void sift_space_move(Relation rel, SiftWriteTarget *wt, const void *arg);
extern int64 backoff_ms(const SiftMetaPageData *meta);
extern bool in_backoff(const SiftMetaPageData *meta);
extern int promote_locked(const SiftMaintOps *ops, Relation index, bool seal_active, bool propagate);
extern uint64 online_shrink(const SiftMaintOps *ops, Relation index, int elevel);
extern void online_fallback(const SiftMaintOps *ops, Relation index, int elevel);
extern void cut_free_end(const SiftMaintOps *ops, Relation index, bool released);
extern SiftStepResult promote_next(Relation index, bool propagate, uint64 *busy);

/* The INFO lines of the cut and the shrink of the free end of a file (space_say.c); a skipped cut returns what kept it, for the metapage */
typedef enum CutSkip
{
	SKIP_STANDBY,
	SKIP_LOCK,
	SKIP_SNAPSHOT,
	SKIP_FALLBACK
} CutSkip;
extern SiftShrinkBlock sift_cut_say_skipped(Relation rel, const char *what, int elevel, CutSkip why, int64 blocks, int waited, int64 more);
extern void sift_cut_say_truncated(Relation rel, const char *what, int elevel, BlockNumber freed, bool online);
extern void sift_cut_say_relocated(Relation rel, int elevel, uint64 bytes);
extern SiftStepResult merge_next(Relation index, bool force, SiftMergeGoal goal, bool propagate, uint64 *busy);

#endif							/* SIFT_MAINT_INT_H */
