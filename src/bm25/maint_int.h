/*
 * bm25/maint_int.h - shared between the maintenance files (maint.c, step.c, space.c).
 */
#ifndef SIFT_MAINT_INT_H
#define SIFT_MAINT_INT_H

#include "bm25/maint.h"

/* inputs of one merge: its output (runs grow with a stream, core/writer.c) stays well within the extents a header page lists */
#define MERGE_MAX_BYTES		(UINT64CONST(16) << 30)

#ifdef SIFT_TESTING
#define merge_max_bytes()	(sift_test_merge_max_kb > 0 ? (uint64) sift_test_merge_max_kb * 1024 : MERGE_MAX_BYTES)
#else
#define merge_max_bytes()	MERGE_MAX_BYTES
#endif

typedef void (*StepFn) (Relation index, SiftWriteTarget *wt, const void *arg);

/* the claims, steps and backoff of step.c and maint.c that space.c and the worker also use */
extern bool claim(Relation index, uint64 gen, bool wait);
extern void unclaim(Relation index, uint64 gen);
extern SiftStepResult run_step(Relation index, bool propagate, StepFn fn, const void *arg);
extern int64 backoff_ms(const SiftMetaPageData *meta);
extern bool in_backoff(const SiftMetaPageData *meta);
extern SiftStepResult promote_next(Relation index, bool propagate, uint64 *busy);
extern SiftStepResult merge_next(Relation index, bool force, SiftMergeGoal goal, bool propagate, uint64 *busy);

#endif							/* SIFT_MAINT_INT_H */
