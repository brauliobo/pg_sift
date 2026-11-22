/*
 * col/f2/f2_drop_plan.c - what sift.drop_parts_before decides from descriptors alone (col/f2/f2_drop_parts.h): the cutoff in the unit of the column, which partitions can be skipped without a page,
 * whether the bounds of a partition still hold, and the plan and the tally of its parts.
 */
#include "core/host.h"

#include "col/f2/f2_drop_parts.h"
#include "col/f2/part_format.h"

int64
sift_drop_cutoff_units(int64 clock_us, int64 unit)
{
	return clock_us / unit + (clock_us % unit > 0 ? 1 : 0);
}

bool
sift_drop_partition_skippable(const SiftPartitionEntry *e, int slot, int64 cutoff)
{
	return (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0 && e->time_min[slot] >= cutoff;
}

bool
sift_drop_time_dirty(const SiftColDeltaAnchor *anchor, int slot)
{
	return (anchor->flags & (slot == 0 ? SIFT_CDLA_DIRTY_TIME0 : SIFT_CDLA_DIRTY_TIME1)) != 0;
}

static SiftTtlPlan *
empty_plan(int n)
{
	SiftTtlPlan *p = palloc0(sizeof(SiftTtlPlan));

	p->drop_gens = palloc(Max(n, 1) * sizeof(uint64));
	p->mark_gens = palloc(Max(n, 1) * sizeof(uint64));
	p->unmark_gens = palloc(Max(n, 1) * sizeof(uint64));
	return p;
}

SiftTtlPlan *
sift_drop_plan(const SiftPartDesc *parts, int np, int slot, int64 cutoff, FullTransactionId horizon, bool time_dirty, SiftDropResult *tally)
{
	SiftTtlPlan *plan = time_dirty ? empty_plan(np) : sift_ttl_plan(parts, np, slot, cutoff, horizon);
	int			next = 0;

	for (int i = 0; i < np; i++)
	{
		const SiftPartDesc *d = &parts[i];
		SiftTtlVerdict v = sift_ttl_classify(d, slot, cutoff);

		tally->parts_partial += v == SIFT_TTL_PARTIAL;
		if (next < plan->ndrop && plan->drop_gens[next] == d->part_gen)
		{
			next++;
			tally->parts_dropped++;
			tally->rows_dropped += (int64) (d->nrows - d->ndead);
			tally->bytes_reclaimed += (int64) d->nbytes;
		}
		else
			tally->parts_unsettled += v == SIFT_TTL_EXPIRED;
	}
	return plan;
}
