/*
 * agg/v6/group/topk.h - ORDER BY .. LIMIT K [OFFSET M] of the groups of SiftAggV6 as a heap of the best K = LIMIT + OFFSET of them (CB-4). The groups are not all boxed as Datums and handed to a Sort:
 * a group is offered by the values of its sort columns (made only as far as the first column that tells it from the worst group held), and only a group that stays is copied out whole. The planner side
 * (agg/v6/plan/rewrite.h, sift_v6_topk_paths) adds a path of the same node with the order of the query's ORDER BY, so that no Sort is planned above it.
 */
#ifndef SIFT_AGG_V6_GROUP_TOPK_H
#define SIFT_AGG_V6_GROUP_TOPK_H

#include "postgres.h"

#define SIFT_TOPK_MAX_K		65536
#define SIFT_TOPK_MAX_ITEMS	8

/* a < b under the order of sort column item: negative when a sorts first */
typedef int (*SiftTopKCmp) (int item, Datum a, bool a_null, Datum b, bool b_null, void *arg);
/* the value of the sort column item of the row being offered */
typedef Datum (*SiftTopKValue) (void *ctx, int item, bool *isnull);

typedef struct SiftGTopK SiftGTopK;

/* ncols: the columns of a row; typlen and typbyval of each, for the copies; the sort columns are positions item_pos[0 .. nitems-1] of the row */
extern SiftGTopK *sift_gtopk_new(MemoryContext cx, uint32 k, int nitems, const int *item_pos, int ncols, const int16 *typlen, const bool *typbyval, SiftTopKCmp cmp, void *cmp_arg);
/* whether a row with these sort values would stay: the heap is not full, or the row sorts before the worst row held */
extern bool sift_gtopk_wants(SiftGTopK *t, SiftTopKValue value, void *ctx);
/* the row (copied) in place of the worst row, or added; only after sift_gtopk_wants said true for it */
extern void sift_gtopk_put(SiftGTopK *t, const Datum *values, const bool *isnull);
/* the rows best first */
extern void sift_gtopk_finish(SiftGTopK *t);
extern uint32 sift_gtopk_count(const SiftGTopK *t);
/* the value of sort column item in the worst row held, when the heap is full */
extern bool sift_gtopk_worst(const SiftGTopK *t, int item, Datum *value, bool *isnull);
extern void sift_gtopk_row(const SiftGTopK *t, uint32 i, const Datum **values, const bool **isnull);
extern void sift_gtopk_free(SiftGTopK *t);

#endif							/* SIFT_AGG_V6_GROUP_TOPK_H */
