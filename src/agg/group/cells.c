/*
 * agg/group/cells.c - pre-aggregated cells folded into the groups of SiftAggV6 (agg/group/int.h): a cell stands for the rows of one group of a granule, its key one row of the key
 * vectors and its partial states the states the rows would have made, which combine into the group's exactly as two partial states of a parallel run do. The distinct values a cell holds go through
 * the same sets the rows of a batch do.
 */
#include "postgres.h"

#include "agg/group/int.h"
#include "agg/group/compact.h"

static uint16
identity_sel(SiftV6Group *g, uint16 n)
{
	for (uint16 c = 0; c < n; c++)
		g->sel[c] = c;
	return n;
}

/* From here the groups of the set are in its group table alone: what the compact table holds moves there, and neither it nor the lock of the first groups is used again */
void
v6g_table_only(SiftV6Group *g, V6Set *set)
{
	set->ht.lock_at = 0;
	v6g_compact_flush(g, set);
	set->cmp_off = true;
	if (set->ht.count == 0)
		v6g_presize_set(g, set);
}

bool
sift_v6_group_takes_cells(const SiftV6Group *g)
{
	return !g->frozen;
}

void
sift_v6_group_merge_cells(SiftV6Group *g, SiftVec *const *keyvec, const V6Acc *acc, uint16 n)
{
	V6Set	   *set = &g->set[0];
	const int	naggs = g->spec->naggs;
	MemoryContext old = MemoryContextSwitchTo(g->cx);

	SIFT_INVARIANT(!g->frozen, "cells reached a table that sends the rows of its other groups away");
	if (set->nk > 0)
	{
		v6g_table_only(g, set);
		v6g_grouped_slots(g, set, identity_sel(g, n), keyvec);
	}
	for (uint16 c = 0; c < n; c++)
		for (int j = 0; j < naggs; j++)
			v6g_merge_acc(g, &g->spec->agg[j], set->nk == 0 ? &set->plain[j] : (V6Acc *) (sift_ht_ext(&set->ht, g->slots[c]) + g->off[j]), &acc[c * naggs + j]);
	v6g_spill_if_over(g);
	MemoryContextSwitchTo(old);
}

void
sift_v6_group_merge_dist(SiftV6Group *g, int agg, SiftVec *const *keyvec, SiftVec *arg, uint16 n)
{
	V6Set	   *set = &g->set[0];
	MemoryContext old = MemoryContextSwitchTo(g->cx);
	SiftVec   **argvec = palloc0(Max(g->spec->naggs, 1) * sizeof(SiftVec *));
	uint64		all[SIFT_VEC_WORDS] = {0};

	SIFT_INVARIANT(!g->frozen, "distinct values of cells reached a table that sends the rows of its other groups away");
	argvec[agg] = arg;
	if (set->nk > 0)
	{
		v6g_table_only(g, set);
		v6g_grouped_slots(g, set, identity_sel(g, n), keyvec);
	}
	for (uint16 r = 0; r < n; r++)
		all[r >> 6] |= UINT64CONST(1) << (r & 63);
	if (set->nk == 0)
		v6g_dist_plain(g, set, agg, n, all, argvec);
	else
		v6g_dist_group_rows(g, set, agg, argvec, n);
	pfree(argvec);
	v6g_spill_if_over(g);
	MemoryContextSwitchTo(old);
}
