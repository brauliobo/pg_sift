/*
 * agg/par/run.h - SiftAggV6 on the compute threads of the backend (core/par/pool.h): the grouped aggregation ORDER BY count or sum LIMIT K of integer or text keys, which the scan hands over
 * granule by granule (core/par/pin.h) and the threads filter, decode and fold (agg/par/group.h), each keeping its K best groups.
 *
 * The K best of the threads are the answer only when every row of the statement went through them: the scan folds on the backend thread what it could not hand over, and a thread says
 * SIFT_ERR_SERIAL for a granule whose rows it does not fold (a NULL in a column the statement reads). When any row was left to the serial path, the groups of the threads are dropped and every
 * handed-over granule runs through the scan, as it would have without the threads.
 */
#ifndef SIFT_AGG_V6_PAR_RUN_H
#define SIFT_AGG_V6_PAR_RUN_H

#include "postgres.h"

#include "agg/engine.h"
#include "core/par/pin.h"

typedef struct SiftV6ParScan SiftV6ParScan;

/* Whether the threads of the backend run the statement (its shape, and the backend has threads): the planner then leaves the statement to the serial node, which starts them */
extern bool sift_v6_par_wanted(const SiftV6Spec *spec);

/*
 * The scan of the index (first: the leaf of each attribute) hands its granules over: NULL when the statement is not of the shape the threads run, the backend has no threads for it, or the memory
 * the threads need is more than work_mem allows. The answer goes to g, or to rows for the Top-N of rows (exactly one of them).
 */
extern SiftV6ParScan *sift_v6_par_begin(const SiftV6Spec *spec, SiftV6Group *g, SiftV6Rows *rows, SiftV6ScanStats *stats, const int *first, Relation index, Snapshot snap, SiftPinPartOk part_ok,
										void *part_arg);
extern SiftPinWin *sift_v6_par_window(SiftV6ParScan *p);

/* The scan has read every part: the threads fold the granules handed over (or the scan does), and the groups are in g. Before the scan ends */
extern void sift_v6_par_end(SiftV6ParScan *p);

#endif							/* SIFT_AGG_V6_PAR_RUN_H */
