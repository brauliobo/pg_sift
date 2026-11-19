/*
 * agg/col_scope.c - the source of the scope segments of an index (SiftSrcProvider.open_scope, col/scope.h) and its statistics
 */
#include "postgres.h"

#if __has_include("col/col.h")

#include "col/col.h"
#include "col/pagg.h"
#include "col/pg.h"
#include "col/scope.h"
#include "col/side.h"

#include "agg/col_int.h"

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
			t[nt++] = (SiftColSuperTest) {z[k].leaf, z[k].lo, z[k].hi, z[k].nvals, z[k].vals, z[k].scope};
	sift_col_super_tests(((ColSource *) impl)->cs, t, nt, dim, hdim, need);
	pfree(t);
}

bool
col_unit_exact(void *impl, const SiftSrcUnit *unit)
{
	SiftColSegment *seg;

	/* a whole segment answers from its key table only (sift_col_unit_hist): every other question of it is refused */
	return sift_col_unit_whole(&((ColSource *) impl)->unit, &seg) || sift_col_unit_preagg_ok(&((ColSource *) impl)->unit) || col_unit_run(impl, unit);
}

bool
col_unit_run(void *impl, const SiftSrcUnit *unit)
{
	SiftColSegment *seg;
	const SideRun *run;
	const SideCell *cell;

	return sift_col_unit_run(&((ColSource *) impl)->unit, &seg, &run, &cell);
}

void
col_scope_side(void *impl, const char *key_name, int64 key, SiftKeyMap *km, SiftKeyMap *mm, const int64 *members, uint32 nmembers)
{
	ColSource  *c = impl;
	int			scope = sift_scope_ordinal(c->index, key_name);
	int64	   *keyp = MemoryContextAlloc(c->cxt, sizeof(int64));

	SideValid  *valid = sift_side_valid_open(c->index, scope, c->snapshot, km, keyp, 1, c->cxt);

	*keyp = key;
	sift_col_side_read(c->cs, scope, key, sift_side_run_valid, valid);
	if (mm != NULL)
	{
		uint64	   *words = MemoryContextAlloc(c->cxt, Max(nmembers, 1) * sizeof(uint64));

		for (uint32 i = 0; i < nmembers; i++)
			words[i] = sift_keymap_member_hash(mm, members[i]);
		sift_side_valid_members(valid, words, nmembers);
		sift_col_side_strict(c->cs, sift_side_members_covered);
	}
}

bool
col_unit_dset(void *impl, const SiftSrcUnit *unit, int cal, int leaf, int64 cell, SiftSrcPresence *p)
{
	return sift_aggpre_unit_dset(&((ColSource *) impl)->unit, cal, leaf, cell, p);
}

#endif							/* __has_include("col/col.h") */
