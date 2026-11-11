/*
 * agg/col_scope.c - the source of the scope segments of an index (SiftSrcProvider.open_scope, col/scope.h)
 */
#include "postgres.h"

#if __has_include("col/col.h")

#include "col/col.h"
#include "col/scope.h"

#include "agg/col_int.h"

void *
sift_col_open_scope(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, const void *view)
{
	return col_open_flags(heap, index, snapshot, mcxt, SIFT_COLO_NOACL | SIFT_COLO_POINTS | SIFT_COLO_POINT_ROWS | SIFT_COLO_SCOPE, view);
}

void
sift_col_open_scope_delta(void *impl, const SiftScopeRead *rd)
{
	sift_col_scope_delta(((ColSource *) impl)->cs, rd);
}

#endif							/* __has_include("col/col.h") */
