/*
 * agg/par/star_run.h - the star and snowflake joins of agg/par/star.h as a run of agg/par/pinrun.h: the statements it takes, and its scan.
 */
#ifndef SIFT_AGG_V6_PAR_STAR_RUN_H
#define SIFT_AGG_V6_PAR_STAR_RUN_H

#include "postgres.h"

#include "agg/par/pinrun.h"

/* NULL for any other statement, or when a column of the scan is not an integer the threads decode as it is */
extern SiftV6PinRun *sift_v6_star_pin_begin(const SiftV6Spec *spec, SiftV6Group *g, SiftV6ScanStats *stats, Relation index, Snapshot snap, SiftPinPartOk part_ok, void *part_arg, const SiftV6Pipe *pipe,
										const SiftV6PinCol *cols, int ncols);

#endif							/* SIFT_AGG_V6_PAR_STAR_RUN_H */
