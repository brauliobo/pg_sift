/*
 * col/f2/f2_store_ops_scope.c - SiftScopeOps of format major 2: the paths that answer a join of the facts with the pairs of a scope of the index from the cubes of its summary
 * (f2_v6_scope*.c), and the router that keeps a scan to the granules of one scope key (f2_scope_route.h).
 */
#include "postgres.h"

#include "col/f2/f2_scope_route.h"
#include "col/f2/f2_store_ops_int.h"
#include "col/f2/f2_v6_scope_int.h"

static const struct SiftScopeRouter *
scope_build_router(Relation index, int ord, Snapshot snap, const int64 *keys, int nkeys, MemoryContext cx)
{
	return sift_f2_scope_build_router(index, ord, snap, keys, (uint32) nkeys, cx);
}

static void
scope_router_counts(const struct SiftScopeRouter *r, int64 *kept, int64 *total)
{
	*kept = r->mask.kept;
	*total = r->mask.n;
}

const SiftScopeOps sift_f2_scope_ops = {
	.upper_paths = sift_agg_v6_scope_upper_paths,
	.topk_paths = sift_agg_v6_scope_topk_paths,
	.one_key = sift_v6_scope_one_key,
	.build_router = scope_build_router,
	.router_counts = scope_router_counts
};
