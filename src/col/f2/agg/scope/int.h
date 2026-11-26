/*
 * col/f2/agg/scope/int.h - the executor of the scope aggregate of Format 2 (run.c, topn.c): the plans of the scope planner (agg/scope/scope.h) read from the summary of col/f2/scope/scope.h.
 */
#ifndef SIFT_COL_F2_V6_SCOPE_INT_H
#define SIFT_COL_F2_V6_SCOPE_INT_H

#include "postgres.h"

#include "nodes/extensible.h"

#include "agg/scope/scope.h"
#include "col/f2/scope/scope.h"

/* The cells of the leaves (leaves[0 .. nleaves - 1]) of the newest rows of a key under the order of the statement, from its ring and the rows the summary lacks; false: the join answers */
extern bool sift_v6_scope_topn(SiftF2Scope *sc, const ScopeSpec *sp, int64 key, bool no_key, int tie_leaf, const int *leaves, int nleaves, SiftF2ScopeCell **out, int *nout);

extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_scope_plan_methods;

#endif							/* SIFT_COL_F2_V6_SCOPE_INT_H */
