/*
 * agg/group/sets.c - grouping sets summed from one of them. When every set lies inside one (ROLLUP, CUBE, the sets of a report: (a, b), (a), (b), ()) and the states of every
 * aggregate add exactly, the rows of a batch go to that set alone and the groups of the others are the sums of its groups, so that a row is hashed and its aggregates updated
 * once instead of once per set. The sums are made before the groups of that set leave its table (the end of the scan, a spill, the export of a participant of a parallel run)
 * and before a full table starts sending the rows of its other groups away (a frozen state updates every set from then on).
 */
#include "postgres.h"

#include "agg/gpu/scan.h"
#include "agg/group/int.h"

/* Counts, sums and averages of integers and numerics, minima and maxima: float sums would add in another order than the rows came in, the others keep what does not add */
static bool
fold_agg(const SiftV6Agg *a)
{
	const SiftV6Class c = sift_v6_col_class(&a->arg);

	if (a->fclause != 0)
		return false;
	switch (a->kind)
	{
		case SV6_COUNT_STAR:
		case SV6_COUNT:
		case SV6_GROUPING:
			return true;
		case SV6_SUM:
		case SV6_AVG:
			return c == SV6C_I32 || c == SV6C_I64;
		case SV6_MIN:
		case SV6_MAX:
			return c == SV6C_I32 || c == SV6C_I64 || c == SV6C_F64 || c == SV6C_TEXT;
		default:
			return false;
	}
}

int
sift_v6_spec_fold_set(const SiftV6Spec *spec)
{
	if (spec->nsets < 2 || spec->topk > 0)
		return -1;
	for (int j = 0; j < spec->naggs; j++)
		if (!fold_agg(&spec->agg[j]))
			return -1;
	for (int s = 0; s < spec->nsets; s++)
	{
		bool		all = true;

		for (int c = 0; c < spec->nsets; c++)
			all &= (spec->set_mask[c] & ~spec->set_mask[s]) == 0;
		if (all)
			return s;
	}
	return -1;
}

/* The groups of a set: one for the set of no keys, whose table is never made */
static uint32
set_groups(const V6Set *set)
{
	return set->nk == 0 ? 1 : set->ht.count;
}

/* The set with the fewest groups among those summed already that holds every key of set t (a set written twice holds the other) */
static int
parent_of(const SiftV6Group *g, int t, const bool *done)
{
	int			best = -1;

	for (int p = 0; p < g->nsets; p++)
		if (done[p] && (g->set[t].mask & ~g->set[p].mask) == 0 && (best < 0 || set_groups(&g->set[p]) < set_groups(&g->set[best])))
			best = p;
	SIFT_INVARIANT(best >= 0, "grouping set %d has no set summed already that holds its keys", t);
	return best;
}

static void
fold_into(SiftV6Group *g, const V6Set *from, V6Set *to)
{
	const SiftSaltedHT *ht = &from->ht;
	int			pos[SIFT_V6_MAX_KEYS];

	if (from->nk == 0)
	{
		for (int j = 0; j < g->spec->naggs; j++)
			v6g_merge_acc(g, &g->spec->agg[j], &to->plain[j], &from->plain[j]);
		return;
	}
	for (int i = 0; i < to->nk; i++)
		for (int p = 0; p < from->nk; p++)
			if (from->kidx[p] == to->kidx[i])
				pos[i] = p;
	for (uint32 i = 0; i < ht->capacity; i++)
	{
		int64		key[SIFT_V6_MAX_KEYS];
		SiftV6StrIn str[SIFT_V6_MAX_KEYS];
		uint64		nulls,
					to_nulls = 0;
		const V6Acc *src;
		V6Acc	   *dst;

		if (!(ht->ctrl[i] & 0x80))
			continue;
		v6g_slot_keys(g, from, ht, &ht->slots[i], key, str, &nulls);
		for (int k = 0; k < to->nk; k++)
			to_nulls |= ((nulls >> pos[k]) & 1) << k;
		src = (const V6Acc *) sift_ht_ext(ht, &ht->slots[i]);
		dst = to->nk == 0 ? to->plain : (V6Acc *) sift_ht_ext(&to->ht, v6g_slot_of(g, to, &to->ht, key, str, to_nulls));
		for (int j = 0; j < g->spec->naggs; j++)
			v6g_merge_acc(g, &g->spec->agg[j], &dst[j], &src[j]);
	}
}

void
v6g_sets_settle(SiftV6Group *g)
{
	MemoryContext old;
	bool	   *done;

	if (!g->fold_owed)
		return;
	g->fold_owed = false;
	old = MemoryContextSwitchTo(g->cx);
	done = palloc0(g->nsets * sizeof(bool));
	done[g->fold] = true;
	for (int nk = g->set[g->fold].nk; nk >= 0; nk--)
		for (int t = 0; t < g->nsets; t++)
			if (!done[t] && g->set[t].nk == nk)
			{
				fold_into(g, &g->set[parent_of(g, t, done)], &g->set[t]);
				done[t] = true;
			}
	pfree(done);
	MemoryContextSwitchTo(old);
}
