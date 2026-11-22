/*
 * col/f2/f2_proj_fresh.c - which base parts a projection may stand for, and which projection parts a maintenance step has to write or drop.
 *
 * A projection part answers for a base part exactly when its link says it was derived from that part as the part is now: the same part generation (the link is looked up by it) and the same
 * deletion vector generation. Nothing is flagged stale: a part that was dropped, rewritten or given another deletion vector no longer matches its link and is read from the base, so a commit of
 * the base that knows nothing of projections cannot make an answer wrong. The one exception that is not an exact match is SPS_SUBTRACT, which is exact because the projection part had no
 * dead row when it was written, every dead row now is settled, and the cells an AGG part holds (counts and sums) can have rows taken away.
 */
#include "core/host.h"

#include "col/f2/f2_proj.h"
#include "col/f2/f2_proj_overlay.h"
#include "col/f2/part_format.h"

static uint64
dv_gen_of(const SiftPartDesc *d)
{
	return d->dv_blk == InvalidBlockNumber ? 0 : d->dv_gen;
}

bool
sift_proj_part_projectable(const SiftPartDesc *base)
{
	if (base->flags & (SIFT_PART_FLAG_SCOPE | SIFT_PART_FLAG_L0_MICRO | SIFT_PART_FLAG_PACKED_SLOT))
		return false;
	return base->level >= 1 && (base->flags & SIFT_PART_FLAG_ALL_VISIBLE) && base->col_delta_blk == InvalidBlockNumber &&
		(base->dv_blk == InvalidBlockNumber || (base->flags & SIFT_PART_FLAG_DV_SETTLED));
}

SiftProjLink
sift_proj_link_make(const SiftManifestProjRef *ref, const SiftProjSpec *spec, const SiftPartDesc *base, uint64 proj_gen, bool where_exc)
{
	return (SiftProjLink) {.src_gen = base->part_gen,.proj_gen = proj_gen,.src_dv_gen = dv_gen_of(base),.src_ndead = (uint32) base->ndead,
	.flags = (ref->kind == SIFT_PROJ_AGG && sift_proj_spec_invertible(spec) ? SIFT_PROJLINK_INVERTIBLE : 0) | (where_exc ? SIFT_PROJLINK_HAS_WHERE_EXC : 0)};
}

const SiftProjLink *
sift_proj_link_of(const SiftProjParts *p, uint64 src_gen)
{
	int			lo = 0,
				hi = p->nlinks - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (p->links[mid].src_gen == src_gen)
			return &p->links[mid];
		if (p->links[mid].src_gen < src_gen)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

static bool
rebuild_needed(const SiftManifestProjRef *ref, const SiftPartDesc *b, const SiftProjLink *l)
{
	if (l->src_dv_gen == dv_gen_of(b))
		return false;
	if (ref->kind == SIFT_PROJ_SORT)
		return !sift_proj_sort_sparse(b);
	return !((l->flags & SIFT_PROJLINK_INVERTIBLE) && l->src_ndead == 0 && sift_proj_agg_sparse(b));
}

SiftProjRefreshPlan
sift_proj_refresh_plan(const SiftManifestProjRef *ref, const SiftPartDesc *base, int nbase, const SiftProjParts *p)
{
	SiftProjRefreshPlan plan = {0};

	plan.build_gens = palloc(Max(nbase, 1) * sizeof(uint64));
	plan.drop_proj_gens = palloc(Max(nbase, 1) * sizeof(uint64));
	plan.drop_src_gens = palloc(Max(nbase, 1) * sizeof(uint64));
	for (int i = 0; i < nbase; i++)
	{
		const SiftProjLink *l = sift_proj_link_of(p, base[i].part_gen);

		if (!sift_proj_part_projectable(&base[i]) || (l != NULL && !rebuild_needed(ref, &base[i], l)))
			continue;
		plan.build_gens[plan.nbuild++] = base[i].part_gen;
		if (l != NULL)
		{
			plan.drop_proj_gens[plan.ndrop] = l->proj_gen;
			plan.drop_src_gens[plan.ndrop++] = l->src_gen;
		}
	}
	return plan;
}

void
sift_proj_refresh_plan_free(SiftProjRefreshPlan *plan)
{
	pfree(plan->build_gens);
	pfree(plan->drop_proj_gens);
	pfree(plan->drop_src_gens);
}

void
sift_proj_delta_for_dropped_parts(const SiftProjParts *p, const uint64 *dropped_src_gens, int ndropped, SiftProjDelta *out)
{
	uint64	   *proj = palloc(Max(ndropped, 1) * sizeof(uint64)),
			   *src = palloc(Max(ndropped, 1) * sizeof(uint64));
	int			n = 0;

	for (int i = 0; i < ndropped; i++)
	{
		const SiftProjLink *l = sift_proj_link_of(p, dropped_src_gens[i]);

		if (l == NULL)
			continue;
		proj[n] = l->proj_gen;
		src[n++] = l->src_gen;
	}
	*out = (SiftProjDelta) {.drop_proj_gens = proj,.ndrop_parts = n,.drop_src_gens = src,.ndrop_links = n};
}
