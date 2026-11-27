/*
 * col/f2/build/build.h - the build of an index of format major 2 (f2_build.c) and its parallel form (f2_build_par.c): every participant reads its share of the heap, collects the rows
 * in memory and writes each run that fills its share of maintenance_work_mem as a part of the cold tier into the index itself, never into a temporary file; the parts reach the manifest a page of them at a time.
 */
#ifndef SIFT_F2_BUILD_H
#define SIFT_F2_BUILD_H

#include "access/tableam.h"
#include "nodes/execnodes.h"
#include "utils/rel.h"

typedef struct SiftF2Build SiftF2Build;

/* A participant's build state in the current memory context; the budget of a run is its share of maintenance_work_mem among nparticipants */
extern SiftF2Build *sift_f2_build_begin(Relation heap, Relation index, int nparticipants);
extern uint64 sift_f2_build_budget(int nparticipants, int noperators);

/* Reads the heap (all of it for scan NULL, else the share of a parallel scan), writes the last run; the tuples the heap scan saw */
extern double sift_f2_build_scan(SiftF2Build *bs, IndexInfo *info, TableScanDesc scan, bool progress);
extern uint64 sift_f2_build_rows(const SiftF2Build *bs);
extern void sift_f2_build_end(SiftF2Build *bs);

/*
 * ON COMMIT DELETE ROWS, and a TRUNCATE of a relation made in the transaction, keep its file and begin its generations, blocks and versions again: what the backend and the shared memory
 * hold of the file (rows staged, snapshots, pins, the pages of its pool, its leased generations, the caches keyed by them) names the contents before and goes with them
 */
extern void sift_f2_forget_file(Relation rel);

/* Builds with the parallel workers the planner gave the index (info->ii_ParallelWorkers), the leader taking a share; false, with nothing read, when there are none or none could be started; res gets the tuples of the heap and the rows the workers indexed (the leader's own are added by the caller) */
extern bool sift_f2_build_par(Relation heap, Relation index, IndexInfo *info, SiftF2Build *leader, IndexBuildResult *res);

#endif							/* SIFT_F2_BUILD_H */
