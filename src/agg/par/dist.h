/*
 * agg/par/dist.h - COUNT(DISTINCT integer column) in the parallel SiftAggV6, with the flat sets of the serial run (v6_distinct_u64.h) on both sides of the partitions: a participant folds the values of
 * its rows into the flat set of its own state (no per-row call, no hash of bytes), hands the values it holds to the partitions as records of V6REC_DIST_INT routed by the hash of their group (of the
 * value, for a query of no keys), and the owner of a partition adds the records of each aggregate to a flat set of its own, which the group of the owner then finishes as a serial run does. Every
 * value of a group reaches the one partition that owns the group, so the count of each group is final there and no set of one participant is merged into another's.
 */
#ifndef SIFT_AGG_V6_PAR_DIST_H
#define SIFT_AGG_V6_PAR_DIST_H

#include "postgres.h"

#include "agg/group/int.h"
#include "agg/par/par.h"

/* every COUNT(DISTINCT) of the spec is of one integer column of a plain GROUP BY: the shapes this path carries */
extern bool sift_v6_par_dist_lean(const SiftV6Spec *spec);

/* ---- a participant that scans ---- */

typedef struct SiftV6ParDistOut SiftV6ParDistOut;

/*
 * The state of a participant takes its COUNT(DISTINCT) rows as this: a query of no keys keeps the flat set of the serial run (the values repeat, and the set is small and in the cache), a GROUP BY makes
 * a record of each row at once (a set of pairs is as large as the rows and the owner of a partition, which holds a share of it, counts them in its cache). It is bounded by the share the caller hands it over at.
 */
/* share: the bytes the participant holds before it hands them over (sift_v6_par_share); the flat sets of a query of no keys have room for that and the values of one more morsel */
extern SiftV6ParDistOut *sift_v6_par_dist_out_new(SiftV6Group *g, uint64 share);
/* The bytes it holds */
extern uint64 sift_v6_par_dist_load(const SiftV6ParDistOut *o);
/* Everything it holds as records, which are then gone from it */
extern void sift_v6_par_dist_export(SiftV6ParDistOut *o, SiftV6RecPut put, void *arg);

/* ---- the owner of a partition ---- */

typedef struct SiftV6ParDist SiftV6ParDist;

extern SiftV6ParDist *sift_v6_par_dist_new(SiftV6Group *g);
/* Takes the record rec (V6REC_DIST_INT) */
extern void sift_v6_par_dist_add(SiftV6ParDist *d, const uint8 *rec);
/* The records taken so far are in the sets of the state (the group's finish takes the counts from there) */
extern void sift_v6_par_dist_flush(SiftV6ParDist *d);
/* The distinct values of aggregate slot of the set of no keys, final */
extern uint64 sift_v6_par_dist_count(SiftV6ParDist *d, uint32 slot);
/* The sets are empty for the records of the next partition */
extern void sift_v6_par_dist_reset(SiftV6ParDist *d);

/* a record that does not fit a chunk of the partitions is an error that names the way out */
extern void sift_v6_par_check_record_size(uint32 rlen);

#endif							/* SIFT_AGG_V6_PAR_DIST_H */
