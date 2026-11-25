/*
 * col/f2/v6/proj/seek.h - the granules of a SIFT_PROJ_SORT part a statement can reach, from the predicates on the sort columns of the projection and the key marks (SEC_KEY_MARKS, col/f2/key/marks.h) of
 * the part: the predicates become ranges of the key space (col/f2/key/plan.h) and each range is two binary searches over the marks, so a part of any number of granules costs O(log granules) to
 * seek and no granule outside the ranges is read. A predicate the plan cannot use narrows nothing: the rows are tested on their cells as before, so the answer never depends on the seek. A part
 * without marks (one with a NaN or an exception tag among its keys, or written before the marks) is read whole. Nothing here reads a page.
 */
#ifndef SIFT_COL_F2_V6_PROJ_SEEK_H
#define SIFT_COL_F2_V6_PROJ_SEEK_H

#include "col/f2/v6/proj/pred.h"
#include "col/f2/key/plan.h"

typedef struct SiftV6ProjSeek
{
	SiftKeySpec ks;
	SiftKeyPlan plan;
} SiftV6ProjSeek;

/* false: the predicates leave the whole key space (no sort column is bound), so the part is read from its first granule. The key values are allocated in the current context. */
extern bool sift_v6_proj_seek_plan(const SiftProjPartSpec *pps, const SiftProjSpec *spec, const SiftProjPred *p, int n, SiftV6ProjSeek *out);

#endif							/* SIFT_COL_F2_V6_PROJ_SEEK_H */
