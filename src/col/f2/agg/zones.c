/*
 * col/f2/agg/zones.c - the OR quals of the aggregate scan of an index of format major 2 (SiftV6Spec.vqual), and its equalities of a periodic calendar step, as zone tests of its granules
 * (sift_f2_scan_set_zones): a granule that lies outside every range of one holds no row that passes it and is not read. The quals still test every row of the granules that are.
 */
#include "postgres.h"

#include "agg/qual/bqual.h"
#include "col/am.h"
#include "col/f2/agg/scan_int.h"
#include "col/f2/sink/sink.h"

/* a range as the AND of the comparisons of its bounds, on the leaves of the index */
static bool
range_test(const SiftV6Input *in, const int *first, const SiftV6Range *r, SiftColPrune *out)
{
	SiftColPred *preds = palloc(2 * r->nbounds * sizeof(SiftColPred));
	SiftColValue *vals = palloc0(2 * r->nbounds * sizeof(SiftColValue));
	int			n = 0;

	for (int b = 0; b < r->nbounds; b++)
	{
		const SiftV6Bound *bd = &r->bound[b];
		const int	leaf = first[in->col[bd->slot].attno];

		if (bd->isnull)
			preds[n++] = (SiftColPred) {.col = leaf,.op = SIFT_COLOP_ISNULL};
		if (bd->has_lo)
		{
			vals[n].v.i = bd->lo;
			preds[n] = (SiftColPred) {.col = leaf,.op = SIFT_COLOP_GE,.nvals = 1,.vals = &vals[n]};
			n++;
		}
		if (bd->has_hi)
		{
			vals[n].v.i = bd->hi;
			preds[n] = (SiftColPred) {.col = leaf,.op = SIFT_COLOP_LE,.nvals = 1,.vals = &vals[n]};
			n++;
		}
	}
	*out = (SiftColPrune) {.preds = preds,.npreds = n};
	return n > 0;
}

const SiftColPrune *
sift_f2_v6_zone_tests(const SiftV6Spec *spec, Relation index, int *n, SiftV6ScanStats *stats)
{
	SiftColPrune *zones = palloc(SIFT_V6_MAX_SETS * sizeof(SiftColPrune));
	const int  *first = sift_col_rel(index)->first;
	SiftV6Input in;

	*n = 0;
	sift_v6_input_init_rel(&in, index);
	foreach_ptr(Node, clause, spec->vqual)
	{
		SiftV6RangeSet set;
		SiftColPrune *kids;
		bool		ok;

		if (*n == SIFT_V6_MAX_SETS || !sift_v6_ranges_of(CurrentMemoryContext, clause, SIFT_V6_BQ_RELID, NULL, &in, &set) || set.nranges == 0)
			continue;
		kids = palloc(set.nranges * sizeof(SiftColPrune));
		ok = true;
		for (int i = 0; i < set.nranges && ok; i++)
			ok = range_test(&in, first, &set.range[i], &kids[i]);
		if (ok)
			zones[(*n)++] = (SiftColPrune) {.any = true,.nkids = set.nranges,.kids = kids};
		stats->src_filters += ok && set.periodic;
	}
	return zones;
}
