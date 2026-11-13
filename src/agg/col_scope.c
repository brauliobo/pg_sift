/*
 * agg/col_scope.c - the source of the scope segments of an index (SiftSrcProvider.open_scope, col/scope.h) and its statistics
 */
#include "postgres.h"

#if __has_include("col/col.h")

#include "col/col.h"
#include "col/pg.h"
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

/*
 * The statistics of the scope units of the index, the fields a read of them prices by its units (docs/COL-SCOPE.md): its units are cut by the scope key and the time, so their zones are narrow where
 * the entity units' are wide, and the sections they hold (the histograms, rollups) are those of the units written for the scope, which a member's entity unit may have where they have not. Every unit
 * of a scope is keyed: it answers from its pre-aggregates only where the whole heap is all-visible.
 */
bool
sift_col_scope_stats(Relation index, SiftSrcStats *st)
{
	SiftColLeafMemo memo;
	SiftColIndexMeta *meta = sift_col_index_meta(index, &memo);
	bool		wovf[SIFT_COLSRC_MAX_LEAVES] = {false},
				wjnull[SIFT_COLSRC_MAX_LEAVES] = {false};

	if (memo.scope_leaves == NULL)
		return false;
	memset(st, 0, sizeof(*st));
	st->scope = true;
	st->exact_frac = meta->stats.clean_frac >= 1.0 ? 1.0 : 0.0;
	sift_colsrc_leaf_stats(meta->cols, meta->ncols, memo.scope_leaves, wovf, wjnull, st);
	st->zones = col_zones(memo.scope_zones, meta, memo.scope_zones->units);
	st->miss_pending = true;
	return true;
}

#endif							/* __has_include("col/col.h") */
