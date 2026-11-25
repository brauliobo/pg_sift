/*
 * col/f2/proj/overlay.c - which base parts a projection stands for while the relation is written to, and the rows an overlay takes away (col/f2/proj/overlay.h).
 *
 * A projection part answers for a base part exactly when the part is what the link says it was: nothing has happened to it since. What happens to a part without changing it is a delete: a
 * deletion vector that grows (in place, with a delta tail that does not change its generation), a pending delete of its partition, a heap row that died. Each of those is a set of rows the
 * host can name, so a SORT part is read with those addresses left out and an invertible AGG part has those rows taken away from its cells, each exactly once. Only what the overlay cannot name
 * (column patches on a column the projection reads, inserters not settled, too many dead rows) sends the part to the base.
 */
#include "core/host.h"

#include "col/f2/proj/overlay.h"
#include "col/f2/part/format.h"

static uint64
dv_gen_of(const SiftPartDesc *d)
{
	return d->dv_blk == InvalidBlockNumber ? 0 : d->dv_gen;
}

bool
sift_proj_dv_unchanged(const SiftPartDesc *base, const SiftProjLink *link)
{
	bool		open_to_tail = base->dv_blk != InvalidBlockNumber && !(base->flags & SIFT_PART_FLAG_DV_SETTLED);

	return link->src_dv_gen == dv_gen_of(base) && !open_to_tail;
}

SiftProjHostVis
sift_proj_vis_class(const SiftProjVisIn *in)
{
	if (!in->inserts_settled)
		return SPV_UNSETTLED;
	if (in->heap_host && in->sort_kind)
		return SPV_SORT_INS_SETTLED;
	if (in->heap_host)
	{
		if (!in->has_envelope || in->ndirty_blocks < 0 || in->ndirty_blocks > SIFT_PROJ_MAX_DIRTY_VM_BLKS)
			return SPV_UNSETTLED;
		if (in->ndirty_blocks > 0)
			return SPV_LIVE_DV;
	}
	if (!in->has_dv)
		return SPV_CLEAN;
	return in->dv_settled ? SPV_SETTLED_DV : SPV_LIVE_DV;
}

static SiftProjPartServe
verdict(SiftProjServe serve, SiftProjWhy why, const SiftProjLink *link)
{
	return (SiftProjPartServe) {.serve = serve,.why = why,.proj_gen = serve == SPS_BASE ? 0 : link->proj_gen};
}

static bool
columns_patched(const SiftProjServeIn *in)
{
	return in->base->col_delta_blk != InvalidBlockNumber || (in->col_delta_bloom & in->proj_att_bloom) != 0;
}

static SiftProjPartServe
serve_sort(const SiftProjServeIn *in, bool unchanged)
{
	return unchanged || sift_proj_sort_sparse(in->base) ? verdict(SPS_OVERLAY, SPW_SERVED, in->link) : verdict(SPS_BASE, SPW_DV_CHANGED, in->link);
}

/*
 * An AGG part is read with its dead rows taken away once: the rows the vector, the pending deletes and the changed heap blocks name, less those the cells never counted. When the part was
 * written from a vector with dead rows (src_ndead > 0) the rows that vector held are out of the cells already, and they can be told apart from the new ones only while the vector is the same.
 */
static SiftProjPartServe
serve_agg(const SiftProjServeIn *in, bool unchanged)
{
	if (!(in->link->flags & SIFT_PROJLINK_INVERTIBLE))
		return verdict(SPS_BASE, unchanged ? SPW_PEND_DEL : SPW_DV_CHANGED, in->link);
	if (!sift_proj_agg_sparse(in->base))
		return verdict(SPS_BASE, SPW_DV_CHANGED, in->link);
	if (in->link->src_ndead == 0 || unchanged)
		return verdict(SPS_OVERLAY, SPW_SERVED, in->link);
	return verdict(SPS_BASE, SPW_DV_CHANGED, in->link);
}

