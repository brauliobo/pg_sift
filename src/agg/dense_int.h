/*
 * agg/dense_int.h - shared between agg/update/ and agg/dense.c
 */
#ifndef SIFT_AGG_DENSE_INT_H
#define SIFT_AGG_DENSE_INT_H

#include "agg/agg_int.h"

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* update/ */
void float_guard(AggExec *ex, int ai, const double *x, int nsel);

/* dense.c: false when the chunk is not dense and goes through the list of tuples */
bool dense_chunk(AggExec *ex, int start, int n, const uint64 *mask);

#pragma GCC visibility pop

#endif
