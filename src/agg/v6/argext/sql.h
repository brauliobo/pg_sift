/*
 * agg/v6/argext/sql.h - sift.arg_max and sift.arg_min as PostgreSQL aggregates (the heap path, identical to what SiftAggV6 computes), and the test of the planner for them.
 */
#ifndef SIFT_AGG_V6_ARGEXT_SQL_H
#define SIFT_AGG_V6_ARGEXT_SQL_H

#include "postgres.h"

#include "nodes/primnodes.h"

/* SIFT_ARG_MAX or SIFT_ARG_MIN when the Aggref is sift.arg_max or sift.arg_min with no DISTINCT and no ORDER BY, else -1 (a FILTER is the caller's to take or refuse) */
extern int sift_argext_aggref_dir(const Aggref *a);

#endif							/* SIFT_AGG_V6_ARGEXT_SQL_H */
