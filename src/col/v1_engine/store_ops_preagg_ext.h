/*
 * col/v1_engine/store_ops_preagg_ext.h - the states the pre-aggregates of a Format 1 unit give beyond those of plain columns (col/v1_engine/store_ops_preagg.c): the sum of a line of a
 * measure and the distinct values of the dimension of a histogram.
 */
#ifndef SIFT_V1_STORE_OPS_PREAGG_EXT_H
#define SIFT_V1_STORE_OPS_PREAGG_EXT_H

#include "agg/cube.h"
#include "agg/group/int.h"
#include "col/v1/src_exec.h"

/* The state of the sum of lin over n values of [lo, hi] whose sum is sum; false when the line may leave its type on one of them, or its sum 128 bits */
extern bool sift_v1_line_acc(const SiftV6Linear *lin, int64 n, SiftSrcI128 sum, int64 lo, int64 hi, V6Acc *out);

/* Whether group r of h is one the tests of the statement keep */
typedef bool (*SiftV1HistKeep) (const void *arg, const SiftSrcHist *h, uint32 r);

/*
 * The dimension values of the groups of h with rows, not NULL, that keep passes (NULL: every group), folded into COUNT(DISTINCT) aggregate agg, under no key or under
 * the key that is the dimension itself (the values are the keys too). buf holds SIFT_VEC_LEN string views.
 */
extern void sift_v1_hist_merge_dist(SiftV6Group *g, int agg, const SiftSrcHist *h, bool text, void *buf, SiftV1HistKeep keep, const void *arg);

#endif							/* SIFT_V1_STORE_OPS_PREAGG_EXT_H */
