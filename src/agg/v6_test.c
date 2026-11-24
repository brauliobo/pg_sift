/*
 * agg/v6_test.c - test-only SQL functions of the 2048-row engine, compiled only with SIFT_TESTING and registered by the pg_sift_test
 * extension: the salted hash table (agg/group/ht.c) against a sorted reference, with the growth and the radix scatter of a parallel
 * merge; the element mask of an array column (agg/array.c) against a per element loop; the cost of a path (agg/v6/v6_cost_model.c).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "funcapi.h"

#include "agg/v6/v6_cost_model.h"
#include "agg/group/ht.h"
#include "utils/builtins.h"

PG_FUNCTION_INFO_V1(sift_test_salted_ht);
PG_FUNCTION_INFO_V1(sift_test_agg_cost);

static uint64 rng_state;

static uint64
rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state * UINT64CONST(0x2545F4914F6CDD1D);
}

typedef struct Row
{
	uint64		k;
	int64		v;
} Row;

static int
row_cmp(const void *a, const void *b)
{
	const Row  *x = a,
			   *y = b;

	return x->k < y->k ? -1 : x->k > y->k;
}

static SiftInlineKey16
key_of(uint64 k)
{
	SiftInlineKey16 key;

	key.u64[0] = k;
	key.u64[1] = k * 7 + 1;
	return key;
}

/* The groups of rows[0..n) (sorted by key) as count / sum / min / max in out, one per distinct key; returns their number */
static uint32
reference_groups(const Row *rows, uint32 n, SiftAggSlot64 *out)
{
	uint32		g = 0;

	for (uint32 i = 0; i < n; i++)
	{
		if (i == 0 || rows[i].k != rows[i - 1].k)
		{
			memset(&out[g], 0, sizeof(out[g]));
			out[g].key = key_of(rows[i].k);
			sift_slot_cmsm_init(&out[g]);
			g++;
		}
		sift_slot_cmsm_add(&out[g - 1], rows[i].v);
	}
	return g;
}

static bool
same_agg(const SiftAggSlot64 *a, const SiftAggSlot64 *b)
{
	return !memcmp(a->agg.raw, b->agg.raw, 32);
}

