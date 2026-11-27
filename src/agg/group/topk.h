/*
 * agg/group/topk.h - ORDER BY .. LIMIT K [OFFSET M] of the groups of SiftAggV6 as a heap of the best K = LIMIT + OFFSET of them (CB-4). The groups are not all boxed as Datums and handed to a Sort:
 * a group is offered by the values of its sort columns (made only as far as the first column that tells it from the worst group held), and only a group that stays is copied out whole. The planner side
 * (agg/plan/rewrite.h, sift_v6_topk_paths) adds a path of the same node with the order of the query's ORDER BY, so that no Sort is planned above it.
 */
#ifndef SIFT_AGG_V6_GROUP_TOPK_H
#define SIFT_AGG_V6_GROUP_TOPK_H

#include "postgres.h"

#include "agg/engine.h"

#define SIFT_TOPK_MAX_K		65536
/* the value of the sort column item of the row being offered */
typedef Datum (*SiftTopKValue) (void *ctx, int item, bool *isnull);

typedef struct SiftGTopK SiftGTopK;

/* desc: the columns of a row; items: its sort columns and their order */
extern SiftGTopK *sift_gtopk_new(MemoryContext cx, uint32 k, TupleDesc desc, int nitems, const SiftV6TopKItem *items);
/* whether a row with these sort values would stay: the heap is not full, or the row sorts before the worst row held */
extern bool sift_gtopk_wants(SiftGTopK *t, SiftTopKValue value, void *ctx);
/* the row (copied) in place of the worst row, or added; only after sift_gtopk_wants said true for it */
extern void sift_gtopk_put(SiftGTopK *t, const Datum *values, const bool *isnull);
/* the rows best first */
extern void sift_gtopk_finish(SiftGTopK *t);
extern uint32 sift_gtopk_count(const SiftGTopK *t);
/* the value of sort column item in the worst row held, when the heap is full (a heap that moved to a sort has none) */
extern bool sift_gtopk_worst(const SiftGTopK *t, int item, Datum *value, bool *isnull);
/* the next row, best first, after sift_gtopk_finish: valid until the next call */
extern bool sift_gtopk_next(SiftGTopK *t, const Datum **values, const bool **isnull);
/* the sort the rows moved to, and its files, end; the rest goes with the memory context */
extern void sift_gtopk_end(SiftGTopK *t);

#endif							/* SIFT_AGG_V6_GROUP_TOPK_H */
