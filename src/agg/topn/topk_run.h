/*
 * agg/topn/topk_run.h - the node's side of ORDER BY .. LIMIT of the groups (agg/group/topk.h): the groups of a SiftV6Group, or the tuples of a stream (a projection's), offered to the heap of
 * the best K, then handed out best first.
 */
#ifndef SIFT_AGG_V6_TOPK_RUN_H
#define SIFT_AGG_V6_TOPK_RUN_H

#include "postgres.h"

#include "executor/executor.h"

#include "agg/engine.h"
#include "agg/group/topk.h"

typedef struct SiftV6TopKRun SiftV6TopKRun;

/* desc: the tuple of the node (the keys then the aggregates, as the scan tlist has them) */
extern SiftV6TopKRun *sift_v6_topk_run_new(MemoryContext cx, const SiftV6Spec *spec, TupleDesc desc);
/*
 * Offers every group of g (the node's HAVING qual, if any, is tried on the whole tuple with ec and slot, the scan slot of the node); the groups that a spilled run left in a tuplestore come
 * as tuples.
 */
extern void sift_v6_topk_run_groups(SiftV6TopKRun *r, SiftV6Group *g, ExprState *qual, ExprContext *ec, TupleTableSlot *slot);
/* The same without finishing the heap, so that the groups of one more state can be offered; sift_v6_topk_run_finish then orders it */
extern void sift_v6_topk_run_offer_groups(SiftV6TopKRun *r, SiftV6Group *g, ExprState *qual, ExprContext *ec, TupleTableSlot *slot);
/* The tuples next gives (values and nulls of the whole tuple) in the same way */
extern void sift_v6_topk_run_stream(SiftV6TopKRun *r, bool (*next) (void *arg, Datum *values, bool *isnull), void *arg, ExprState *qual, ExprContext *ec, TupleTableSlot *slot);
/* A row of a stream the caller builds (agg/topn/rows.h): offered by its sort values, then copied in when wants says so */
extern bool sift_v6_topk_run_wants(SiftV6TopKRun *r, SiftTopKValue value, void *ctx);
extern void sift_v6_topk_run_put(SiftV6TopKRun *r, const Datum *values, const bool *isnull);
/* The value of sort column item in the worst row held, when the heap is full */
extern bool sift_v6_topk_run_worst(const SiftV6TopKRun *r, int item, Datum *value, bool *isnull);
extern void sift_v6_topk_run_finish(SiftV6TopKRun *r);
/* The sort a heap past work_mem moved to ends with its files; the rest goes with the memory context */
extern void sift_v6_topk_run_end(SiftV6TopKRun *r);
/* The candidates offered that are not among the best K: refused, or pushed out by a better one */
extern uint64 sift_v6_topk_run_dropped(const SiftV6TopKRun *r);
/* The next best tuple; false after the last */
extern bool sift_v6_topk_run_next(SiftV6TopKRun *r, Datum *values, bool *isnull);

#endif							/* SIFT_AGG_V6_TOPK_RUN_H */
