/*
 * agg/pred_in_int.h - shared between agg/pred.c and agg/pred_in.c
 */
#ifndef SIFT_AGG_PRED_IN_INT_H
#define SIFT_AGG_PRED_IN_INT_H

#include "agg/agg_int.h"

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* pred.c */
void clear_nulls(AggExec *ex, const SiftSrcVec *v, int start, int n, uint64 *out);
void negate(AggExec *ex, uint64 *out, int n);

/* pred_in.c */
void eval_in(AggExec *ex, SiftAggPred *p, PredState *st, int start, int n, uint64 *out);
void eval_lut(AggExec *ex, SiftAggPred *p, PredState *st, int start, int n, uint64 *out);

#pragma GCC visibility pop

#endif
