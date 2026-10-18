/*
 * sift_col_build.h - the build of a sift_col index in two files (docs/COL.md 2.6): sift_col_build.c turns the rows of heap pages into
 * batches and the batches into segments (a ColBuild is one participant) and runs the serial build; sift_col_build_par.c shares the
 * superblocks of the table between the participants of a parallel build.
 */
#ifndef SIFT_COL_BUILD_H
#define SIFT_COL_BUILD_H

#include "access/genam.h"
#include "nodes/execnodes.h"
#include "port/atomics.h"
#include "utils/snapshot.h"

#include "sift_col_am.h"
#include "sift_col_seg.h"

#define SIFT_COL_PHASE_SCAN		2
#define SIFT_COL_PHASE_WRITE	3

/* The segments a build may write: the directory keeps room for the mutable ones and a merge */
#define SIFT_COL_BUILD_SEGMENTS	(SIFT_MAX_SEGMENTS - 7)

typedef struct ColBuild ColBuild;

/*
 * segs has room for maxsegs entries. A segment is cut when its writer holds more than budget bytes, except the last one that
 * maxsegs allows. done != NULL: a participant of a parallel build, which writes through a SHARED target, takes the generations of its
 * segments from the metapage and adds the rows it indexed to *done; only the leader reports progress then.
 */
extern ColBuild *sift_colb_begin(Relation index, SiftWriteTarget *wt, Size budget, int maxsegs, SiftSegEntry *segs, pg_atomic_uint64 *done,
								 bool leader);
/* Scans the whole heap (serial build) or the superblock sb of nblocks heap blocks (parallel); both return the heap tuples scanned */
extern double sift_colb_scan(ColBuild *b, Relation heap, IndexInfo *info);
extern double sift_colb_scan_sb(ColBuild *b, Relation heap, IndexInfo *info, Snapshot snapshot, uint32 sb, BlockNumber nblocks);
/* Writes what is left; returns the number of segments, in segs */
extern int	sift_colb_end(ColBuild *b);
extern double sift_colb_rows(const ColBuild *b);
extern void sift_colb_free(ColBuild *b);
/* Publishes the segments of a build in the directory, ordered by their first superblock */
extern void sift_colb_publish(Relation index, SiftSegEntry *segs, int nsegs);

/* sift_col_build_par.c: the whole build with workers, NULL when none could be launched (nothing is written then) */
extern IndexBuildResult *sift_colp_build(Relation heap, Relation index, IndexInfo *info);

#ifdef SIFT_TESTING
/*
 * Parallel workers the last sift_col build of this session launched; the superblock (1 = the first) whose participant fails (0 = none);
 * kB the writer of a segment holds before the segment is cut (0 = by maintenance_work_mem)
 */
extern PGDLLIMPORT int sift_test_col_workers;
extern PGDLLIMPORT int sift_test_col_fail_sb;
extern PGDLLIMPORT int sift_test_col_segment_kb;
#endif

#endif							/* SIFT_COL_BUILD_H */
