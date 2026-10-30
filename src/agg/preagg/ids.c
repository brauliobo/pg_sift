/*
 * agg/preagg/ids.c - the group ids of the keys of a histogram
 * The id of a group for each key value of a histogram, and the group of each id.
 */
#include "postgres.h"

#include "agg/agg_int.h"

/*
 * The ids of key ki for the kept groups or tuples: the unit's dictionary entries, or the integers. A group is NULL when isnull[g] (NULL: the
 * groups from nonnull on are, the NULL group of a histogram being its last)
 */
void
preagg_key_ids(AggExec *ex, int ki, const int64 *key, const uint8 *isnull, uint32 nonnull, const SiftSrcDict *dict, const uint32 *kept, int nkept,
			   uint32 *ids)
{
	KeyState   *k = &ex->ks[ki];
	int32		mn = PG_INT32_MAX,
				mx = PG_INT32_MIN;
	uint32	   *map = NULL;
	bool		cover = false;

#define PA_NULL(g) (isnull ? isnull[g] : (g) >= nonnull)
	if (k->numk)
	{
		/* the program of one leaf: the ids of its values, as the scan gives them */
		for (int j = 0; j < nkept; j++)
			ids[j] = PA_NULL(kept[j]) ? 0 : idmap_id(&k->im, key[kept[j]]);
		return;
	}
	if (k->bucket || k->cal)
	{
		MemoryContext old = MemoryContextSwitchTo(ex->tcxt);

		/* a calendar dim: its keys are the first days of cells (days since 2000-01-01), whose expression is evaluated once for each cell */
		if (!k->wcap)
			ident_go_wide(ex, k);
		for (int j = 0; j < nkept; j++)
			ids[j] = PA_NULL(kept[j]) ? key_null_id(ex, ki) : ident_wide_id(ex, k, key[kept[j]]);
		MemoryContextSwitchTo(old);
		return;
	}
	if (dict)
		map = dict_kmap(ex, dict_slot(ex, dict), dict, ki);
	for (int j = 0; j < nkept && !dict; j++)
		if (!PA_NULL(kept[j]))
		{
			mn = Min(mn, (int32) key[kept[j]]);
			mx = Max(mx, (int32) key[kept[j]]);
		}
	if (!dict)
		cover = key_cover(ex, ki, mn <= mx, mn, mx, nkept);
	for (int j = 0; j < nkept; j++)
	{
		uint32		g = kept[j];

		if (dict)
			ids[j] = kmap_id(ex, map, dict, ki, PA_NULL(g) ? dict->n : (uint32) key[g]);
		else if (PA_NULL(g))
			ids[j] = key_null_id(ex, ki);
		else
			ids[j] = cover ? ((const uint32 *) k->rt.tab)[(int32) key[g] - k->rt.lo] : ident_wide_id(ex, k, (int32) key[g]);
	}
#undef PA_NULL
}

/* the groups of the table for n tuples that have the key ids ids[key * n + i]: compute_gids for ids that are known */
void
ids_to_gids(AggExec *ex, int n, const uint32 *ids, uint32 *gid)
{
	int			nk = ex->spec->nkeys;

	if (nk == 0)
	{
		memset(gid, 0, n * sizeof(uint32));
		return;
	}
	for (int j = 0; j < nk && !ex->hash; j++)
		if (key_used(&ex->ks[j]) > ex->ks[j].cap)
		{
			direct_grow(ex);
			break;
		}
	if (!ex->hash)
	{
		for (int i = 0; i < n; i++)
		{
			uint32		g = 0;

			for (int j = 0; j < nk; j++)
				g += ids[(Size) j * n + i] * ex->ks[j].mult;
			gid[i] = g;
		}
		return;
	}
	for (int i = 0; i < n; i++)
	{
		uint32		k[SIFT_AGG_MAX_KEYS] = {0};

		for (int j = 0; j < nk; j++)
			k[j] = ids[(Size) j * n + i];
		gid[i] = hash_group(ex, k);
	}
	if (ex->ngroups > ex->gcap)
		groups_resize(ex, Max(ex->gcap * 2, ex->ngroups), NULL, ex->gcap);
}
