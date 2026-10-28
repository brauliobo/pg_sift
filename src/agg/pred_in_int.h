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
void set_rows(const uint64 *rows, int n, bool res, uint64 *out);

#ifdef SIFT_TESTING
/* the dictionaries a pattern test ran on the candidates of, and the entries it did not test (sift._test_prefilter) */
extern int64 sift_agg_prefilter_dicts;
extern int64 sift_agg_prefilter_skipped;
#endif

/* pred_in.c */
void eval_in(AggExec *ex, SiftAggPred *p, PredState *st, int start, int n, uint64 *out);
void eval_lut(AggExec *ex, SiftAggPred *p, PredState *st, int start, int n, uint64 *out);
void eval_step(AggExec *ex, SiftAggPred *p, PredState *st, int start, int n, uint64 *out);

#pragma GCC visibility pop

#endif
