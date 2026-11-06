/*
 * agg/intids_test.c - test-only SQL function sift._test_intids, compiled only with SIFT_TESTING and registered by the pg_sift_test extension:
 * seeded key sequences of many shapes (ascending, descending, dense and sparse in random order, clusters, walks, strides, the ends of int64,
 * a dense range that turns sparse and the reverse) are looked up in an IntIds table and in a plain hash of another make; every id must agree.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/htup_details.h"
#include "common/int.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"

#include "agg/intids.h"
#include "core/testrng.h"

PG_FUNCTION_INFO_V1(sift_test_intids);

typedef struct RefMap
{
	uint32		nslot;
	uint32		n;
	int64	   *key;
	uint32	   *id;
} RefMap;

static uint32
ref_id(RefMap *m, int64 k, uint32 next)
{
	uint64		x = (uint64) k;
	uint32		h;

	x = (x ^ (x >> 33)) * UINT64CONST(0xFF51AFD7ED558CCD);
	x = (x ^ (x >> 33)) * UINT64CONST(0xC4CEB9FE1A85EC53);
	for (h = (uint32) (x ^ (x >> 33)) & (m->nslot - 1); m->id[h]; h = (h + 1) & (m->nslot - 1))
		if (m->key[h] == k)
			return m->id[h];
	m->key[h] = k;
	m->id[h] = next;
	m->n++;
	return next;
}

enum
{
	PAT_ASC, PAT_DESC, PAT_DENSE, PAT_SPARSE, PAT_CLUSTERS, PAT_WALK, PAT_STRIDE, PAT_DENSE_THEN_SPARSE, PAT_SPARSE_THEN_DENSE, PAT_N
};

static const int64 bases[] = {0, 1, -1, -70000, 4000000000, PG_INT64_MAX - 40000, PG_INT64_MIN + 40000, PG_INT64_MAX - 3, PG_INT64_MIN, 123456789012345};

static int64
sat_add(int64 b, int64 d)
{
	int64		r;

	return pg_add_s64_overflow(b, d, &r) ? (d < 0 ? PG_INT64_MIN : PG_INT64_MAX) : r;
}

/* the key of lookup i of m in pattern pat around base; walk is the position of the walk and the strides the cluster state */
static int64
pat_key(SiftRng *r, int pat, int64 base, int i, int m, int64 *walk, int64 *clu, int stride)
{
	switch (pat)
	{
		case PAT_ASC:
			return sat_add(base, i);
		case PAT_DESC:
			return sat_add(base, -(int64) i);
		case PAT_DENSE:
			return sat_add(base, (int64) sift_rng_below(r, Max(m / 2, 1) + 1));
		case PAT_SPARSE:
			return (int64) sift_rng_next(r);
		case PAT_CLUSTERS:
			return sat_add(clu[sift_rng_below(r, 4)], (int64) sift_rng_below(r, 500));
		case PAT_WALK:
			*walk = sat_add(*walk, (int64) sift_rng_below(r, 7) - 3);
			return *walk;
		case PAT_STRIDE:
			return sat_add(base, (int64) i * stride);
		case PAT_DENSE_THEN_SPARSE:
			return i < m / 2 ? sat_add(base, (int64) sift_rng_below(r, m)) : (int64) sift_rng_next(r);
		default:
			return i < m / 2 ? (int64) sift_rng_next(r) : sat_add(base, (int64) sift_rng_below(r, m));
	}
}

/* sift._test_intids(seed, cases): bad = lookups that disagree with the reference; paged / hashed = cases that ended in each mode; up / down = switches from the hash to pages and back */
Datum
sift_test_intids(PG_FUNCTION_ARGS)
{
	SiftRng		r;
	int64		bad = 0,
				paged = 0,
				hashed = 0,
				up = 0,
				down = 0;
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5] = {false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	sift_rng_seed(&r, PG_GETARG_INT64(0));
	for (int c = 0; c < PG_GETARG_INT32(1); c++)
	{
		IntIds		x;
		RefMap		ref;
		int			pat = (int) sift_rng_below(&r, PAT_N),
					m = 200 + (int) sift_rng_below(&r, 30000),
					stride = 1 + (int) sift_rng_below(&r, 3000);
		int64		base = bases[sift_rng_below(&r, lengthof(bases))],
					walk = base,
					clu[4];
		int64	   *hist = palloc(m * sizeof(int64));
		uint32		next = 1;

		for (int i = 0; i < 4; i++)
			clu[i] = (int64) sift_rng_next(&r);
		ref.nslot = 4;
		while (ref.nslot < 2 * (uint32) m)
			ref.nslot *= 2;
		ref.n = 0;
		ref.key = palloc(ref.nslot * sizeof(int64));
		ref.id = palloc0(ref.nslot * sizeof(uint32));
		intids_init(&x);
		/* a small free span makes the density rule decide from the start, a large one the pages */
		x.free_span = (uint32) 1 << sift_rng_below(&r, 21);
		for (int i = 0; i < m; i++)
		{
			bool		was_paged = x.pg != NULL;
			int64		k = i > 0 && sift_rng_below(&r, 4) == 0 ? hist[sift_rng_below(&r, i)] :
				pat_key(&r, pat, base, i, m, &walk, clu, stride);
			uint32		id = intids_id(&x, k, next),
						want = ref_id(&ref, k, next);

			hist[i] = k;
			bad += id != want;
			if (id == next)
				next++;
			up += !was_paged && x.pg != NULL;
			down += was_paged && x.pg == NULL;
			CHECK_FOR_INTERRUPTS();
		}
		/* every key again, in the order it came: the same ids, and no key new */
		for (int i = 0; i < m; i++)
			bad += intids_id(&x, hist[i], next) != ref_id(&ref, hist[i], next);
		bad += x.n != ref.n || next != ref.n + 1;
		paged += x.pg != NULL;
		hashed += x.pg == NULL;
		intids_free(&x);
		pfree(hist);
		pfree(ref.key);
		pfree(ref.id);
	}
	values[0] = Int64GetDatum(bad);
	values[1] = Int64GetDatum(paged);
	values[2] = Int64GetDatum(hashed);
	values[3] = Int64GetDatum(up);
	values[4] = Int64GetDatum(down);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

#endif							/* SIFT_TESTING */
