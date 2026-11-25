/*
 * agg/col_read.c - the reads of the column source: the engine decodes a unit into the vectors of the executor
 */
#include "postgres.h"

#if __has_include("col/col.h")

#include "agg/kern.h"
#include "col/v1/src_sel.h"
#include "agg/wide.h"
#include "col/col.h"

#include "agg/col_int.h"

/* a numeric leaf that is only summed is one record for each row: the dictionary of its entries is not made */
void
col_wide_dict(SiftV1Idx *r, int leaf, SiftSrcWide *w, const SiftColVec *v, uint32 n)
{
	ColSource  *src = (ColSource *) r;
	const SiftColDef *def = sift_col_def(r->cs, leaf);

	if (!src->numsum[leaf] || def->type != SIFT_COLT_NUMERIC)
	{
		sift_v1_idx_wide_dict(r, leaf, w, v, n);
		return;
	}
	w->cxt = r->cxt;
	sift_aggwide_nums(w, def, v, n);
	r->dict[leaf] = (SiftSrcDict) {.id = (const void *) (uintptr_t) (UINT64CONST(0x7000000000000000) + ++r->wseq),.n = w->nd,.bytes = (const char *) w->recs,.gctx = w,.numrec = true,.rec = sift_aggwide_rec};
}

void
col_live(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	ColSource  *src = impl;

	sift_v1_idx_live(&src->rd, unit, bits);
	if (src->rck && !src->rck->active)
		recheck_collect(src, unit->nrow, bits);
}

bool
col_gorder_compact(void *impl, int i, int leaf)
{
	return sift_col_gorder_compact(((ColSource *) impl)->rd.cs, i, leaf);
}

bool
col_gorder_clean(void *impl, int i)
{
	return sift_col_gorder_clean(((ColSource *) impl)->rd.cs, i);
}

void *
col_gorder_groups_open(void *impl, int gleaf, int kleaf, const int64 *only, bool desc, MemoryContext cxt)
{
	return sift_col_gorder_groups_open(((ColSource *) impl)->rd.cs, gleaf, kleaf, only, desc, cxt);
}

bool
col_gorder_groups_next(void *cursor, int64 *group)
{
	return sift_col_gorder_groups_next(cursor, group);
}

bool
col_gorder_groups(void *impl, int i, int leaf, uint32 *ngroups)
{
	return sift_col_gorder_groups(((ColSource *) impl)->rd.cs, i, leaf, ngroups);
}

void
col_gorder_group_at(void *impl, int i, int leaf, uint32 gi, int64 *group, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne)
{
	sift_col_gorder_group_at(((ColSource *) impl)->rd.cs, i, leaf, gi, group, vs, ve, ns, ne);
}

#endif							/* __has_include("col/col.h") */
