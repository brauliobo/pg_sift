/*
 * col/f2/store/ops_proj.c - SiftProjOps of format major 2: the groups of an aggregate statement from the cells of a pre-aggregating projection, and the planner path of a sorted projection
 * (f2_v6_proj*.c), for a sift_col index and a sift_table alike.
 */
#include "postgres.h"

#include "col/f2/store/ops_int.h"
#include "col/f2/agg/proj/exec.h"
#include "col/f2/agg/proj/sort_plan.h"

static bool
proj_serve(const SiftV6Spec *spec, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, MemoryContext cx, SiftProjResult *out)
{
	if (index != NULL)
		return sift_v6_proj_serve(spec, heap, index, keys, nkeys, snap, cx, out);
	return sift_v6_proj_serve_table(spec, heap, keys, nkeys, snap, cx, out);
}

const SiftProjOps sift_f2_proj_ops = {
	.serve = proj_serve,
	.next = sift_v6_proj_next,
	.sort_offered = sift_v6_proj_sort_offered
};
