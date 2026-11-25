/*
 * agg/v6/osa/osa.h - SV6_OSA: the ordered-set and hypothetical-set aggregates (WITHIN GROUP) of SiftAggV6. They run as the other generic aggregates (v6_gen.h) over the sort of their inputs
 * (v6_gen_sort.h): the values of a group reach the aggregate's transition function, which sorts them as it does in nodeAgg, and its final function takes the direct arguments.
 */
#ifndef SIFT_AGG_V6_OSA_H
#define SIFT_AGG_V6_OSA_H

#include "agg/v6/gen/gen.h"

/* the shapes of WITHIN GROUP the scan runs: one sort column for mode, percentile_disc and percentile_cont, as many as the call has arguments for a hypothetical-set aggregate */
extern bool sift_v6_osa_eligible(const Aggref *a);
/* does the input take part in the aggregate: a percentile or mode leaves NULLs out, a hypothetical-set aggregate counts them */
extern bool v6_osa_takes(const V6GenAgg *ga, const bool *nulls);

#endif							/* SIFT_AGG_V6_OSA_H */
