/*
 * agg/sip_test.c - test-only SQL functions of the runtime join filter (agg/sip.h), compiled only with SIFT_TESTING and registered by the pg_sift_test
 * extension: sift._test_sip builds filters of many shapes (no key, one, a handful, past the exact keys, tens of thousands; int4 and int8; too small a size
 * for the keys; one builder or four merged) and holds every answer to a sorted array of the keys: a key that was inserted passes at every level, a NULL
 * never does, the exact filter has no false positive and the Bloom one stays near its measured rate. sift._test_sip_filter is the filter as a join
 * semi-filter over two arrays, for the SQL test to compare with PostgreSQL's own join.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "agg/sip.h"
#include "core/testrng.h"

PG_FUNCTION_INFO_V1(sift_test_sip);
PG_FUNCTION_INFO_V1(sift_test_sip_filter);

static SiftRng rng;

static uint64
rnd(void)
{
	return sift_rng_next(&rng);
}

static int
i64_cmp(const void *a, const void *b)
{
	const int64 x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

/* is there a key of the sorted array in [lo, hi] */
static bool
has_key_in(const int64 *keys, int n, int64 lo, int64 hi)
{
	int			a = 0,
				b = n;

	while (a < b)
	{
		const int	mid = (a + b) / 2;

		if (keys[mid] < lo)
			a = mid + 1;
		else
			b = mid;
	}
	return a < n && keys[a] <= hi;
}

typedef struct Batch
{
	SiftVec		v;
	int64	   *k64;
	int32	   *k32;
	uint64		valid[SIFT_VEC_WORDS];
	uint64		mask[SIFT_VEC_WORDS];
} Batch;

