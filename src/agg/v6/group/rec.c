/*
 * agg/v6/group/rec.c - the groups of SiftAggV6 that come as records, a key and the states of its aggregates (sift_v6_group_fold_rec). A record goes into the group table, which finds the group of its
 * key and merges the states. The records of a parallel run have no two of one key, so they need no table: sift_v6_group_bulk_begin keeps them as they come, and when the groups are read they are
 * the groups, at the price of the pass that reads them (a table of them costs a probe and a state block for each, and a second pass over the table to read them out). A group table that has groups
 * of its own by then takes them in (v6g_bulk_settle).
 */
#include "postgres.h"

#include "agg/gpu/v6.h"
#include "agg/v6/group/int.h"
#include "agg/v6/group/cmp.h"
#include "agg/v6/group/compact.h"

typedef struct V6Bulk
{
	bool		open;			/* sift_v6_group_fold_rec adds to it */
	uint64		n;
	uint64		cap;
	uint64		at;				/* the next record to read */
	int64	   *key;			/* spec->nkeys of each record */
	uint64	   *nulls;
	V6Acc	   *acc;			/* spec->naggs of each */
} V6Bulk;

static V6Acc
acc_of_in(const SiftV6Agg *a, const SiftV6AccIn *in)
{
	V6Acc		acc = {.n = in->n,.v0 = in->v0,.v1 = in->v1};
	double		sum;

	if (v6g_acc_op(a) != V6A_SUM_F64)
		return acc;
	memcpy(&sum, &in->v0, sizeof(sum));
	return v6fsum_of_double(sum, in->n);
}

/* ---- the group table ---- */

/* The state of the group of the key, made when it is new (the one state of a set of no keys) */
static uint8 *
rec_state(SiftV6Group *g, const int64 *key, const SiftV6StrIn *str, uint64 key_nulls)
{
	V6Set	   *set = &g->set[0];
	SiftAggSlot64 *slot;

	if (set->nk == 0)
		return (uint8 *) set->plain;
	v6g_table_only(g, set);
	sift_ht_reserve(&set->ht, 1);
	if (set->packed)
	{
		int64		x[SIFT_PACK_MAX_KEYS];

		for (int i = 0; i < set->nk; i++)
			x[i] = key[set->kidx[i]];
		g->hash[0] = sift_pack_key(&set->pack, x, key_nulls, &g->ikey[0]);
		sift_ht_upsert_batch(&set->ht, g->hash, g->ikey, 1, g->slots);
		slot = g->slots[0];
	}
	else if (set->wide)
	{
		bool		found;

		resetStringInfo(&g->kbuf);
		for (int i = 0; i < set->nk; i++)
			if (!((key_nulls >> i) & 1))
			{
				const SiftV6StrIn *s = str != NULL ? &str[set->kidx[i]] : NULL;

				if (s != NULL && s->p != NULL)
					v6g_append_key_str(g, s->p, s->len);
				else
					appendBinaryStringInfo(&g->kbuf, (const char *) &key[set->kidx[i]], sizeof(int64));
			}
		slot = sift_ht_upsert_wide(&set->ht, sift_aggk6_hash1(sift_aggk6_hash_bytes(g->kbuf.data, g->kbuf.len) ^ key_nulls), g->kbuf.data, g->kbuf.len, key_nulls, &found);
	}
	else
	{
		const int64 x = (key_nulls & 1) ? 0 : key[set->kidx[0]];

		g->ikey[0].i64[0] = x;
		g->ikey[0].i64[1] = key_nulls & 1;
		g->hash[0] = sift_aggk6_hash1((uint64) x);
		sift_ht_upsert_batch(&set->ht, g->hash, g->ikey, 1, g->slots);
		slot = g->slots[0];
	}
	return sift_ht_ext(&set->ht, slot);
}

/* ---- the bulk records ---- */

static uint64
bulk_bytes(const SiftV6Spec *spec, uint64 n)
{
	return n * (spec->nkeys * sizeof(int64) + sizeof(uint64) + spec->naggs * sizeof(V6Acc));
}

bool
sift_v6_group_bulk_begin(SiftV6Group *g, uint64 records)
{
	const SiftV6Spec *spec = g->spec;
	V6Bulk	   *b;
	MemoryContext old;

	if (g->bulk != NULL || spec->nsets != 0 || g->set[0].nk != spec->nkeys || spec->nkeys == 0 || spec->topk != 0 || g->gkeys != NULL || g->ndist != 0 || g->nfilters != 0 ||
		g->gen != NULL || g->slab != NULL || bulk_bytes(spec, records) > Min(g->budget, MaxAllocSize))
		return false;
	for (int k = 0; k < spec->nkeys; k++)
		if (sift_v6_class(spec->key[k].typid) != SV6C_I32 && sift_v6_class(spec->key[k].typid) != SV6C_I64)
			return false;
	old = MemoryContextSwitchTo(g->cx);
	g->bulk = b = palloc0(sizeof(V6Bulk));
	b->cap = records;
	/* check-memory: allow the records fit the budget of the group, which is under the largest allocation */
	b->key = palloc(Max(records, 1) * spec->nkeys * sizeof(int64));
	b->nulls = palloc(Max(records, 1) * sizeof(uint64));
	b->acc = palloc(Max(records, 1) * spec->naggs * sizeof(V6Acc));
	b->open = true;
	MemoryContextSwitchTo(old);
	return true;
}

