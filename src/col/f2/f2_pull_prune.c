/*
 * col/f2/f2_pull_prune.c - what the predicates of a pull leave of an index (col/f2/f2_pull_prune.h): the key set, the key plan and the time window are made once, then each partition, part and granule
 * is asked. The granules of a part are those the key marks (or the zones of the lead key column) reach, less those a zone of a leaf of the predicates rules out (sift_f2_zone_verdict, the verdict
 * the scans of the index use); a granule with a value the leaf could not hold is never ruled out by the zone of that leaf.
 */
#include "postgres.h"

#include "col/f2/f2_col_delta_idx.h"
#include "col/f2/f2_marks.h"
#include "col/f2/f2_pull_prune.h"
#include "col/f2/part_format.h"
#include "core/f2/manifest.h"
#include "table/f2/f2_col_delta.h"

void
sift_f2_pull_prune_begin(SiftF2PullPrune *pr, Relation index, const SiftF2PullSpec *spec)
{
	*pr = (SiftF2PullPrune) {.index = index,.npred = spec->npred,.pred = spec->pred,.nelem = spec->nelem_pred,.elem = spec->elem_pred,.possible = true};
	if (pr->npred == 0)
		return;
	sift_f2_key_plan(sift_f2_rel(index), sift_col_rel(index)->schema->cols, pr->pred, pr->npred, &pr->plan);
	pr->possible = sift_f2_key_set(sift_f2_rel(index), pr->pred, pr->npred, &pr->keys) && sift_f2_time_window(sift_f2_rel(index), pr->pred, pr->npred, &pr->win) && pr->plan.state != SIFT_KEY_PLAN_EMPTY;
}

