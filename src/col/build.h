/*
 * col/build.h - the build of a sift_col index in two files: col/build.c turns the rows of heap pages into
 * batches and the batches into segments (a ColBuild is one participant) and runs the serial build; col/build_par.c shares the
 * superblocks of the table between the participants of a parallel build.
 */
#ifndef SIFT_COL_BUILD_H
#define SIFT_COL_BUILD_H

#include "access/genam.h"
#include "nodes/execnodes.h"
#include "port/atomics.h"
#include "storage/s_lock.h"
#include "utils/snapshot.h"

#include "col/am.h"
#include "col/enc.h"
#include "col/seg.h"

#define SIFT_COL_PHASE_SCAN		2
#define SIFT_COL_PHASE_WRITE	3

/* The segments a build may write: the directory keeps room for the write buffer and a merge */
#define SIFT_COL_BUILD_SEGMENTS	(SIFT_MAX_SEGMENTS - 7)

/* The writer of a segment of a participant holds up to half of maintenance_work_mem shared by nslots participants, at least this much (the serial build 64 MB) */
#define SIFT_COL_BUILD_MIN_BUDGET	((Size) 16 * 1024 * 1024)
#define SIFT_COL_BUILD_SERIAL_BUDGET ((Size) 64 * 1024 * 1024)

typedef struct ColBuild ColBuild;

extern Size sift_colb_budget(int nslots);
extern void sift_colb_init_meta(Relation index);

/*
 * segs has room for maxsegs entries. A segment is cut when its writer holds more than budget bytes, except the last one that
 * maxsegs allows. shared: a participant of a parallel build, which writes through a SHARED target and takes the generations of its
 * segments from the metapage; the rows and heap blocks it did go to the progress of the leader.
 */
/* What the formula of the design notes looked at in the strings of an index being built, by kind and leaf (past the first SIFT_COL_SCAN_LEAVES none), summed over its participants: [0] the LZ4 chosen, [1] the zstd kept */
#define SIFT_COL_SCAN_LEAVES	64
typedef struct ColScanAcc
{
	slock_t		mutex;
	SiftCcScanSum sum[SIFT_SCAN_KINDS][SIFT_COL_SCAN_LEAVES][2];
} ColScanAcc;

/* The writes of this backend add to acc until it is watched with NULL; the leader reports the sums with one NOTICE at the end of the build */
extern void sift_colb_scan_watch(ColScanAcc *acc);
extern void sift_colb_scan_report(Relation index, ColScanAcc *acc);
extern ColBuild *sift_colb_begin(Relation index, SiftWriteTarget *wt, Size budget, int maxsegs, SiftSegEntry *segs, bool shared, int nslots);
/* Scans the whole heap (serial build) or the superblock sb of nblocks heap blocks (parallel); both return the heap tuples scanned */
extern double sift_colb_scan(ColBuild *b, Relation heap, IndexInfo *info);
extern double sift_colb_scan_sb(ColBuild *b, Relation heap, IndexInfo *info, Snapshot snapshot, uint32 sb, BlockNumber nblocks);
/* Writes what is left; returns the number of segments, in segs */
extern int	sift_colb_end(ColBuild *b);
extern double sift_colb_rows(const ColBuild *b);
extern void sift_colb_free(ColBuild *b);
/* Publishes the segments of a build in the directory, ordered by their first superblock */
extern void sift_colb_publish(Relation index, SiftSegEntry *segs, int nsegs);

/* col/build_par.c: the whole build with workers, NULL when none could be launched (nothing is written then) */
extern IndexBuildResult *sift_colp_build(Relation heap, Relation index, IndexInfo *info);

#ifdef SIFT_TESTING
/*
 * Parallel workers the last sift_col build of this session launched; the superblock (1 = the first) whose participant fails (0 = none);
 * kB the writer of a segment holds before the segment is cut (0 = by maintenance_work_mem); the segments a participant may hold (0 = the directory's room)
 */
extern PGDLLIMPORT int sift_test_col_workers;
extern PGDLLIMPORT int sift_test_col_fail_sb;
extern PGDLLIMPORT int sift_test_col_segment_kb;
extern PGDLLIMPORT int sift_test_col_max_segments;
extern PGDLLIMPORT int sift_test_col_gap_blocks;	/* blocks a parallel build reserves for the merge (-1 = by the statistics, 0 = none) */
#endif

#endif							/* SIFT_COL_BUILD_H */
