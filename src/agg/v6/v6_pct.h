/*
 * agg/v6/v6_pct.h - percentile_cont(fraction) WITHIN GROUP (ORDER BY float8 column) in the groups of SiftAggV6: the values of a group are kept in the byte arena of the text states (group_ext.h) as a
 * list of chunks, the state (a V6Acc of 32 bytes) being their number, the offset of the first chunk and the offset of the last. The answer is the one PostgreSQL's percentile_cont_float8_final gives:
 * the two values around the rank fraction * (n - 1) found by selection (agg/nth.h) instead of a sort, and the interpolation rounded as the server rounds it (agg/osa_round.c).
 *
 * The offsets are those of the arena of one group table: a state is merged only into a state of the same table (a spill of a serial run), never across workers.
 */
#ifndef SIFT_AGG_V6_PCT_H
#define SIFT_AGG_V6_PCT_H

#include "postgres.h"

#include "nodes/primnodes.h"

#include "agg/v6/group_ext.h"
#include "agg/v6/v6_acc.h"

/* The aggregate is percentile_cont of a constant fraction in [0, 1] over a float8 expression in ascending order; frac is the fraction */
extern bool sift_v6_pct_cont_of(const Aggref *a, double *frac);

extern void sift_v6_pct_add(SiftTextArena *ar, V6Acc *acc, double x);
extern void sift_v6_pct_merge(SiftTextArena *ar, V6Acc *d, const V6Acc *s);
/* The percentile of the values of a state with at least one value */
extern double sift_v6_pct_final(const SiftTextArena *ar, const V6Acc *acc, double frac);

#endif							/* SIFT_AGG_V6_PCT_H */
