/*
 * agg/v6/group_dist.c - count(DISTINCT) of the groups of SiftAggV6 through the exact set of v6_distinct.h: any number of such aggregates in a statement, on one column or on a row of several
 * (COUNT(DISTINCT (a, b))), with or without keys, whatever the number of groups.
 *
 * Each set of keys (a plain GROUP BY is one; GROUPING SETS, ROLLUP and CUBE have one each) has one SiftDistinct whose slots are the COUNT(DISTINCT) aggregates of the statement. A set with no keys
 * counts in them as the set of an ungrouped aggregate does. A set with keys holds the bytes of the group in each key (its nulls and its key bytes, the prefix of v6_distinct.h): the group has no
 * number, which is what lets its accumulators leave for a temporary file when the groups outgrow their share of memory and come back merged. When the scan ends, the set hands the counts of each
 * (aggregate, group), in pieces, to dist_emit, which adds them to the accumulator of the group: V6Acc.v0 of a COUNT(DISTINCT) is a count that merges by addition like COUNT.
 */
#include "postgres.h"

#include "agg/group/ht.h"
#include "agg/v6/group_int.h"

#define DICT_PARALLEL_CAP		((uint64) 8 << 20)	/* the strings a participant of a parallel run holds by dictionary before it hands them on: what its own set of distinct values has no budget for either */

void
v6g_dist_init(SiftV6Group *g)
{
	const SiftV6Spec *spec = g->spec;
	int			k = 0;

	g->dslot = palloc(Max(spec->naggs, 1) * sizeof(int));
	g->dagg = palloc(Max(spec->naggs, 1) * sizeof(int));
	for (int j = 0; j < spec->naggs; j++)
	{
		g->dslot[j] = -1;
		if (spec->agg[j].kind == SV6_COUNT_DISTINCT)
		{
			g->dagg[k] = j;
			g->dslot[j] = k++;
		}
	}
	g->ndist = k;
	g->dist_budget = k > 0 ? g->budget / 2 : 0;
	sift_dkey_init(&g->dkey, g->cx);
}

/* The bytes of a group in a grouped set, added to its accumulator as a piece of the count of aggregate dagg[slot] */
static void
dist_emit(void *arg, uint32 slot, const uint8 *prefix, uint32 plen, uint64 count)
{
	V6Set	   *set = arg;
	SiftV6Group *g = set->owner;
	SiftAggSlot64 *s;
	uint64		nulls;

	memcpy(&nulls, prefix, sizeof(nulls));
	sift_ht_reserve(&set->ht, 1);
	if (set->wide)
	{
		const uint8 *key = prefix + sizeof(nulls);
		const uint32 klen = plen - (uint32) sizeof(nulls);
		bool		found;

		s = sift_ht_upsert_wide(&set->ht, sift_aggk6_hash1(sift_aggk6_hash_bytes(key, klen) ^ nulls), key, klen, nulls, &found);
	}
	else
	{
		SiftInlineKey16 ik;
		uint64		h;

		memcpy(&ik.i64[0], prefix + sizeof(nulls), sizeof(int64));
		ik.i64[1] = (int64) nulls;
		h = sift_aggk6_hash1((uint64) ik.i64[0]);
		sift_ht_upsert_batch(&set->ht, &h, &ik, 1, &s);
	}
	((V6Acc *) (sift_ht_ext(&set->ht, s) + g->off[g->dagg[slot]]))->v0 += count;
	v6g_spill_if_over(g);
}

static SiftDistinct *
dist_of(SiftV6Group *g, V6Set *set)
{
	if (set->dist == NULL)
	{
		const uint64 budget = g->dist_budget / (uint64) g->nsets;

		set->dist = set->nk == 0 ? sift_distinct_create(g->cx, budget, 0) : sift_distinct_create_grouped(g->cx, budget, 0, dist_emit, set);
	}
	return set->dist;
}

