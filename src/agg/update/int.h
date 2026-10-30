/*
 * agg/update/int.h - what the files of agg/update/ share
 * The aggregate update record, the value stack macro, and the functions the selection, the
 * sums, the updates, the tuple lists and the chunk call in one another.
 */
#ifndef SIFT_AGG_UPDATE_DIR_INT_H
#define SIFT_AGG_UPDATE_DIR_INT_H

#include "agg/agg_int.h"

extern void mapped_update(AggExec *ex, int ai, const uint64 *ok, int nsel);
extern void wide_sum_update(AggExec *ex, int ai, const uint64 *ok, int nsel);
extern void direct_update(AggExec *ex, int ai, const uint64 *ok, int nsel);

extern void agg_update(AggExec *ex, int ai, int start, int n, int nsel);

extern int post_filter(AggExec *ex, Tup *t, int first, int end, int stages, int set);

#endif							/* SIFT_AGG_UPDATE_DIR_INT_H */
