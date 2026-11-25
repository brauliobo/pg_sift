/*
 * agg/v6/par/topk.h - ORDER BY .. LIMIT of the groups inside a participant of the parallel SiftAggV6. The groups a participant finishes (the partitions it claims hold every record of
 * their keys, so their accumulators are final) are offered to the heap of the best K = LIMIT + OFFSET of agg/v6/topn/topk_run.h by their sort values: the Datums of the keys are made only for
 * the groups that stay. Only those K tuples leave through the Gather, which the Sort above it orders again with the K of every other participant.
 */
#ifndef SIFT_AGG_V6_PAR_TOPK_H
#define SIFT_AGG_V6_PAR_TOPK_H

#include "postgres.h"

#include "agg/v6/v6.h"
#include "agg/v6/par/par.h"

typedef struct SiftV6ParTopK SiftV6ParTopK;

extern SiftV6ParTopK *sift_v6_par_topk_new(MemoryContext cx, const SiftV6Spec *spec, TupleDesc desc);
/* The heap the run of the participant offers its groups to */
extern SiftV6TopKRun *sift_v6_par_topk_heap(const SiftV6ParTopK *t);
/* The best groups of the participant, best first; the first call runs r to its end */
extern bool sift_v6_par_topk_next(SiftV6ParTopK *t, SiftV6ParRun *r, Datum *values, bool *isnull);

#endif							/* SIFT_AGG_V6_PAR_TOPK_H */