/* the columns of the row r of aggregate j after what is in the key */
static void
append_row(SiftV6Group *g, const SiftV6Agg *a, int j, SiftVec *const *argvec, uint16 r)
{
	sift_dkey_vec(&g->dkey, argvec[j], r, NULL);
	for (int c = 0; c < a->nxcols; c++)
		sift_dkey_vec(&g->dkey, argvec[sift_v6_agg_xvec(g->spec, j) + c], r, NULL);
}

/* A string the dictionary path holds that must leave it (its cap, or a dictionary that is not in byte order): a row of the set */
static void
dict_emit(void *arg, const void *bytes, uint32 len)
{
	V6DictSlot *ds = arg;
	SiftV6Group *g = ds->g;

	sift_dkey_reset(&g->dkey);
	sift_dkey_bytes(&g->dkey, bytes, len);
	if (g->dist_put != NULL)
		g->dist_put(g->dist_arg, (uint32) (ds->set - g->set), ds->slot, g->dkey.buf, g->dkey.len);
	else
		sift_distinct_add_dkey(dist_of(g, ds->set), ds->slot, &g->dkey);
}

static V6DictSlot *
dict_slot(SiftV6Group *g, V6Set *set, uint32 slot)
{
	if (set->dict == NULL)
		set->dict = MemoryContextAllocZero(g->cx, Max(g->ndist, 1) * sizeof(V6DictSlot));
	if (set->dict[slot].seen == NULL)
		set->dict[slot] = (V6DictSlot) {.g = g,.set = set,.slot = slot,
			.seen = sift_dictseen_create(g->cx, g->dist_budget > 0 ? g->dist_budget / (uint64) g->nsets / 2 : DICT_PARALLEL_CAP)};
	return &set->dict[slot];
}

/* The codes of a batch of a column the parts code by dictionaries: false when the dictionary is too large, and the rows are read as strings */
static bool
dict_batch(SiftV6Group *g, V6Set *set, int j, const SiftVec *v, uint16 n_raw, const uint64 *take)
{
	V6DictSlot *ds = dict_slot(g, set, (uint32) g->dslot[j]);

	return sift_dictseen_batch(ds->seen, v->dict, v->data, take, v->all_valid ? NULL : v->valid, n_raw, dict_emit, ds);
}

typedef struct DrainArg
{
	V6DictSlot *ds;
	V6DistKeyFn fn;
	void	   *arg;
} DrainArg;

static void
drain_emit(void *arg, const void *bytes, uint32 len)
{
	const DrainArg *da = arg;
	SiftV6Group *g = da->ds->g;

	sift_dkey_reset(&g->dkey);
	sift_dkey_bytes(&g->dkey, bytes, len);
	da->fn(da->arg, (uint32) (da->ds->set - g->set), da->ds->slot, g->dkey.buf, g->dkey.len);
}

void
v6g_dist_dict_drain(SiftV6Group *g, V6DistKeyFn fn, void *arg)
{
	for (int s = 0; s < g->nsets; s++)
		for (int k = 0; g->set[s].dict != NULL && k < g->ndist; k++)
			if (g->set[s].dict[k].seen != NULL)
			{
				DrainArg	da = {&g->set[s].dict[k], fn, arg};

				sift_dictseen_drain(g->set[s].dict[k].seen, drain_emit, &da);
			}
}

uint64
v6g_dist_dict_load(const SiftV6Group *g)
{
	uint64		n = 0;

	for (int s = 0; s < g->nsets; s++)
		for (int k = 0; g->set[s].dict != NULL && k < g->ndist; k++)
			n += g->set[s].dict[k].seen != NULL ? sift_dictseen_count(g->set[s].dict[k].seen) : 0;
	return n;
}

static void
dist_finish_add(void *arg, uint32 set_idx, uint32 slot, const void *key, uint32 len)
{
	SiftV6Group *g = arg;

	sift_distinct_add(dist_of(g, &g->set[set_idx]), slot, key, len);
}