SiftProjPartServe
sift_proj_serve_part(const SiftProjServeIn *in)
{
	bool		unchanged,
				clean;

	if (in->ref->state != SIFT_PROJ_READY)
		return verdict(SPS_BASE, SPW_NOT_READY, in->link);
	if (in->link == NULL)
		return verdict(SPS_BASE, SPW_NO_LINK, in->link);
	if (columns_patched(in))
		return verdict(SPS_BASE, SPW_COL_DELTA, in->link);
	if (!(in->base->flags & SIFT_PART_FLAG_ALL_VISIBLE) || in->vis == SPV_UNSETTLED)
		return verdict(SPS_BASE, SPW_NOT_SETTLED, in->link);
	if (in->link->flags & SIFT_PROJLINK_HAS_WHERE_EXC)
		return verdict(SPS_BASE, SPW_WHERE_EXC, in->link);
	unchanged = sift_proj_dv_unchanged(in->base, in->link);
	clean = in->vis == SPV_CLEAN || in->vis == SPV_SETTLED_DV;
	if (unchanged && !in->part_has_pend && clean)
		return verdict(SPS_EXACT, SPW_SERVED, in->link);
	return in->ref->kind == SIFT_PROJ_SORT ? serve_sort(in, unchanged) : serve_agg(in, unchanged);
}

void
sift_proj_spec_uses(const SiftProjSpec *s, const SiftProjColumns *cols, bool *used)
{
	memset(used, 0, cols->n * sizeof(bool));
	for (int i = 0; i < s->nkeys; i++)
		used[s->key[i].leaf] = true;
	for (int i = 0; i < s->ninclude; i++)
		used[s->include[i]] = true;
	for (int i = 0; s->kind == SIFT_PROJ_SORT && s->all_columns && i < cols->n; i++)
		used[i] |= cols->col[i].usable;
	for (int i = 0; i < s->nwhere; i++)
		used[s->where[i].leaf] = true;
	for (int m = 0; m < s->nmeas; m++)
	{
		if (s->meas[m].leaf >= 0)
			used[s->meas[m].leaf] = true;
		for (int f = 0; f < s->meas[m].nfilter; f++)
			used[s->meas[m].filter[f].leaf] = true;
	}
}

bool
sift_proj_part_meets_pend(const SiftPartDesc *base, const SiftPendDelIndex *px)
{
	return px != NULL && !sift_part_tid_excludes(base, px->tid_min, px->tid_max);
}

uint64
sift_proj_dirty_bloom(const BlockNumber *blk, int n)
{
	uint64		bloom = 0;

	for (int i = 0; i < n; i++)
		bloom |= sift_tid_block_bit((uint64) blk[i] << 16);
	return bloom;
}

bool
sift_proj_dirty_has(const SiftProjDirty *d, BlockNumber blk)
{
	int			lo = 0,
				hi = d->n - 1;

	while (lo <= hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (d->blk[mid] == blk)
			return true;
		if (d->blk[mid] < blk)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

bool
sift_proj_overlay_granule_hit(bool dv_has_granule, const SiftPendDelIndex *pend, const SiftProjDirty *dirty, const SiftTidIndexEntry *tix)
{
	return dv_has_granule || (pend != NULL && sift_pdel_idx_may_hit_granule(pend, tix)) || (dirty->n > 0 && (tix->blk_bloom & dirty->bloom) != 0);
}

static bool
mask_has(const uint64 *w, uint32 r)
{
	return w != NULL && ((w[r / 64] >> (r % 64)) & 1) != 0;
}

static bool
row_dead(const SiftProjOverlayIn *in, uint32 r)
{
	uint64		tid;

	if (mask_has(in->dv_dead, r))
		return true;
	if (in->tids == NULL)
		return false;
	tid = in->tids[r];
	if (in->pend != NULL && sift_pdel_idx_has(in->pend, tid))
		return true;
	return in->dirty.n > 0 && sift_proj_dirty_has(&in->dirty, (BlockNumber) (tid >> 16)) && !in->visible(in->arg, tid);
}

int
sift_proj_overlay_mask(const SiftProjOverlayIn *in, uint64 *mask)
{
	int			n = 0;

	memset(mask, 0, SIFT_DV_MASK_WORDS * sizeof(uint64));
	for (uint32 r = 0; r < in->nrows; r++)
	{
		if (mask_has(in->baseline_dead, r) || !row_dead(in, r))
			continue;
		mask[r / 64] |= UINT64CONST(1) << (r % 64);
		n++;
	}
	return n;
}
