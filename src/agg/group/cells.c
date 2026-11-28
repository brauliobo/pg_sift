/*
 * agg/group/cells.c - pre-aggregated cells folded into the groups of SiftAggV6 (agg/group/int.h): a cell stands for the rows of one group of a granule, its key one row of the key
 * vectors and its partial states the states the rows would have made, which combine into the group's exactly as two partial states of a parallel run do. The cells go to the sets of
 * keys as the rows of a batch do, and the distinct values a cell holds go through the same sets the rows of a batch do.
 */
#include "postgres.h"

#include "agg/group/int.h"
#include "agg/datum.h"
#include "agg/group/compact.h"

static void
all_rows(SiftBatchSel *bs, uint16 n)
{
	*bs = (SiftBatchSel) {.n_raw = n,.n_active = n,.all_active = true};
	for (uint16 r = 0; r < n; r++)
		bs->mask[r >> 6] |= UINT64CONST(1) << (r & 63);
}

/* The keys of the cells of bs as the table groups them: a generic key as the codes of its interner (agg/datum.h), as the keys of the rows of a batch are */
static SiftVec *const *
cell_keys(SiftV6Group *g, SiftVec *const *keyvec, const SiftBatchSel *bs)
{
	return g->gkeys == NULL ? keyvec : sift_v6_genkeys_apply(g->gkeys, bs, keyvec);
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

/* The cells of bs into the groups of set: the state of an aggregate with a FILTER (WHERE ..) only from the cells its clause keeps, when the clauses are the group's to evaluate */
static void
cells_into_set(SiftV6Group *g, V6Set *set, const SiftBatchSel *bs, SiftVec *const *keyvec, const V6Acc *acc, bool filters)
{
	const int	naggs = g->spec->naggs;
	const uint16 n = sift_aggk6->mask_to_sel16(g->sel, bs->mask, bs->n_raw);

	if (set->nk > 0)
	{
		v6g_table_only(g, set);
		v6g_grouped_slots(g, set, n, keyvec);
	}
	else
		set->plain_row |= g->spec->constant_groups;
	for (int j = 0; j < naggs; j++)
	{
		const SiftV6Agg *a = &g->spec->agg[j];
		uint16		taken;
		const uint64 *take = filters ? v6g_filter_take(g, a, bs, &taken) : bs->mask;

		for (uint16 k = 0; k < n; k++)
		{
			const uint16 r = g->sel[k];

			if ((take[r >> 6] >> (r & 63)) & 1)
				v6g_merge_acc(g, a, set->nk == 0 ? &set->plain[j] : (V6Acc *) (sift_ht_ext(&set->ht, g->slots[k]) + g->off[j]), &acc[r * naggs + j]);
		}
	}
}

/* The cells of bs into every set of keys, or into the one the others are summed from, as the rows of a batch go (v6g_update_set); argvec NULL: the states are those of the FILTER already */
static void
cells_update(SiftV6Group *g, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec, const V6Acc *acc)
{
	MemoryContext old;
	int			only = -1;

	SIFT_INVARIANT(!g->frozen, "cells reached a table that sends the rows of its other groups away");
	if (bs->n_active == 0)
		return;
	old = MemoryContextSwitchTo(g->cx);
	if (argvec != NULL)
		v6g_filter_eval(g, bs, argvec);
	if (g->spec->nkeys > 0)
		keyvec = cell_keys(g, keyvec, bs);
	if (g->fold >= 0 && !g->fold_off)
	{
		only = g->fold;
		g->fold_owed = true;
	}
	for (int s = 0; s < g->nsets; s++)
		if (only < 0 || s == only)
			cells_into_set(g, &g->set[s], bs, keyvec, acc, argvec != NULL);
	v6g_spill_if_over(g);
	MemoryContextSwitchTo(old);
}

void
sift_v6_group_merge_cells(SiftV6Group *g, SiftVec *const *keyvec, const V6Acc *acc, uint16 n)
{
	SiftBatchSel bs;

	all_rows(&bs, n);
	cells_update(g, &bs, keyvec, NULL, acc);
}

void
sift_v6_group_update_cells(SiftV6Group *g, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec, const V6Acc *acc)
{
	cells_update(g, bs, keyvec, argvec, acc);
}

void
sift_v6_group_merge_dist(SiftV6Group *g, int agg, SiftVec *const *keyvec, SiftVec *arg, uint16 n)
{
	V6Set	   *set = &g->set[0];
	MemoryContext old = MemoryContextSwitchTo(g->cx);
	SiftVec   **argvec = palloc0(Max(g->spec->naggs, 1) * sizeof(SiftVec *));
	SiftBatchSel bs;

	SIFT_INVARIANT(!g->frozen, "distinct values of cells reached a table that sends the rows of its other groups away");
	all_rows(&bs, n);
	argvec[agg] = arg;
	if (set->nk > 0)
	{
		v6g_table_only(g, set);
		v6g_grouped_slots(g, set, sift_aggk6->mask_to_sel16(g->sel, bs.mask, n), cell_keys(g, keyvec, &bs));
	}
	if (set->nk == 0)
		v6g_dist_plain(g, set, agg, n, bs.mask, argvec);
	else
		v6g_dist_group_rows(g, set, agg, argvec, n);
	pfree(argvec);
	v6g_spill_if_over(g);
	MemoryContextSwitchTo(old);
}
