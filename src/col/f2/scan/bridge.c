/*
 * col/f2/scan/bridge.c - a scan of an index of format major 2 that has a TID bridge or predicates of a hybrid scan (agg/hybrid.h), bound to the scan of f2_scan.c: the part and the slab the
 * bridge misses are not read, the _tid chunk of a granule is probed before any other chunk is loaded, and the predicates of a faceted search (f2_pred_ext.h) join those of the index quals.
 * The rules are in bridge_core.c.
 */
#include "postgres.h"

#include "col/f2/delvec.h"
#include "col/f2/f2_col_delta_idx.h"
#include "col/f2/pend_del.h"
#include "col/f2/scan/bridge_core.h"
#include "col/f2/scan/scan_int.h"
#include "core/f2/manifest.h"

static bool
pend_dead(void *arg, uint64 tid)
{
	return sift_pdel_idx_has(arg, tid);
}

bool
sift_f2_scan_bridge_part_skip(F2Scan *s, const SiftPartitionEntry *e, const SiftPartDesc *d)
{
	if (s->bridge == NULL || !sift_f2_bridge_part_excluded(s->bridge, d->tid_min, d->tid_max, (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0))
		return false;
	if (s->bridge_parts_skipped)
		(*s->bridge_parts_skipped)++;
	return true;
}

bool
sift_f2_scan_bridge_granule_miss(const F2Scan *s, uint32 g, const SiftTidIndexEntry *tix)
{
	return s->bridge != NULL && tix != NULL && sift_f2_bridge_granule_excluded(s->bridge, true, sift_cdi_scan_remaps(s, g), tix[g].tid_min, tix[g].tid_max, tix[g].blk_bloom);
}

int
sift_f2_scan_bridge_probe(Relation rel, F2Scan *s, uint32 g, SiftLazyGranule *lg, const SiftGranuleView *v, const SiftDvGran *dg, const SiftPendDelIndex *px, bool pend_hit, Snapshot snap, uint64 *active)
{
	int			n = lg->gd.nrows,
				kept;
	uint64		dead[SIFT_DV_MASK_WORDS];
	uint64	   *tids = palloc(Max(n, 1) * sizeof(uint64));
	bool		any_dead = false;
	int			chunk = SIFT_COL_TID_IDX;

	sift_lazy_granule_load(rel, lg, &chunk, 1);
	sift_granule_tids(v, tids);
	sift_cdi_scan_remap_tids(s, g, tids, n);
	if (dg != NULL)
		any_dead = sift_dv_mask64(dg, snap, dead) > 0;
	s->bscores = palloc(Max(n, 1) * sizeof(float4));
	kept = sift_f2_bridge_probe_rows(s->bridge, tids, n, any_dead ? dead : NULL, pend_hit ? pend_dead : NULL, (void *) px, active, s->bscores);
	if (kept == 0 && s->bridge_skipped)
		(*s->bridge_skipped)++;
	pfree(tids);
	return kept;
}

uint64 *
sift_f2_scan_slab_bridge(F2Scan *s, const uint64 *tids, int n)
{
	uint64	   *mask;

	if (s->bridge == NULL)
		return NULL;
	mask = MemoryContextAlloc(s->cx, ((n + 63) / 64 + 1) * sizeof(uint64));
	s->bscores = MemoryContextAlloc(s->cx, Max(n, 1) * sizeof(float4));
	sift_f2_bridge_slab_rows(s->bridge, tids, n, mask, s->bscores);
	return mask;
}

int
sift_f2_scan_append_ext(F2Scan *s, int kept)
{
	if (s->n_ext_preds > 0)
		memcpy(s->preds + kept, s->ext_preds, s->n_ext_preds * sizeof(SiftColPred));
	return kept + s->n_ext_preds;
}

void
sift_f2_scan_set_ext_preds(IndexScanDesc scan, const SiftColPred *preds, int npreds, uint64 *parts_skipped)
{
	F2Scan	   *s = scan->opaque;

	s->ext_preds = preds;
	s->n_ext_preds = npreds;
	s->bridge_parts_skipped = parts_skipped;
}

void
sift_f2_scan_set_heap_decide(IndexScanDesc scan)
{
	((F2Scan *) scan->opaque)->heap_decide = true;
}
