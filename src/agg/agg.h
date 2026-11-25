/*
 * agg/agg.h - what the engines of the aggregation share: the walker pre-test of the passes that rewrite a query
 * before it is planned (core/jsonbuild.c) and the registration of the hybrid scan.
 */
#ifndef SIFT_AGG_H
#define SIFT_AGG_H

#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"

/*
 * The passes that rewrite a query before it is planned (core/jsonbuild.c) look for sub-selects, CTE references and calls; a list of constants
 * (`k IN (1, 2, ..)`) holds none, and walking it costs about 570 instructions a key over all of them.
 * SIFT_SKIP_CONST_LIST(n, ret) is the first line of their walkers, after the NULL test.
 */
static inline bool
sift_const_list(Node *n)
{
	if (!IsA(n, ArrayExpr))
		return false;
	foreach_ptr(Node, e, ((ArrayExpr *) n)->elements)
		if (!IsA(e, Const))
			return false;
	return true;
}

#define SIFT_SKIP_CONST_LIST(n, ret) \
	do { if (sift_const_list(n)) return ret; } while (0)

/* agg/hybrid/plan.c: registers the path hook of SiftHybridScan, after sift_planner_init: the path competes with the best paths of a scored query */
extern void sift_hybrid_init(void);

#endif							/* SIFT_AGG_H */
