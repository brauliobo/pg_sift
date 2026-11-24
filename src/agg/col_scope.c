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
	return col_open_flags(heap, index, snapshot, mcxt, SIFT_COLO_NOACL | SIFT_COLO_POINTS | SIFT_COLO_POINT_ROWS | SIFT_COLO_SCOPE, view, NULL);
}

void
col_super(void *impl, bool on)
{
	sift_col_super(((ColSource *) impl)->cs, on);
}

uint32
col_super_align(void *impl, uint32 sb)
{
	return sift_col_super_align(((ColSource *) impl)->cs, sb);
}

void
col_super_tests(void *impl, const SiftSrcPrune *z, int nz, int dim, int hdim, uint64 need)
{
	SiftColSuperTest *t = palloc(Max(nz, 1) * sizeof(SiftColSuperTest));
	int			nt = 0;

	for (int k = 0; k < nz; k++)
		if (z[k].nkids == 0 && !z[k].nulltest)
			t[nt++] = (SiftColSuperTest) {z[k].leaf, z[k].lo, z[k].hi, z[k].nvals, z[k].vals};
	sift_col_super_tests(((ColSource *) impl)->cs, t, nt, dim, hdim, need);
	pfree(t);
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
	/* a unit cut by time holds the rows of a slice of time, the pages of a slice of the heap: the share of the heap's clean superblocks is the share of the units whose pages are all-visible (a heap of rows
	 * that arrive out of time order makes it an estimate, and the read proves each unit); without a time cut a unit may hold any page and answers only from a clean heap */
	st->exact_frac = sift_scope_time_leaf(meta->cols, meta->ncols) >= 0 || meta->stats.clean_frac >= 1.0 ? meta->stats.clean_frac : 0.0;
	sift_src_leaf_stats(meta->cols, meta->ncols, memo.scope_leaves, wovf, wjnull, st);
	st->zones = col_zones(memo.scope_zones, meta, memo.scope_zones->units);
	st->miss_pending = true;
	return true;
}

bool
col_unit_dset(void *impl, const SiftSrcUnit *unit, int cal, int leaf, int64 cell, SiftSrcPresence *p)
{
	return sift_aggpre_unit_dset(&((ColSource *) impl)->unit, cal, leaf, cell, p);
}

#endif							/* __has_include("col/col.h") */
