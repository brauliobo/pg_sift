/*
 * col/f2/ttl.c - the classification of the parts of a relation against the cutoff of its TTL and the plan of the commit that drops the expired ones (col/f2/ttl.h).
 */
#include "core/host.h"

#include "col/f2/ttl.h"
#include "col/f2/part/format.h"

SiftTtlVerdict
sift_ttl_classify(const SiftPartDesc *d, int slot, int64 cutoff)
{
	if (!(d->flags & (slot == 0 ? SIFT_PART_FLAG_HAS_TIME0 : SIFT_PART_FLAG_HAS_TIME1)) || d->time_min[slot] > d->time_max[slot])
		return SIFT_TTL_NO_BOUNDS;
	if (d->time_max[slot] < cutoff)
		return SIFT_TTL_EXPIRED;
	return d->time_min[slot] < cutoff ? SIFT_TTL_PARTIAL : SIFT_TTL_KEEP;
}

bool
sift_ttl_droppable(const SiftPartDesc *d, FullTransactionId horizon)
{
	return (d->flags & SIFT_PART_FLAG_ALL_VISIBLE) && !(d->flags & SIFT_PART_FLAG_UNFROZEN) &&
		(!FullTransactionIdIsValid(d->max_xid) || FullTransactionIdPrecedes(d->max_xid, horizon));
}

SiftTtlPlan *
sift_ttl_plan(const SiftPartDesc *parts, int n, int slot, int64 cutoff, FullTransactionId horizon)
{
	SiftTtlPlan *p = palloc0(sizeof(SiftTtlPlan));

	p->drop_gens = palloc(Max(n, 1) * sizeof(uint64));
	p->mark_gens = palloc(Max(n, 1) * sizeof(uint64));
	p->unmark_gens = palloc(Max(n, 1) * sizeof(uint64));
	for (int i = 0; i < n; i++)
	{
		const SiftPartDesc *d = &parts[i];
		SiftTtlVerdict v = sift_ttl_classify(d, slot, cutoff);
		bool		flagged = (d->ext_flags & SIFT_PART_EXT_EXPIRED) != 0;

		if (v == SIFT_TTL_EXPIRED && sift_ttl_droppable(d, horizon))
			p->drop_gens[p->ndrop++] = d->part_gen;
		else if (v == SIFT_TTL_EXPIRED && !flagged)
			p->mark_gens[p->nmark++] = d->part_gen;
		else if (v != SIFT_TTL_EXPIRED && flagged)
			p->unmark_gens[p->nunmark++] = d->part_gen;
		p->npartial += v == SIFT_TTL_PARTIAL;
		p->nkeep += v == SIFT_TTL_KEEP || v == SIFT_TTL_NO_BOUNDS;
	}
	return p;
}

void
sift_ttl_plan_free(SiftTtlPlan *plan)
{
	pfree(plan->drop_gens);
	pfree(plan->mark_gens);
	pfree(plan->unmark_gens);
	pfree(plan);
}

int
sift_ttl_due_deletes(const SiftTierPartRef *refs, int n, TimestampTz now, uint64 *gens)
{
	int			k = 0;

	for (int i = 0; i < n; i++)
		if ((refs[i].flags & SIFT_TIER_DELETE_PENDING) && refs[i].expires_at <= now)
			gens[k++] = refs[i].part_gen;
	return k;
}
