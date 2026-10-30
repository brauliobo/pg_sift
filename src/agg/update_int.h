/*
 * agg/update_int.h - the update of the aggregates per chunk (agg/update/)
 */
#ifndef SIFT_AGG_UPDATE_INT_H
#define SIFT_AGG_UPDATE_INT_H

#include "agg/agg_fwd_int.h"
#include "agg/tup_int.h"

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* update/ */
void agg_selection(AggExec *ex, SiftAggAgg *a, int start, int n, int nsel, bool *all);
const double *eval_vx(AggExec *ex, SiftAggAgg *a, int nsel);
void agg_tuples(AggExec *ex, Tup *t);
bool set_has_post(const SiftAggSpec *s, int set);
void sets_fold_begin(AggExec *ex);
void sets_fold(AggExec *ex);
void join_stage(AggExec *ex, int s, Tup *in);
Tup *stage_tuples(AggExec *ex, int s);
void dense_init(AggExec *ex);
void agg_chunk(AggExec *ex, int start, int n, const uint64 *mask);

#pragma GCC visibility pop

#endif
