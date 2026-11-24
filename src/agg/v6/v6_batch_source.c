/*
 * agg/v6/v6_batch_source.c - what a storage format asks of the operators of SiftAggV6 while it makes the batches of a scan (agg/batch_source.h): to keep the program of the quals as long as
 * the source and to cover the zone of the timestamps of a batch before the program reads them.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "utils/memutils.h"

#include "agg/v6/v6_vexpr.h"
#include "core/zoneoff.h"

SiftV6Vexpr *
sift_v6_vexpr_dup(MemoryContext cx, const SiftV6Vexpr *quals)
{
	SiftV6Vexpr *copy = MemoryContextAlloc(cx, sizeof(SiftV6Vexpr));

	*copy = *quals;
	return copy;
}

void
sift_v6_vexpr_cover(const SiftV6Vexpr *quals, const SiftV6Input *in, const SiftV6Batch *b)
{
	int64		lo = INT64_MAX,
				hi = INT64_MIN;

	if (quals->zone == NULL)
		return;
	for (int i = 0; i < in->ncols; i++)
	{
		const SiftVec *v = &b->col[i];

		if (in->col[i].typid != TIMESTAMPTZOID && in->col[i].typid != TIMESTAMPOID)
			continue;
		for (int r = 0; r < b->n; r++)
			if ((v->valid[r >> 6] >> (r & 63)) & 1)
			{
				lo = Min(lo, ((const int64 *) v->data)[r]);
				hi = Max(hi, ((const int64 *) v->data)[r]);
			}
	}
	if (lo <= hi)
		zone_cover(quals->zone, lo, hi);
}
