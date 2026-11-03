/*
 * bm25/build.h - the build of a sift index; the inverted buffer and the sorted runs it uses, which promotion and merge share
 * with it, are in bm25/runs.h.
 */
#ifndef SIFT_BUILD_H
#define SIFT_BUILD_H

#include "storage/sharedfileset.h"

#include "bm25/runs.h"

/*
 * Merges the segments in[0..n) by the process and max_parallel_maintenance_workers workers (merge_ranges.c) into neu: 1 written, 0 no live
 * doc, -1 not done, which one process then merges.
 */
extern int	sift_merge_ranges(Relation index, SiftWriteTarget *wt, const SiftSegEntry *in, int n, const SiftScoreRef *ref, SiftSegEntry *neu);

#ifdef SIFT_TESTING
/* kB: the most memory a CREATE INDEX of this backend allocated since the variable was last set (to 0) */
extern PGDLLIMPORT int sift_test_build_peak;
/* bytes of the text of a heap page the build holds (0 = by memory) */
extern PGDLLIMPORT int sift_test_page_text;
/* the nth merge of a CREATE INDEX (0 = none) is cancelled after a few terms: in the leader or a serial build (1), the workers (2), any (0) */
extern PGDLLIMPORT int sift_test_fail_merge;
extern PGDLLIMPORT int sift_test_fail_who;
/* a CREATE INDEX waits to enter each progress phase (2 scan, 3 spill, 4 merge, 5 write) for the advisory lock this + the phase (0 = no gate) */
extern PGDLLIMPORT int sift_test_phase_gate;
extern void sift_test_phase_wait(int phase);
/* ranges of terms the participants of the last parallel CREATE INDEX of this backend wrote (0 = none) */
extern PGDLLIMPORT int sift_test_range_pieces;
#endif

#endif							/* SIFT_BUILD_H */
