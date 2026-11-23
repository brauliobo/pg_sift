/*
 * bm25/score_fallback.h - SiftScoreScan: sift.score() and sift.max_score() of a relation that no sift index scan serves.
 */
#ifndef SIFT_SCORE_FALLBACK_H
#define SIFT_SCORE_FALLBACK_H

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

extern PGDLLIMPORT const CustomPathMethods sift_fallback_path_methods;
extern PGDLLIMPORT const CustomScanMethods sift_fallback_plan_methods;

/* The rel's only path: a scan of the heap that scores the rows it keeps */
extern void sift_fallback_publish(PlannerInfo *root, RelOptInfo *rel);

#endif							/* SIFT_SCORE_FALLBACK_H */
