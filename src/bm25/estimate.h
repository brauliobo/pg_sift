/*
 * bm25/estimate.h - the planner's questions to the index (PostgreSQL only): the estimate of a query, remembered across the
 * several times a statement asks for it. The estimate itself (sift_eval_estimate) is in the portable core.
 */
#ifndef SIFT_ESTIMATE_H
#define SIFT_ESTIMATE_H

#include "bm25/eval.h"

/* sift_eval_estimate, with the last few answers of the backend kept */
extern void sift_estimate_cached(Relation index, text **queries, int nqueries, SiftEstimate *est);
extern void sift_estimate_cached_src(Relation index, const SiftQuerySrc *srcs, int nqueries, SiftEstimate *est);

#endif							/* SIFT_ESTIMATE_H */
