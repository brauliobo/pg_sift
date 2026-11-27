/*
 * col/f2/scan/zones.c - the zone tests a scan of an index of format major 2 holds beyond the conjunction of its predicates (SiftColPrune, sift_f2_scan_set_zones): a granule is ruled out
 * when the zones of its leaves leave no row that passes one of them, an OR being kept when one of its kids may hold such a row.
 */
#include "postgres.h"

#include "col/f2/scan/int.h"

void
sift_f2_scan_set_zones(IndexScanDesc scan, const SiftColPrune *zones, int nzones)
{
	F2Scan	   *s = scan->opaque;

	s->zones = zones;
	s->nzones = nzones;
}

/* whether a row of granule g may pass the test: each of its predicates may select a row the zone of its leaf holds, and its kids keep the granule (one of them for an OR) */
static bool
admits(Relation rel, const SiftColSchema *schema, SiftF2PartPin *pin, const SiftColPrune *z, uint32 g, uint32 nrows, const SiftVariantExcSummary *exc, int nexc)
{
	for (int k = 0; k < z->npreds; k++)
	{
		const SiftColPred *p = &z->preds[k];
		const SiftZone *zones = sift_f2_pin_zones(rel, pin, p->col);

		if (zones != NULL && sift_f2_zone_verdict(&schema->cols[p->col], p, &zones[g], nrows, sift_f2_exc_has(exc, nexc, g, p->col)) == Z_NONE)
			return false;
	}
	for (int i = 0; i < z->nkids; i++)
		if (admits(rel, schema, pin, &z->kids[i], g, nrows, exc, nexc) == z->any)
			return z->any;
	return !z->any || z->nkids == 0;
}

static void
tree_leaves(const SiftColPrune *z, bool *leaves)
{
	for (int k = 0; k < z->npreds; k++)
		leaves[z->preds[k].col] = true;
	for (int i = 0; i < z->nkids; i++)
		tree_leaves(&z->kids[i], leaves);
}

void
sift_f2_zones_leaves(const F2Scan *s, bool *leaves)
{
	for (int i = 0; i < s->nzones; i++)
		tree_leaves(&s->zones[i], leaves);
}

bool
sift_f2_zones_exclude(Relation rel, const F2Scan *s, const SiftColSchema *schema, SiftF2PartPin *pin, uint32 g, uint32 nrows, const SiftVariantExcSummary *exc, int nexc)
{
	for (int i = 0; i < s->nzones; i++)
		if (!admits(rel, schema, pin, &s->zones[i], g, nrows, exc, nexc))
			return true;
	return false;
}