static void
batch_init(Batch *b, SiftVecType type, uint16 n)
{
	memset(b, 0, sizeof(*b));
	b->k64 = palloc_aligned(SIFT_VEC_LEN * 8 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	b->k32 = palloc_aligned(SIFT_VEC_LEN * 4 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	b->v.data = type == SVEC_I32 ? (void *) b->k32 : (void *) b->k64;
	b->v.valid = b->valid;
	b->v.len = n;
	b->v.dense = true;
	b->v.type = type;
}

static void
batch_free(Batch *b)
{
	pfree(b->k64);
	pfree(b->k32);
}

static int
count_bits(const uint64 *m, int n)
{
	int			c = 0;

	for (int i = 0; i < n; i++)
		c += (int) ((m[i >> 6] >> (i & 63)) & 1);
	return c;
}

static int
one_case(uint64 seed)
{
	static const int ndvs[] = {0, 1, 3, 64, 65, 300, 5000, 60000};
	const int	ndv = ndvs[rnd() % lengthof(ndvs)];
	const SiftVecType type = rnd() % 2 ? SVEC_I32 : SVEC_I64;
	const int64 base = type == SVEC_I32 ? (int64) (rnd() % 1000) - 500 : (int64) rnd() % 1000000 - 500000;
	const int64 span = rnd() % 2 ? Max(ndv, 1) : (int64) Max(ndv, 1) * 997;
	const uint64 est = rnd() % 4 ? ndv : ndv / 8;
	const uint64 cap = rnd() % 5 ? sift_sip_budget_bytes() : (rnd() % 2 ? 1024 : 4096);
	SiftSipFilter *sip = sift_sip_create(CurrentMemoryContext, type, est, cap),
			   *parts[4];
	int64	   *keys = palloc(Max(ndv, 1) * sizeof(int64)),
			   *uniq = palloc(Max(ndv, 1) * sizeof(int64));
	int			nuniq = 0,
				bad = 0;
	Batch		b;
	SiftSipStats st = {0};
	uint32		flags;

	for (int i = 0; i < 4; i++)
		parts[i] = sift_sip_create(CurrentMemoryContext, type, est, cap);
	for (int i = 0; i < ndv; i++)
	{
		keys[i] = base + (int64) (rnd() % span);
		sift_sip_insert_i64(sip, keys[i]);
		sift_sip_insert_i64(parts[i % 4], keys[i]);
	}
	memcpy(uniq, keys, ndv * sizeof(int64));
	qsort(uniq, ndv, sizeof(int64), i64_cmp);
	for (int i = 0; i < ndv; i++)
		if (i == 0 || uniq[i] != uniq[i - 1])
			uniq[nuniq++] = uniq[i];

	/* a filter that is not finalized passes everything */
	batch_init(&b, type, 64);
	for (int i = 0; i < 64; i++)
		b.k64[i] = b.k32[i] = (int32) i;
	memset(b.valid, 0xFF, sizeof(b.valid));
	b.v.all_valid = false;
	memset(b.mask, 0xFF, sizeof(b.mask));
	bad += sift_sip_probe_vec(sip, &b.v, b.mask, NULL) != 64;
	bad += !sift_sip_probe_granule(sip, 0, 10, NULL);

	sift_sip_finalize(sip);
	for (int i = 0; i < 4; i++)
		sift_sip_finalize(parts[i]);
	flags = sift_sip_flags(sip);
	bad += !(flags & SIFT_SIP_FLAG_READY);
	bad += ((flags & SIFT_SIP_FLAG_EMPTY) != 0) != (ndv == 0);
	bad += ndv > 0 && ((flags & SIFT_SIP_FLAG_HAS_EXACT) != 0) != (nuniq <= SIFT_SIP_EXACT_MAX);
	bad += ndv > 0 && (!(flags & SIFT_SIP_FLAG_HAS_RANGE) || sip->min_key != uniq[0] || sip->max_key != uniq[nuniq - 1]);
	bad += sift_sip_is_empty(sip) != (ndv == 0);
	bad += sift_sip_fpr(sip) > SIFT_SIP_MAX_FPR && (flags & SIFT_SIP_FLAG_HAS_BLOOM);

	/* four builders merged: the Bloom bits, the range and the count of one builder */
	{
		SiftSipFilter *m = sift_sip_create(CurrentMemoryContext, type, est, cap);

		for (int i = 0; i < 4; i++)
			sift_sip_merge_atomic(m, parts[i]);
		sift_sip_finalize(m);
		bad += memcmp(sift_sip_blocks(m), sift_sip_blocks(sip), (size_t) sip->n_blocks * SIFT_SBBF_BLOCK_BYTES) != 0;
		bad += m->n_inserted != sip->n_inserted || m->min_key != sip->min_key || m->max_key != sip->max_key;
		bad += (sift_sip_flags(m) & SIFT_SIP_FLAG_HAS_EXACT) != 0;
		sift_sip_free(m);
	}

	/* zones: a zone with a key in it is never pruned, and an exact filter prunes every zone without one */
	for (int z = 0; z < 400; z++)
	{
		const int64 lo = base + (int64) (rnd() % (span + 40)) - 20,
					hi = lo + (rnd() % 3 ? (int64) (rnd() % 8) : (int64) (rnd() % (span + 1)));
		const bool	present = has_key_in(uniq, nuniq, lo, hi),
					pass = sift_sip_probe_granule(sip, lo, hi, &st);

		bad += present && !pass;
		bad += (flags & SIFT_SIP_FLAG_EMPTY) && pass;
		bad += (flags & SIFT_SIP_FLAG_HAS_EXACT) && present != pass;
	}
	bad += st.granules_probed != 400;

	/* rows: the present keys pass, the NULLs and the rows outside the mask do not */
	{
		const int	n = 1 + (int) (rnd() % SIFT_VEC_LEN);
		int			absent = 0,
					fp = 0;

		batch_free(&b);
		batch_init(&b, type, (uint16) n);
		memset(b.mask, 0, sizeof(b.mask));
		for (int i = 0; i < n; i++)
		{
			const bool	hit = nuniq > 0 && rnd() % 2;
			const int64 x = hit ? uniq[rnd() % nuniq] : base + (int64) (rnd() % (span * 3 + 7)) - span;

			b.k64[i] = x;
			b.k32[i] = (int32) x;
			if (rnd() % 8)
				b.valid[i >> 6] |= UINT64CONST(1) << (i & 63);
			if (rnd() % 8)
				b.mask[i >> 6] |= UINT64CONST(1) << (i & 63);
		}
		{
			uint64		in[SIFT_VEC_WORDS];
			uint16		left;

			memcpy(in, b.mask, sizeof(in));
			left = sift_sip_probe_vec(sip, &b.v, b.mask, NULL);
			bad += left != count_bits(b.mask, n);
			for (int i = 0; i < n; i++)
			{
				const bool	was = (in[i >> 6] >> (i & 63)) & 1,
							valid = (b.valid[i >> 6] >> (i & 63)) & 1,
							now = (b.mask[i >> 6] >> (i & 63)) & 1,
							key_in = has_key_in(uniq, nuniq, type == SVEC_I32 ? b.k32[i] : b.k64[i], type == SVEC_I32 ? b.k32[i] : b.k64[i]);

				bad += now && !was;
				bad += now && !valid;
				bad += was && valid && key_in && !now;
				if (was && valid && !key_in)
				{
					absent++;
					fp += now;
				}
			}
			bad += (flags & SIFT_SIP_FLAG_EMPTY) && left != 0;
			bad += (flags & SIFT_SIP_FLAG_HAS_EXACT) && fp != 0;
			bad += fp > absent * (sift_sip_fpr(sip) * 2 + 0.05) + 8 && (flags & SIFT_SIP_FLAG_HAS_BLOOM);
		}
	}
	batch_free(&b);
	sift_sip_free(sip);
	for (int i = 0; i < 4; i++)
		sift_sip_free(parts[i]);
	pfree(keys);
	pfree(uniq);
	(void) seed;
	return bad;
}

/* a filter that took no key prunes every hash, and its blocks live in the context it was created in */
static int
empty_hash_case(void)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sip empty", ALLOCSET_SMALL_SIZES);
	SiftSipFilter *sip = sift_sip_create(cxt, SVEC_I64, 100, sift_sip_budget_bytes());
	uint64		hashes[SIFT_VEC_LEN],
				mask[SIFT_VEC_WORDS];
	int			bad = 0;

	bad += GetMemoryChunkContext(sip) != cxt;
	for (int i = 0; i < SIFT_VEC_LEN; i++)
		hashes[i] = sift_aggk6_hash1((uint64) i);
	memset(mask, 0xFF, sizeof(mask));
	bad += sift_sip_probe_hashes(sip, hashes, mask, SIFT_VEC_LEN, NULL) != SIFT_VEC_LEN;
	sift_sip_finalize(sip);
	memset(mask, 0xFF, sizeof(mask));
	bad += sift_sip_probe_hashes(sip, hashes, mask, SIFT_VEC_LEN, NULL) != 0;
	bad += mask[0] != 0;
	MemoryContextDelete(cxt);
	return bad;
}

Datum
sift_test_sip(PG_FUNCTION_ARGS)
{
	int64		bad = empty_hash_case();

	sift_aggkern6_init();
	sift_rng_seed(&rng, PG_GETARG_INT64(0));
	for (int i = 0; i < PG_GETARG_INT32(1); i++)
	{
		CHECK_FOR_INTERRUPTS();
		bad += one_case(i) != 0;
	}
	PG_RETURN_INT64(bad);
}

/* sift._test_sip_filter(fact int8[], dim int8[], as_int4 bool) RETURNS TABLE(ord int4, pass bool): the rows of fact whose key may join the keys of dim */
Datum
sift_test_sip_filter(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	ArrayType  *fact = PG_GETARG_ARRAYTYPE_P(0),
			   *dim = PG_GETARG_ARRAYTYPE_P(1);
	const SiftVecType type = PG_GETARG_BOOL(2) ? SVEC_I32 : SVEC_I64;
	Datum	   *fd,
			   *dd;
	bool	   *fn,
			   *dn;
	int			nf,
				nd;
	SiftSipFilter *sip;
	Batch		b;

	sift_aggkern6_init();
	InitMaterializedSRF(fcinfo, 0);
	deconstruct_array(fact, INT8OID, 8, true, 'd', &fd, &fn, &nf);
	deconstruct_array(dim, INT8OID, 8, true, 'd', &dd, &dn, &nd);
	sip = sift_sip_create(CurrentMemoryContext, type, nd, sift_sip_budget_bytes());
	for (int i = 0; i < nd; i++)
		if (!dn[i])
			sift_sip_insert_i64(sip, DatumGetInt64(dd[i]));
	sift_sip_finalize(sip);
	for (int from = 0; from < nf; from += SIFT_VEC_LEN)
	{
		const int	n = Min(SIFT_VEC_LEN, nf - from);

		batch_init(&b, type, (uint16) n);
		memset(b.mask, 0, sizeof(b.mask));
		for (int i = 0; i < n; i++)
		{
			b.k64[i] = DatumGetInt64(fd[from + i]);
			b.k32[i] = (int32) b.k64[i];
			b.valid[i >> 6] |= (uint64) !fn[from + i] << (i & 63);
			b.mask[i >> 6] |= UINT64CONST(1) << (i & 63);
		}
		sift_sip_probe_vec(sip, &b.v, b.mask, NULL);
		for (int i = 0; i < n; i++)
		{
			Datum		row[2] = {Int32GetDatum(from + i + 1), BoolGetDatum((b.mask[i >> 6] >> (i & 63)) & 1)};
			bool		nulls[2] = {false, false};

			tuplestore_putvalues(rsi->setResult, rsi->setDesc, row, nulls);
		}
		batch_free(&b);
	}
	sift_sip_free(sip);
	return (Datum) 0;
}

#endif							/* SIFT_TESTING */