static uint32
find_ref(const SiftAggSlot64 *ref, uint32 n, const SiftInlineKey16 *key)
{
	uint32		lo = 0,
				hi = n;

	while (lo < hi)
	{
		const uint32 mid = (lo + hi) / 2;

		if (ref[mid].key.u64[0] < key->u64[0])
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < n && ref[lo].key.u64[0] == key->u64[0] ? lo : n;
}

/* Every group of ht is in ref with the same aggregates, and ht has as many groups as ref */
static bool
ht_matches(const SiftSaltedHT *ht, const SiftAggSlot64 *ref, uint32 nref)
{
	SiftAggSlot64 *s;
	uint32		seen = 0;

	if (ht->count != nref)
		return false;
	SIFT_HT_FOREACH(ht, s)
	{
		const uint32 at = find_ref(ref, nref, &s->key);

		if (at == nref || !same_agg(s, &ref[at]) || s->hash64 != sift_aggk6_hash1(s->key.u64[0]))
			return false;
		seen++;
	}
	return seen == nref;
}

/* One case: n rows over a pool of distinct keys, aggregated one by one or in batches, then scattered and merged as two workers and the radix partitions would */
static bool
ht_case(void)
{
	const uint32 n = 1 + rnd() % 30000,
				pool = 1 + rnd() % (rnd() % 4 == 0 ? 3 : n);
	Row		   *rows = palloc(sizeof(Row) * n);
	SiftAggSlot64 *ref = palloc_aligned(sizeof(SiftAggSlot64) * n, _Alignof(SiftAggSlot64), 0);
	SiftSaltedHT ht[3];
	uint32		nref;
	bool		ok = true;
	const bool	batch = rnd() & 1;

	for (uint32 i = 0; i < n; i++)
	{
		rows[i].k = (rnd() % pool) * UINT64CONST(0x9E3779B97F4A7C15) + 1;
		rows[i].v = (int64) rnd() >> (rnd() % 64);
	}
	{
		Row		   *sorted = palloc(sizeof(Row) * n);

		memcpy(sorted, rows, sizeof(Row) * n);
		qsort(sorted, n, sizeof(Row), row_cmp);
		nref = reference_groups(sorted, n, ref);
		pfree(sorted);
	}

	/* two workers over the halves of the rows, each a small table that grows */
	for (int w = 0; w < 2; w++)
	{
		const uint32 lo = w ? n / 2 : 0,
					hi = w ? n : n / 2;

		sift_ht_init(&ht[w], 1);
		if (batch)
		{
			uint64		hashes[SIFT_VEC_LEN];
			SiftInlineKey16 keys[SIFT_VEC_LEN];
			SiftAggSlot64 *slots[SIFT_VEC_LEN];

			for (uint32 base = lo; base < hi; base += SIFT_VEC_LEN)
			{
				const uint32 m = Min(SIFT_VEC_LEN, hi - base);

				for (uint32 i = 0; i < m; i++)
				{
					keys[i] = key_of(rows[base + i].k);
					hashes[i] = sift_aggk6_hash1(rows[base + i].k);
				}
				sift_ht_upsert_batch(&ht[w], hashes, keys, m, slots);
				for (uint32 i = 0; i < m; i++)
				{
					if (slots[i]->agg.i64[0] == 0)
						sift_slot_cmsm_init(slots[i]);
					sift_slot_cmsm_add(slots[i], rows[base + i].v);
				}
			}
		}
		else
			for (uint32 i = lo; i < hi; i++)
			{
				const SiftInlineKey16 key = key_of(rows[i].k);
				bool		found;
				SiftAggSlot64 *s = sift_ht_upsert(&ht[w], sift_aggk6_hash1(rows[i].k), &key, &found);

				if (!found)
					sift_slot_cmsm_init(s);
				sift_slot_cmsm_add(s, rows[i].v);
			}
	}

	/* the merge: the groups of both workers into one table per radix partition, by the stored hash */
	{
		const uint32 parts = sift_radix_parts(1 + rnd() % 16),
					bits = __builtin_ctz(parts);
		SiftSaltedHT *pt = palloc(sizeof(SiftSaltedHT) * parts);
		uint32		groups = 0;
		SiftAggSlot64 *s;

		for (uint32 p = 0; p < parts; p++)
			sift_ht_init(&pt[p], 1);
		for (int w = 0; w < 2; w++)
			SIFT_HT_FOREACH(&ht[w], s)
			{
				const uint32 p = SIFT_HASH_RADIX(s->hash64, bits);
				bool		found;
				SiftAggSlot64 *d = sift_ht_upsert(&pt[p], s->hash64, &s->key, &found);

				if (!found)
					sift_slot_cmsm_init(d);
				sift_slot_cmsm_merge(d, s);
			}
		for (uint32 p = 0; p < parts; p++)
		{
			SIFT_HT_FOREACH(&pt[p], s)
				ok = ok && SIFT_HASH_RADIX(s->hash64, bits) == p && same_agg(s, &ref[find_ref(ref, nref, &s->key)]);
			groups += pt[p].count;
			sift_ht_destroy(&pt[p]);
		}
		ok = ok && groups == nref;
		pfree(pt);
	}
	for (int w = 0; w < 2; w++)
		sift_ht_destroy(&ht[w]);

	/* a single table of every row, then cleared and reused */
	sift_ht_init(&ht[2], 1);
	for (uint32 i = 0; i < n; i++)
	{
		const SiftInlineKey16 key = key_of(rows[i].k);
		bool		found;
		SiftAggSlot64 *s = sift_ht_upsert(&ht[2], sift_aggk6_hash1(rows[i].k), &key, &found);

		if (!found)
			sift_slot_cmsm_init(s);
		sift_slot_cmsm_add(s, rows[i].v);
	}
	ok = ok && ht_matches(&ht[2], ref, nref);
	sift_ht_clear(&ht[2]);
	ok = ok && ht[2].count == 0;
	sift_ht_destroy(&ht[2]);
	pfree(rows);
	pfree(ref);
	return ok;
}

/*
 * The compare of 16 control bytes at every level against the scalar loop, with bytes drawn from a few values so that matches are common: the
 * 16-bit mask of sift_ht_match16, and the lanes the probe loops walk in the mask of sift_ht_match16_fast
 */
static bool
match16_case(void)
{
	uint8		c[SIFT_HT_GROUP_WIDTH + 1];
	const uint8 vals[4] = {0, 0x80, 0x9C, 0xFF};
	const uint8 b = vals[rnd() % 4];
	const uint8 *p = c + rnd() % 2;
	SiftHtMask	m;
	uint32		walked = 0;

	for (int i = 0; i < SIFT_HT_GROUP_WIDTH + 1; i++)
		c[i] = vals[rnd() % 4];
	for (m = sift_ht_match16_fast(p, b); m; SIFT_HT_MASK_CLEAR(m))
		walked |= (uint32) 1 << SIFT_HT_MASK_NEXT(m);
	return sift_ht_match16(p, b) == sift_ht_match16_scalar(p, b) && walked == sift_ht_match16_scalar(p, b);
}

/* Number of failed cases */
Datum
sift_test_salted_ht(PG_FUNCTION_ARGS)
{
	int64		bad = 0;

	rng_state = PG_GETARG_INT64(0) * UINT64CONST(0x9E3779B97F4A7C15) | 1;
	for (int i = 0; i < PG_GETARG_INT32(1); i++)
	{
		CHECK_FOR_INTERRUPTS();
		bad += !ht_case();
		for (int j = 0; j < 64; j++)
			bad += !match16_case();
	}
	PG_RETURN_INT64(bad);
}

Datum
sift_test_agg_cost(PG_FUNCTION_ARGS)
{
	const SiftCostInputs in = {PG_GETARG_FLOAT8(0), PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(1), PG_GETARG_FLOAT8(2), PG_GETARG_FLOAT8(3), SIFT_COST_R_BW_CPU, 0.25};
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	Cost		startup,
				total;
	int			w;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	w = sift_agg_choose_workers(&in, PG_GETARG_FLOAT8(4), PG_GETARG_BOOL(5), PG_GETARG_BOOL(6), 0.0, PG_GETARG_INT32(7), &startup, &total);
	values[0] = Int32GetDatum(w);
	values[1] = Float8GetDatum(startup);
	values[2] = Float8GetDatum(total);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

#endif							/* SIFT_TESTING */