SiftF2TimeWin
sift_f2_time_window_clean(Relation index, const SiftPartitionEntry *e, const SiftF2TimeWin *w)
{
	SiftF2TimeWin clean = *w;
	uint16		dirty = (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0 ? sift_manifest_read_col_delta(index, e).flags : 0;

	clean.use[0] &= (dirty & SIFT_CDLA_DIRTY_TIME0) == 0;
	clean.use[1] &= (dirty & SIFT_CDLA_DIRTY_TIME1) == 0;
	return clean;
}

bool
sift_f2_pull_prune_entry(const SiftF2PullPrune *pr, const SiftPartitionEntry *e)
{
	SiftF2TimeWin win;

	if (pr->npred == 0)
		return false;
	if (!pr->possible || !sift_manifest_overlaps(e->key_lo, e->key_hi, pr->keys.hull.has_lo ? pr->keys.hull.lo : NULL, pr->keys.hull.has_hi ? pr->keys.hull.hi : NULL))
		return true;
	win = sift_f2_time_window_clean(pr->index, e, &pr->win);
	return e->l0_slab_blk == InvalidBlockNumber && !sift_f2_entry_meets_time(e, &win);
}

bool
sift_f2_pull_prune_part(const SiftF2PullPrune *pr, const SiftPartitionEntry *e, const SiftPartDesc *d)
{
	SiftF2TimeWin win;

	if (pr->npred == 0)
		return false;
	win = sift_f2_time_window_clean(pr->index, e, &pr->win);
	return !pr->possible || !sift_f2_part_overlaps(d, &pr->keys) || !sift_f2_part_meets_time(d, &win);
}

static bool
has_exception(const SiftF2PullPrune *pr, uint32 g, int leaf)
{
	int			lo = 0,
				hi = pr->nexc;

	while (lo < hi)
	{
		int			mid = lo + ((hi - lo) >> 1);

		if (pr->exc[mid].granule < g || (pr->exc[mid].granule == g && pr->exc[mid].col < leaf))
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < pr->nexc && pr->exc[lo].granule == g && pr->exc[lo].col == leaf;
}

static SiftZone *
read_zones(const SiftF2PullPrune *pr, int leaf)
{
	const SiftSecDirEntry *zs = sift_part_section(pr->part, SEC_ZONES);
	const uint32 ng = pr->part->hdr.ngranules;
	SiftZone   *z;

	if (zs == NULL || zs->len != (uint64) pr->part->hdr.ncols * ng * sizeof(SiftZone))
		return NULL;
	z = palloc(ng * sizeof(SiftZone));
	sift_part_read_section_bytes(pr->index, pr->part, zs, (uint64) leaf * ng * sizeof(SiftZone), (uint64) ng * sizeof(SiftZone), z);
	return z;
}

/* a granule whose leaf of a predicate a column delta patches has no zone of it that decides for it */
static void
add_patch_exceptions(SiftF2PullPrune *pr, const SiftColDeltaPartMap *cd)
{
	const int	ncols = sift_col_rel(pr->index)->schema->ncols;
	bool	   *pruned_leaf = palloc0(ncols);

	for (int k = 0; k < pr->npred + pr->nelem; k++)
		pruned_leaf[(k < pr->npred ? pr->pred[k] : pr->elem[k - pr->npred]).col] = true;
	pr->exc = sift_cdi_exceptions(cd, pr->exc, pr->nexc, pruned_leaf, ncols, &pr->nexc);
}

void
sift_f2_pull_prune_open(SiftF2PullPrune *pr, Relation index, const SiftPart *part, const SiftGranuleDesc *dir, const SiftColDeltaPartMap *cd, SiftF2PullStats *st)
{
	const uint32 ng = part->hdr.ngranules;
	const int	nleaf = pr->npred + pr->nelem;
	uint64		len;
	uint32		kept = 0;

	pr->part = part;
	pr->dir = dir;
	pr->span = 0;
	pr->next = 0;
	pr->zones = palloc0(Max(nleaf, 1) * sizeof(SiftZone *));
	pr->exc = nleaf > 0 ? sift_part_read_section(index, part, SEC_VARIANT_EXC, &len) : NULL;
	pr->nexc = pr->exc != NULL ? len / sizeof(SiftVariantExcSummary) : 0;
	add_patch_exceptions(pr, cd);
	for (int k = 0; k < nleaf; k++)
		pr->zones[k] = read_zones(pr, (k < pr->npred ? pr->pred[k] : pr->elem[k - pr->npred]).col);
	if (pr->npred == 0)
	{
		pr->nspans = ng > 0;
		pr->spans[0][0] = 0;
		pr->spans[0][1] = ng;
	}
	else
		pr->nspans = sift_f2_granule_spans(index, part, sift_f2_rel(index), &pr->keys, &pr->plan, pr->spans);
	for (int i = 0; i < pr->nspans; i++)
		kept += pr->spans[i][1] - pr->spans[i][0];
	st->granules_key_skipped += ng - kept;
}

static bool
zones_rule_out(const SiftF2PullPrune *pr, int from, int n, uint32 g, uint32 nrows)
{
	for (int k = from; k < from + n; k++)
	{
		const SiftColPred *p = k < pr->npred ? &pr->pred[k] : &pr->elem[k - pr->npred];

		if (pr->zones[k] != NULL && sift_f2_zone_verdict(&sift_col_rel(pr->index)->schema->cols[p->col], p, &pr->zones[k][g], nrows, has_exception(pr, g, p->col)) == Z_NONE)
			return true;
	}
	return false;
}

bool
sift_f2_pull_prune_next(SiftF2PullPrune *pr, uint32 *g, SiftF2PullStats *st)
{
	while (pr->span < pr->nspans)
	{
		uint32		at;

		pr->next = Max(pr->next, pr->spans[pr->span][0]);
		if (pr->next >= pr->spans[pr->span][1])
		{
			pr->span++;
			continue;
		}
		at = pr->next++;
		if (!zones_rule_out(pr, 0, pr->npred, at, pr->dir[at].nrows))
		{
			*g = at;
			return true;
		}
		st->granules_zone_skipped++;
	}
	return false;
}

bool
sift_f2_pull_prune_elems(const SiftF2PullPrune *pr, uint32 g, uint32 n_elems)
{
	return zones_rule_out(pr, pr->npred, pr->nelem, g, n_elems);
}
