/*
 * agg/v6/v6_par_topk.h - ORDER BY .. LIMIT of the groups inside a participant of the parallel SiftAggV6. The groups a participant finishes (the partitions it claims hold every record of
 * their keys, so their accumulators are final) go through the heap of the best K = LIMIT + OFFSET of agg/v6/v6_topk_run.h and only those K tuples leave through the Gather, which the
 * Sort above it orders again with the K of every other participant.
 */
#ifndef SIFT_AGG_V6_PAR_TOPK_H
#define SIFT_AGG_V6_PAR_TOPK_H

#include "postgres.h"

#include "agg/v6/v6.h"

typedef struct SiftV6ParTopK SiftV6ParTopK;

/* The next finished group of the participant, in the memory of tuple_cx, which the heap empties after each one */
typedef bool (*SiftV6ParTopKSource) (void *arg, Datum *values, bool *isnull);

extern SiftV6ParTopK *sift_v6_par_topk_new(MemoryContext cx, const SiftV6Spec *spec, TupleDesc desc, MemoryContext tuple_cx, SiftV6ParTopKSource source, void *arg);
/* The best groups of the participant, best first; the first call drains the source */
extern bool sift_v6_par_topk_next(SiftV6ParTopK *t, Datum *values, bool *isnull);

#endif							/* SIFT_AGG_V6_PAR_TOPK_H */
