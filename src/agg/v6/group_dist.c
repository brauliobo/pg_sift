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
#include "agg/v6/v6_group_compact.h"

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
	sift_dkey_init(&g->pkey, g->cx);
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
	else if (set->packed)
	{
		SiftInlineKey16 ik;
		uint64		h = 0;

		memcpy(&ik, prefix + sizeof(nulls), sizeof(ik));
		h = sift_pack_hash(&ik);
		sift_ht_upsert_batch(&set->ht, &h, &ik, 1, &s);
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

static void
move_value(void *arg, uint64 v)
{
	const struct
	{
		SiftDistinct *d;
		uint32		slot;
	}		   *to = arg;

	sift_distinct_add_u64(to->d, to->slot, v);
}

static void
flat_move(SiftV6Group *g, V6Set *set, uint32 slot)
{
	struct
	{
		SiftDistinct *d;
		uint32		slot;
	}			to = {dist_of(g, set), slot};

	sift_du64_each(set->flat[slot], move_value, &to);
	sift_du64_free(set->flat[slot]);
	set->flat[slot] = NULL;
	set->flat_moved[slot] = true;
}

static void
flat_add(SiftV6Group *g, V6Set *set, uint32 slot, const uint64 *vals, uint32 n)
{
	uint32		done = 0;

	if (set->flat == NULL)
	{
		set->flat = MemoryContextAllocZero(g->cx, g->ndist * sizeof(SiftDistinctU64 *));
		set->flat_moved = MemoryContextAllocZero(g->cx, g->ndist * sizeof(bool));
	}
	if (!set->flat_moved[slot])
	{
		if (set->flat[slot] == NULL)
			set->flat[slot] = sift_du64_create(g->cx, g->dist_budget / ((uint64) g->nsets * g->ndist));
		done = sift_du64_add(set->flat[slot], vals, n);
		if (done == n)
			return;
		flat_move(g, set, slot);
	}
	for (uint32 i = done; i < n; i++)
		sift_distinct_add_u64(dist_of(g, set), slot, vals[i]);
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

	ds->mixed = true;
	sift_dkey_reset(&g->dkey);
	sift_dkey_bytes(&g->dkey, bytes, len);
	if (g->dist_put != NULL)
		g->dist_put(g->dist_arg, (uint32) (ds->set - g->set), ds->slot, g->dkey.buf, g->dkey.len);
	else
		sift_distinct_add_dkey(dist_of(g, ds->set), ds->slot, &g->dkey);
}

static V6DictSlot *
dict_slot_of(SiftV6Group *g, V6Set *set, uint32 slot)
{
	if (set->dict == NULL)
	{
		set->dict = MemoryContextAllocZero(g->cx, Max(g->ndist, 1) * sizeof(V6DictSlot));
		for (int k = 0; k < g->ndist; k++)
			set->dict[k] = (V6DictSlot) {.g = g,.set = set,.slot = (uint32) k};
	}
	return &set->dict[slot];
}

static V6DictSlot *
dict_slot(SiftV6Group *g, V6Set *set, uint32 slot)
{
	V6DictSlot *ds = dict_slot_of(g, set, slot);

	if (ds->seen == NULL)
		ds->seen = sift_dictseen_create(g->cx, g->dist_budget > 0 ? g->dist_budget / (uint64) g->nsets / 2 : DICT_PARALLEL_CAP);
	return ds;
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

bool
v6g_dist_dict_only(const SiftV6Group *g)
{
	if (g->nsets != 1 || g->set[0].nk != 0 || g->nfilters != 0 || g->spec->naggs == 0)
		return false;
	for (int j = 0; j < g->spec->naggs; j++)
	{
		const SiftV6Agg *a = &g->spec->agg[j];

		if (a->kind != SV6_COUNT_DISTINCT || a->nxcols != 0 || a->arg.derive != 0 || a->fclause != 0)
			return false;
	}
	return true;
}

bool
v6g_dist_dict_fits(const SiftDictSrc *src)
{
	return sift_dictseen_fits(src);
}

void
v6g_dist_dict_part(SiftV6Group *g, int attno, const SiftDictSrc *src)
{
	for (int j = 0; j < g->spec->naggs; j++)
		if (g->spec->agg[j].arg.attno == attno)
		{
			V6DictSlot *ds = dict_slot(g, &g->set[0], (uint32) g->dslot[j]);

			sift_dictseen_part(ds->seen, src, dict_emit, ds);
		}
}

/* The strings of a set of no keys that its parts have named, and nothing else has, are counted where they are: they are not rows of the exact set */
static void
dict_exact(SiftV6Group *g)
{
	for (int s = 0; s < g->nsets; s++)
		for (int k = 0; g->dist_put == NULL && g->set[s].nk == 0 && g->set[s].dict != NULL && k < g->ndist; k++)
		{
			V6DictSlot *ds = &g->set[s].dict[k];

			ds->direct = ds->seen != NULL && !ds->mixed && sift_dictseen_exact(ds->seen, dict_emit, ds, &ds->direct_n) && !ds->mixed;
		}
}

void
v6g_dist_dict_drain(SiftV6Group *g, V6DistKeyFn fn, void *arg)
{
	for (int s = 0; s < g->nsets; s++)
		for (int k = 0; g->set[s].dict != NULL && k < g->ndist; k++)
			if (g->set[s].dict[k].seen != NULL && !g->set[s].dict[k].direct)
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

static void
v6g_dist_flat(SiftV6Group *g, V6Set *set, int j, uint16 n_raw, const uint64 *take, SiftVec *const *argvec);

void
v6g_dist_plain(SiftV6Group *g, V6Set *set, int j, uint16 n_raw, const uint64 *take, SiftVec *const *argvec)
{
	const SiftV6Agg *a = &g->spec->agg[j];
	const SiftVec *v = argvec[j];
	SiftDistinct *d;
	const uint32 slot = (uint32) g->dslot[j];
	const bool	row_key = a->nxcols > 0 || sift_v6_class(a->arg.typid) == SV6C_F64 || sift_v6_class(a->arg.typid) == SV6C_TEXT;
	uint32		r;

	if (g->dist_put == NULL && set->nk == 0 && !row_key)
	{
		v6g_dist_flat(g, set, j, n_raw, take, argvec);
		return;
	}
	d = g->dist_put != NULL ? NULL : dist_of(g, set);

	if (v->type == SVEC_DICT32 && a->nxcols == 0 && dict_batch(g, set, j, v, n_raw, take))
		return;
	dict_slot_of(g, set, slot)->mixed = true;
	V6_FOR_EACH_ROW(take, n_raw, r)
	{
		if (a->nxcols == 0 && !is_valid(v, r))
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

static void
v6g_dist_flat(SiftV6Group *g, V6Set *set, int j, uint16 n_raw, const uint64 *take, SiftVec *const *argvec)
{
	const SiftVec *v = argvec[j];
	uint64		vals[SIFT_VEC_LEN];
	uint32		n = 0;

	uint32		r;

	V6_FOR_EACH_ROW(take, n_raw, r)
		if (is_valid(v, r))
			vals[n++] = (uint64) int_at(v, r);
	if (n > 0)
		flat_add(g, set, (uint32) g->dslot[j], vals, n);
}

/* The group of a slot of the set as the bytes its COUNT(DISTINCT) pairs name it by: its nulls and its key */
static V6RowKey
slot_key(const V6Set *set, const SiftAggSlot64 *s)
{
	if (set->wide)
		return (V6RowKey) {set->ht.arena + s->key.wide.arena_off, s->key.wide.key_len, s->key.wide.key_nulls};
	if (set->packed)
		return (V6RowKey) {&s->key, sizeof(SiftInlineKey16), 0};
	return (V6RowKey) {&s->key.i64[0], sizeof(int64), (uint64) s->key.i64[1]};
}

typedef struct MovePairs
{
	SiftV6Group *g;
	V6Set	   *set;
	uint32		slot;
	SiftDistinctKey *k;
	const SiftAggSlot64 **by_gid;
} MovePairs;

static void
add_pair_key(SiftV6Group *g, V6Set *set, uint32 slot, SiftDistinctKey *k, const V6RowKey *rk, int64 value)
{
	const uint32 plen = (uint32) sizeof(rk->nulls) + rk->len;

	sift_dkey_reset(k);
	sift_bytes_put(k, &plen, sizeof(plen));
	sift_bytes_put(k, &rk->nulls, sizeof(rk->nulls));
	sift_bytes_put(k, rk->p, rk->len);
	sift_dkey_i64(k, value);
	sift_distinct_add_dkey(dist_of(g, set), slot, k);
}

static void
add_pair(SiftV6Group *g, V6Set *set, uint32 slot, SiftDistinctKey *k, const SiftAggSlot64 *s, int64 value)
{
	const V6RowKey rk = slot_key(set, s);

	add_pair_key(g, set, slot, k, &rk, value);
}

static void
move_pair(void *arg, uint32 gid, uint64 value)
{
	const MovePairs *to = arg;

	add_pair(to->g, to->set, to->slot, to->k, to->by_gid[gid], (int64) value);
}

/* The pairs of the grouped flat set of the slot to the generic set, which names a group by its bytes and so keeps it when the group table is emptied; the slots of the table say what each group number is */
static void
gflat_move(SiftV6Group *g, V6Set *set, uint32 slot)
{
	SiftDistinctKey k;
	const SiftAggSlot64 *s;
	MovePairs	to = {g, set, slot, &k, palloc(Max(set->ht.ext_used, 1) * sizeof(SiftAggSlot64 *))};

	SIFT_HT_FOREACH(&set->ht, s)
		to.by_gid[s->ext_idx] = s;
	sift_dkey_init(&k, g->cx);
	sift_du64g_pairs(set->gflat[slot], move_pair, &to);
	sift_dkey_free(&k);
	pfree(to.by_gid);
	sift_du64g_free(set->gflat[slot]);
	set->gflat[slot] = NULL;
	set->gflat_moved[slot] = true;
}

typedef struct CompactPairs
{
	SiftV6Group *g;
	V6Set	   *set;
	uint32		slot;
	SiftDistinctKey *k;
} CompactPairs;

static void
compact_pair(void *arg, uint32 gid, uint64 value)
{
	const CompactPairs *to = arg;
	V6RowKey	rk;

	v6g_compact_rowkey(to->set, gid, &rk);
	add_pair_key(to->g, to->set, to->slot, to->k, &rk, (int64) value);
}

/* The grouped flat set of the slot is full while the groups are in the compact table: its pairs, and the batch's from done on, go to the generic set under the bytes of their groups, and the table is flushed when the batch is done */
static void
compact_move(SiftV6Group *g, V6Set *set, uint32 slot, const V6DistBatch *b, uint32 done, uint32 m)
{
	SiftDistinctKey k;
	CompactPairs to = {g, set, slot, &k};

	sift_dkey_init(&k, g->cx);
	sift_du64g_pairs(set->gflat[slot], compact_pair, &to);
	for (uint32 i = done; i < m; i++)
		compact_pair(&to, b->gid[i], b->val[i]);
	sift_dkey_free(&k);
	sift_du64g_free(set->gflat[slot]);
	set->gflat[slot] = NULL;
	set->gflat_moved[slot] = true;
	v6g_compact_defer_flush(set);
}

void
v6g_dist_gflat_flush(SiftV6Group *g, V6Set *set)
{
	for (int k = 0; set->gflat != NULL && k < g->ndist; k++)
		if (set->gflat[k] != NULL)
			gflat_move(g, set, (uint32) k);
}

/* The grouped flat set of the slot, made when the first pair comes; NULL once its pairs moved to the generic set */
static SiftDu64G *
gflat_of(SiftV6Group *g, V6Set *set, uint32 slot)
{
	if (set->gflat == NULL)
	{
		set->gflat = MemoryContextAllocZero(g->cx, g->ndist * sizeof(SiftDu64G *));
		set->gflat_moved = MemoryContextAllocZero(g->cx, g->ndist * sizeof(bool));
	}
	if (set->gflat_moved[slot])
		return NULL;
	if (set->gflat[slot] == NULL)
		set->gflat[slot] = sift_du64g_create(g->cx, g->dist_budget / ((uint64) g->nsets * g->ndist));
	return set->gflat[slot];
}

static void
row_to_dist(SiftV6Group *g, V6Set *set, int j, SiftVec *const *argvec, uint16 r, const SiftAggSlot64 *s)
{
	const SiftV6Agg *a = &g->spec->agg[j];
	const V6RowKey rk = slot_key(set, s);
	const uint32 plen = (uint32) sizeof(rk.nulls) + rk.len;

	sift_dkey_reset(&g->dkey);
	sift_bytes_put(&g->dkey, &plen, sizeof(plen));
	sift_bytes_put(&g->dkey, &rk.nulls, sizeof(rk.nulls));
	sift_bytes_put(&g->dkey, rk.p, rk.len);
	append_row(g, a, j, argvec, r);
	if (g->dist_put != NULL)
		g->dist_put(g->dist_arg, (uint32) (set - g->set), (uint32) g->dslot[j], g->dkey.buf, g->dkey.len);
	else
		sift_distinct_add_dkey(dist_of(g, set), (uint32) g->dslot[j], &g->dkey);
}

/* The rows of a batch of the groups of a set, for aggregate j (g->slots[k] is the group of row g->sel[k]): the pairs of the flat kinds go to the grouped flat set a batch at a time, the others one row at a time */
void
v6g_dist_group_rows(SiftV6Group *g, V6Set *set, int j, SiftVec *const *argvec, uint16 n)
{
	const SiftV6Agg *a = &g->spec->agg[j];
	const bool	flat = g->dist_put == NULL && a->nxcols == 0 && sift_v6_class(a->arg.typid) != SV6C_F64 && sift_v6_class(a->arg.typid) != SV6C_TEXT;
	SiftDu64G  *d = flat ? gflat_of(g, set, (uint32) g->dslot[j]) : NULL;
	V6DistBatch *b = g->dbatch;
	uint32		m = 0,
				done;

	if (b == NULL)
		b = g->dbatch = MemoryContextAllocZero(g->cx, sizeof(V6DistBatch));
	for (uint16 k = 0; k < n; k++)
	{
		const uint16 r = g->sel[k];

		if (!v6g_row_takes(g, a, r) || (a->nxcols == 0 && !is_valid(argvec[j], r)))
			continue;
		if (d == NULL)
		{
			row_to_dist(g, set, j, argvec, r, g->slots[k]);
			continue;
		}
		b->gid[m] = g->gid_direct ? g->gid[k] : g->slots[k]->ext_idx;
		b->val[m] = (uint64) int_at(argvec[j], r);
		b->row[m++] = k;
	}
	if (m == 0 || (done = sift_du64g_add_batch(d, b->gid, b->val, m)) == m)
		return;
	if (g->gid_direct)
	{
		compact_move(g, set, (uint32) g->dslot[j], b, done, m);
		return;
	}
	gflat_move(g, set, (uint32) g->dslot[j]);
	for (uint32 i = done; i < m; i++)
		row_to_dist(g, set, j, argvec, g->sel[b->row[i]], g->slots[b->row[i]]);
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

static void
v6g_dist_free_set(V6Set *set)
{
	if (set->dist != NULL)
		sift_distinct_free(set->dist);
	set->dist = NULL;
	for (int k = 0; set->flat != NULL && k < set->owner->ndist; k++)
		if (set->flat[k] != NULL)
			sift_du64_free(set->flat[k]);
	set->flat = NULL;
	for (int k = 0; set->gflat != NULL && k < set->owner->ndist; k++)
		if (set->gflat[k] != NULL)
			sift_du64g_free(set->gflat[k]);
	set->gflat = NULL;
}

static void
emit_group(void *arg, uint32 gid, uint64 count)
{
	const struct
	{
		V6Set	   *set;
		uint32		off;
		int			dk;
	}		   *at = arg;

	if (at->set->cmp != NULL)
		v6g_compact_dist(at->set, at->dk, gid, count);
	else
		((V6Acc *) (sift_ht_ext_idx(&at->set->ht, gid) + at->off))->v0 += count;
}

void
v6g_dist_finish(SiftV6Group *g)
{
	dict_exact(g);
	v6g_dist_dict_drain(g, dist_finish_add, g);
	for (int s = 0; s < g->nsets; s++)
	{
		V6Set	   *set = &g->set[s];

		for (int k = 0; set->gflat != NULL && k < g->ndist; k++)
			if (set->gflat[k] != NULL)
			{
				const struct
				{
					V6Set	   *set;
					uint32		off;
					int			dk;
				}			at = {set, g->off[g->dagg[k]], k};

				sift_du64g_groups(set->gflat[k], emit_group, (void *) &at);
			}
		if (set->dist != NULL)
			sift_distinct_finish(set->dist);
		if (set->nk == 0)
			for (int k = 0; k < g->ndist; k++)
			{
				if (set->dict != NULL && set->dict[k].direct)
					set->plain[g->dagg[k]].v0 = set->dict[k].direct_n;
				else if (set->flat != NULL && set->flat[k] != NULL)
					set->plain[g->dagg[k]].v0 = sift_du64_count(set->flat[k]);
				else if (set->dist != NULL)
					set->plain[g->dagg[k]].v0 = sift_distinct_count(set->dist, (uint32) k);
			}
		v6g_dist_free_set(set);
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
		v6g_dist_free_set(&g->set[s]);
	}
	sift_dkey_free(&g->dkey);
	sift_dkey_free(&g->pkey);
}
