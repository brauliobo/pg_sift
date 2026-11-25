/*
 * col/f2/agg/proj/in_seek.h - a statement that holds the leading sort column of a SIFT_PROJ_SORT projection to a list of values (k0 = ANY ($1), k0 IN (..)) and orders by the columns after it:
 * the rows of one value are in the order of the statement, so each value is one sought stream of every projection part and the streams are merged in the order of the statement, which stops at
 * the limit. The values are distinct and at most SIFT_PROJ_IN_SEEK_MAX; a longer list is not sought (the statement is read from the relation). Nothing here reads a page.
 */
#ifndef SIFT_COL_F2_V6_PROJ_IN_SEEK_H
#define SIFT_COL_F2_V6_PROJ_IN_SEEK_H

#include "core/host.h"

#include "col/f2/agg/proj/proj.h"
#include "col/f2/agg/proj/pred.h"

/* The distinct values of an IN predicate into out (room for SIFT_PROJ_IN_SEEK_MAX); the count, or -1 when there are more of them */
extern int	sift_v6_in_seek_values(const SiftProjPred *in, SiftProjConst *out);

/* preds with the predicate at in_idx made the equality to val: what the seek of one stream is planned from (out has room for n; one holds the equality's value) */
extern void sift_v6_in_seek_preds(const SiftProjPred *preds, int n, int in_idx, const SiftProjConst *val, SiftProjPred *out, SiftProjConst *one);

/* The index of the predicate that holds the leading sort column to a list, -1 when no predicate does */
extern int	sift_v6_in_seek_pred_index(const SiftProjSpec *order, const SiftV6SortUse *use, const SiftProjPred *preds, int n);

/* The order of the statement as the sort columns of a spec: what the streams are merged by */
extern SiftProjSpec sift_v6_in_seek_order(const SiftV6ProjQuery *q);

#endif							/* SIFT_COL_F2_V6_PROJ_IN_SEEK_H */