void
v6g_dist_plain(SiftV6Group *g, V6Set *set, int j, uint16 n_raw, const uint64 *take, SiftVec *const *argvec)
{
	const SiftV6Agg *a = &g->spec->agg[j];
	const SiftVec *v = argvec[j];
	SiftDistinct *d = g->dist_put != NULL ? NULL : dist_of(g, set);
	const uint32 slot = (uint32) g->dslot[j];
	const bool	row_key = a->nxcols > 0 || sift_v6_class(a->arg.typid) == SV6C_F64 || sift_v6_class(a->arg.typid) == SV6C_TEXT;

	if (v->type == SVEC_DICT32 && a->nxcols == 0 && dict_batch(g, set, j, v, n_raw, take))
		return;
	for (uint16 r = 0; r < n_raw; r++)
	{
		if (!((take[r >> 6] >> (r & 63)) & 1) || (a->nxcols == 0 && !is_valid(v, r)))
			continue;
		if (g->dist_put == NULL && !row_key)
		{
			sift_distinct_add_u64(d, slot, (uint64) int_at(v, r));
			continue;
		}
		sift_dkey_reset(&g->dkey);
		if (row_key)
			append_row(g, a, j, argvec, r);
		else
			sift_dkey_i64(&g->dkey, int_at(v, r));
		if (g->dist_put != NULL)
			g->dist_put(g->dist_arg, (uint32) (set - g->set), slot, g->dkey.buf, g->dkey.len);
		else
			sift_distinct_add_dkey(d, slot, &g->dkey);
	}
}

void
v6g_dist_group_row(SiftV6Group *g, V6Set *set, int j, SiftVec *const *argvec, uint16 r, const void *key, uint32 klen, uint64 nulls)
{
	const SiftV6Agg *a = &g->spec->agg[j];
	const uint32 plen = (uint32) sizeof(nulls) + klen;

	if (a->nxcols == 0 && !is_valid(argvec[j], r))
		return;
	sift_dkey_reset(&g->dkey);
	sift_bytes_put(&g->dkey, &plen, sizeof(plen));
	sift_bytes_put(&g->dkey, &nulls, sizeof(nulls));
	sift_bytes_put(&g->dkey, key, klen);
	append_row(g, a, j, argvec, r);
	if (g->dist_put != NULL)
		g->dist_put(g->dist_arg, (uint32) (set - g->set), (uint32) g->dslot[j], g->dkey.buf, g->dkey.len);
	else
		sift_distinct_add_dkey(dist_of(g, set), (uint32) g->dslot[j], &g->dkey);
}

void
v6g_dist_add_raw(SiftV6Group *g, V6Set *set, uint32 slot, const void *key, uint32 len, uint64 hash)
{
	sift_distinct_add_prehashed(dist_of(g, set), slot, key, len, hash);
}

void
v6g_dist_recycle(SiftV6Group *g)
{
	for (int s = 0; s < g->nsets; s++)
		if (g->set[s].dist != NULL)
			sift_distinct_reset(g->set[s].dist);
}

uint64
v6g_dist_count(SiftV6Group *g, uint32 k)
{
	V6Set	   *set = &g->set[0];

	if (set->dist == NULL)
		return 0;
	sift_distinct_finish(set->dist);
	return sift_distinct_count(set->dist, k);
}

void
v6g_dist_finish(SiftV6Group *g)
{
	v6g_dist_dict_drain(g, dist_finish_add, g);
	for (int s = 0; s < g->nsets; s++)
	{
		V6Set	   *set = &g->set[s];

		if (set->dist == NULL)
			continue;
		sift_distinct_finish(set->dist);
		if (set->nk == 0)
			for (int k = 0; k < g->ndist; k++)
				set->plain[g->dagg[k]].v0 = sift_distinct_count(set->dist, (uint32) k);
		sift_distinct_free(set->dist);
		set->dist = NULL;
	}
}

void
v6g_dist_free(SiftV6Group *g)
{
	for (int s = 0; s < g->nsets; s++)
	{
		for (int k = 0; g->set[s].dict != NULL && k < g->ndist; k++)
			if (g->set[s].dict[k].seen != NULL)
				sift_dictseen_free(g->set[s].dict[k].seen);
		if (g->set[s].dist != NULL)
		{
			sift_distinct_free(g->set[s].dist);
			g->set[s].dist = NULL;
		}
	}
	sift_dkey_free(&g->dkey);
}