void
sift_v6_group_bulk_end(SiftV6Group *g)
{
	if (g->bulk != NULL)
		g->bulk->open = false;
}

static void
bulk_put(SiftV6Group *g, const int64 *key, uint64 key_nulls, const SiftV6AccIn *acc)
{
	V6Bulk	   *b = g->bulk;
	const SiftV6Spec *spec = g->spec;
	V6Acc	   *d = b->acc + b->n * spec->naggs;

	Assert(b->n < b->cap);
	memcpy(b->key + b->n * spec->nkeys, key, spec->nkeys * sizeof(int64));
	b->nulls[b->n++] = key_nulls;
	for (int j = 0; j < spec->naggs; j++)
	{
		const V6Acc in = acc_of_in(&spec->agg[j], &acc[j]);

		d[j] = (V6Acc) {0};
		v6g_merge_acc(g, &spec->agg[j], &d[j], &in);
	}
}

uint64
v6g_bulk_bytes(const SiftV6Group *g)
{
	return g->bulk == NULL ? 0 : bulk_bytes(g->spec, g->bulk->cap);
}

void
v6g_bulk_free(SiftV6Group *g)
{
	if (g->bulk == NULL)
		return;
	pfree(g->bulk->key);
	pfree(g->bulk->nulls);
	pfree(g->bulk->acc);
	pfree(g->bulk);
	g->bulk = NULL;
}

/* The records into the group table */
static void
bulk_flush(SiftV6Group *g)
{
	const V6Bulk *b = g->bulk;
	const SiftV6Spec *spec = g->spec;
	MemoryContext old = MemoryContextSwitchTo(g->cx);

	for (uint64 r = 0; r < b->n; r++)
	{
		uint8	   *state = rec_state(g, b->key + r * spec->nkeys, NULL, b->nulls[r]);

		for (int j = 0; j < spec->naggs; j++)
			v6g_merge_acc(g, &spec->agg[j], (V6Acc *) (state + g->off[j]), &b->acc[r * spec->naggs + j]);
		v6g_spill_if_over(g);
	}
	MemoryContextSwitchTo(old);
	v6g_bulk_free(g);
}

void
v6g_bulk_settle(SiftV6Group *g)
{
	const V6Set *set = &g->set[0];

	if (g->bulk != NULL && (set->ht.count != 0 || (set->cmp != NULL && set->cmp->count != 0) || g->spilled))
		bulk_flush(g);
}

bool
v6g_bulk_next(SiftV6Group *g, Datum *values, bool *isnull)
{
	V6Bulk	   *b = g->bulk;
	const SiftV6Spec *spec = g->spec;
	const V6Set *set = &g->set[0];
	const int64 *key;
	const V6Acc *acc;
	uint64		nulls;

	if (b->at == b->n)
		return false;
	key = b->key + b->at * spec->nkeys;
	acc = b->acc + b->at * spec->naggs;
	nulls = b->nulls[b->at++];
	for (int i = 0; i < set->nk; i++)
	{
		const int	k = set->kidx[i];

		isnull[k] = (nulls >> i) & 1;
		values[k] = isnull[k] ? (Datum) 0 : v6g_int_datum(spec->key[k].typid, key[k]);
	}
	for (int j = 0; j < spec->naggs; j++)
		values[spec->nkeys + j] = v6g_agg_datum(g, &spec->agg[j], &acc[j], set->mask, &isnull[spec->nkeys + j]);
	return true;
}

/* ---- the records ---- */

void
sift_v6_group_fold_rec(SiftV6Group *g, const int64 *key, const SiftV6StrIn *str, uint64 key_nulls, const SiftV6AccIn *acc)
{
	MemoryContext old = MemoryContextSwitchTo(g->cx);
	uint8	   *state;

	Assert(g->spec->nsets == 0);
	g->dist_done = true;		/* a record has the count of its distinct values: its group lives in the table, not in the set of pairs, and leaves with the table when it spills */
	if (g->bulk != NULL && g->bulk->open)
	{
		bulk_put(g, key, key_nulls, acc);
		MemoryContextSwitchTo(old);
		return;
	}
	state = rec_state(g, key, str, key_nulls);
	for (int j = 0; j < g->spec->naggs; j++)
	{
		const V6Acc in = acc_of_in(&g->spec->agg[j], &acc[j]);

		if (acc[j].str != NULL)
			sift_tslot_offer(&g->tx, (SiftTextSlot *) (state + g->off[j]), g->spec->agg[j].kind == SV6_MIN, (const char *) acc[j].str, acc[j].len, NULL, NULL);
		else
			v6g_merge_acc(g, &g->spec->agg[j], (V6Acc *) (state + g->off[j]), &in);
	}
	v6g_spill_if_over(g);
	MemoryContextSwitchTo(old);
}
