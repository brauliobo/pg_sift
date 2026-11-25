/*
 * agg/par/tseries_run.h - the time-bucket aggregation of agg/par/tseries.h as a run of agg/par/pinrun.h: the shape of the statement it takes, and its scan.
 */
#ifndef SIFT_AGG_V6_PAR_TSERIES_RUN_H
#define SIFT_AGG_V6_PAR_TSERIES_RUN_H

#include "postgres.h"

#include "agg/par/pinrun.h"

/* Whether the statement is one this operator takes (its shape; the backend's threads are the caller's to ask) */
extern bool sift_v6_tseries_wanted(const SiftV6Spec *spec);
/* NULL for any other statement */
extern SiftV6PinRun *sift_v6_tseries_begin(const SiftV6Spec *spec, SiftV6Group *g, SiftV6ScanStats *stats, const int *first, Relation index, Snapshot snap, SiftPinPartOk part_ok, void *part_arg);

#endif							/* SIFT_AGG_V6_PAR_TSERIES_RUN_H */
