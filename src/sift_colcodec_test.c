/*
 * sift_colcodec_test.c - test-only SQL functions sift._test_cc_*, compiled only with SIFT_TESTING and registered by the pg_sift_test
 * extension. The standalone harness (tools/colcodec) is the exhaustive check; these run the codec inside the backend: through the
 * sift.simd dispatch, with the build's LZ4 and zstd, on bytea blobs and the backend's error reporting.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "catalog/pg_type.h"
#include "common/pg_prng.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "sift_colcodec.h"
#include "sift_colz.h"

PG_FUNCTION_INFO_V1(sift_test_cc_encode);
PG_FUNCTION_INFO_V1(sift_test_cc_decode);
PG_FUNCTION_INFO_V1(sift_test_cc_info);
PG_FUNCTION_INFO_V1(sift_test_cc_filter);
PG_FUNCTION_INFO_V1(sift_test_cc_get);
PG_FUNCTION_INFO_V1(sift_test_cc_has);
PG_FUNCTION_INFO_V1(sift_test_cc_key);
PG_FUNCTION_INFO_V1(sift_test_cc_unkey);
PG_FUNCTION_INFO_V1(sift_test_cc_digest);
PG_FUNCTION_INFO_V1(sift_test_cc_equiv);
PG_FUNCTION_INFO_V1(sift_test_cc_corrupt);

/* ---------- SQL level ---------- */

static SiftCcPolicy
policy_of(text *spec)
{
	SiftCcPolicy pol;
	const char *err = sift_cc_policy_for_column(text_to_cstring(spec), "v", &pol);

	if (err)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("%s", err)));
	return pol;
}

/* The blob in palloc'd (8-aligned) memory: a bytea's data is not */
static SiftCc *
open_blob(bytea *b, bool verify)
{
	Size		len = VARSIZE_ANY_EXHDR(b);
	uint8	   *p = palloc(len);

	memcpy(p, VARDATA_ANY(b), len);
	return sift_cc_open(CurrentMemoryContext, p, len, verify);
}

static ArrayType *
int8_array(const int64 *v, uint32 n)
{
	Datum	   *d = palloc(sizeof(Datum) * (n + 1));

	for (uint32 i = 0; i < n; i++)
		d[i] = Int64GetDatum(v[i]);
	return construct_array(d, (int) n, INT8OID, sizeof(int64), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);
}

/* sift._test_cc_encode(keys int8[], kind int4, policy text): NULL elements are NULL slots */
Datum
sift_test_cc_encode(PG_FUNCTION_ARGS)
{
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(0);
	SiftCcPolicy pol = policy_of(PG_GETARG_TEXT_PP(2));
	Datum	   *d;
	bool	   *isnull;
	int			n;
	int64	   *keys;
	uint64	   *nulls;
	StringInfoData out;
	bytea	   *r;

	deconstruct_array(a, INT8OID, sizeof(int64), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE, &d, &isnull, &n);
	keys = palloc0(sizeof(int64) * (n + 1));
	nulls = palloc0(sizeof(uint64) * ((n + 63) / 64 + 1));
	for (int i = 0; i < n; i++)
	{
		if (isnull[i])
			nulls[i / 64] |= UINT64CONST(1) << (i % 64);
		else
			keys[i] = DatumGetInt64(d[i]);
	}
	initStringInfo(&out);
	sift_cc_encode(&out, (SiftCcKind) PG_GETARG_INT32(1), keys, (uint32) n, nulls, &pol, NULL);
	r = palloc(VARHDRSZ + out.len);
	SET_VARSIZE(r, VARHDRSZ + out.len);
	memcpy(VARDATA(r), out.data, out.len);
	PG_RETURN_BYTEA_P(r);
}

/* sift._test_cc_decode(blob bytea, verify bool): the keys (NULL slots hold the filler); verify = false skips the check word */
Datum
sift_test_cc_decode(PG_FUNCTION_ARGS)
{
	SiftCc	   *cc = open_blob(PG_GETARG_BYTEA_PP(0), PG_GETARG_BOOL(1));
	int64	   *v = palloc(sizeof(int64) * (sift_cc_nrows(cc) + SIFT_COL_PAD / 8));

	sift_cc_decode(cc, v);
	PG_RETURN_ARRAYTYPE_P(int8_array(v, sift_cc_nrows(cc)));
}

/* sift._test_cc_info(blob bytea, OUT kind, OUT n, OUT min, OUT max, OUT tag, OUT bytes) */
Datum
sift_test_cc_info(PG_FUNCTION_ARGS)
{
	bytea	   *b = PG_GETARG_BYTEA_PP(0);
	SiftCc	   *cc = open_blob(b, true);
	TupleDesc	td;
	Datum		v[6];
	bool		nul[6] = {false};

	if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	v[0] = Int32GetDatum(sift_cc_kind(cc));
	v[1] = Int32GetDatum((int32) sift_cc_nrows(cc));
	v[2] = Int64GetDatum(sift_cc_min(cc));
	v[3] = Int64GetDatum(sift_cc_max(cc));
	v[4] = Int32GetDatum(sift_cc_tag(cc));
	v[5] = Int32GetDatum((int32) VARSIZE_ANY_EXHDR(b));
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(td, v, nul)));
}

/* sift._test_cc_filter(blob bytea, lo int8, hi int8, OUT rows int4[], OUT sum int8): the rows with a key in [lo, hi], and their sum */
Datum
sift_test_cc_filter(PG_FUNCTION_ARGS)
{
	SiftCc	   *cc = open_blob(PG_GETARG_BYTEA_PP(0), true);
	uint32		n = sift_cc_nrows(cc);
	uint64	   *bits = palloc0(sizeof(uint64) * SIFT_COL_NWORDS(n));
	Datum	   *rows = palloc(sizeof(Datum) * (n + 1));
	int			nr = 0;
	int64		sum;
	TupleDesc	td;
	Datum		v[2];
	bool		nul[2] = {false};

	if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	sift_cc_filter(cc, PG_GETARG_INT64(1), PG_GETARG_INT64(2), bits);
	sift_cc_sum(cc, bits, &sum);
	for (uint32 i = 0; i < n; i++)
		if ((bits[i / 64] >> (i % 64)) & 1)
			rows[nr++] = Int32GetDatum((int32) i);
	v[0] = PointerGetDatum(construct_array_builtin(rows, nr, INT4OID));
	v[1] = Int64GetDatum(sum);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(td, v, nul)));
}

Datum
sift_test_cc_get(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(sift_cc_get(open_blob(PG_GETARG_BYTEA_PP(0), true), (uint32) PG_GETARG_INT32(1)));
}

/* sift._test_cc_has(alg text): whether this build has LZ4 ('lz4') or zstd ('zstd') */
Datum
sift_test_cc_has(PG_FUNCTION_ARGS)
{
	char	   *alg = text_to_cstring(PG_GETARG_TEXT_PP(0));

	PG_RETURN_BOOL(sift_colz_available(!strcmp(alg, "lz4") ? SIFT_CC_ALG_LZ4 : SIFT_CC_ALG_ZSTD));
}

/* The key of a float8 (and back): what the codec stores for float columns */
Datum
sift_test_cc_key(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(sift_col_f8_key(PG_GETARG_FLOAT8(0)));
}

Datum
sift_test_cc_unkey(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(sift_col_f8_from_key(PG_GETARG_INT64(0)));
}

/* ---------- random chunks ---------- */

typedef pg_prng_state CcgRng;

static inline uint64
ccg_next(CcgRng *r)
{
	return pg_prng_uint64(r);
}

#include "sift_colcodec_gen.h"

typedef struct Chunk
{
	SiftCcKind	kind;
	uint32		n;
	int64	   *v;
	uint64	   *nulls;
} Chunk;

static const char *const policies[] = {"auto", "none", "lz4", "zstd:5", "for", "delta", "rle", "dict", "alp", "rle+dict+zstd"};

#define NPOL ((int) lengthof(policies))

static void
gen_chunk(pg_prng_state *rs, Chunk *c)
{
	static const uint32 sizes[] = {0, 1, 2, 31, 32, 33, 1000, 1024, 1025, 2100, 5000};

	c->n = sizes[pg_prng_uint32(rs) % lengthof(sizes)];
	c->v = palloc0(sizeof(int64) * (c->n + 1));
	ccg_fill(rs, pg_prng_uint32(rs) % CCG_NGEN, c->n, c->v, &c->kind);
	c->nulls = palloc0(sizeof(uint64) * (c->n / 64 + 1));
	if (c->n && pg_prng_uint32(rs) % 2)
		for (uint32 i = 0; i < c->n; i++)
			if (pg_prng_uint32(rs) % 11 == 0)
				c->nulls[i / 64] |= UINT64CONST(1) << (i % 64);
}

/* the policy of the list, or false when this build lacks its library */
static bool
policy_at(int p, SiftCcPolicy *pol)
{
	sift_cc_policy_parse(policies[p], pol);
	return pol->mode != SIFT_CCM_LAYER || sift_colz_available(pol->alg);
}

static bool
is_null(const Chunk *c, uint32 i)
{
	return (c->nulls[i / 64] >> (i % 64)) & 1;
}

/*
 * Everything the codec does with the chunk under the current kernels folded into a hash: the written bytes (without a layer, whose bytes
 * depend on the library version), the decode, filters, sums and values at rows. *bad counts what disagrees with the plain values.
 */
static uint64
chunk_digest(const Chunk *c, const SiftCcPolicy *pol, uint64 *bad)
{
	StringInfoData out;
	SiftCc	   *cc;
	uint32		n = c->n;
	int64	   *dec = palloc(sizeof(int64) * (n + SIFT_COL_PAD / 8));
	uint64	   *bits = palloc0(sizeof(uint64) * SIFT_COL_NWORDS(n));
	uint64		h = 0x9E3779B97F4A7C15;
	int64		mn = INT64_MAX,
				mx = INT64_MIN,
				sum;

	initStringInfo(&out);
	sift_cc_encode(&out, c->kind, c->v, n, c->nulls, pol, NULL);
	if (pol->mode != SIFT_CCM_LAYER && pol->alg != SIFT_CC_ALG_AUTO)
		h = hash_bytes_extended((unsigned char *) out.data, out.len, h);
	cc = sift_cc_open(CurrentMemoryContext, (uint8 *) out.data, out.len, true);
	sift_cc_decode(cc, dec);
	for (uint32 i = 0; i < n; i++)
	{
		if (!is_null(c, i))
		{
			*bad += dec[i] != c->v[i];
			mn = Min(mn, dec[i]);
			mx = Max(mx, dec[i]);
		}
		*bad += sift_cc_get(cc, i) != dec[i];
	}
	h = hash_bytes_extended((unsigned char *) dec, sizeof(int64) * n, h);
	for (int q = 0; q < 4 && n; q++)
	{
		int64		lo = q == 0 ? mn : dec[(n / 4) * q],
					hi = q == 0 ? mx : lo + ((mx - lo) >> (2 * q));
		uint64		cnt;

		memset(bits, 0, sizeof(uint64) * SIFT_COL_NWORDS(n));
		sift_cc_filter(cc, lo, hi, bits);
		for (uint32 i = 0; i < n; i++)
			*bad += (((bits[i / 64] >> (i % 64)) & 1) != 0) != (dec[i] >= lo && dec[i] <= hi);
		cnt = sift_cc_sum(cc, bits, &sum);
		h = hash_bytes_extended((unsigned char *) bits, sizeof(uint64) * ((n + 63) / 64), h + cnt + (uint64) sum);
	}
	sift_cc_close(cc);
	pfree(dec);
	pfree(bits);
	pfree(out.data);
	return h;
}

/* sift._test_cc_digest(seed int8, iters int4): a hash of the codec's output on random chunks at the current sift.simd level; 0 mismatches or ERROR */
Datum
sift_test_cc_digest(PG_FUNCTION_ARGS)
{
	pg_prng_state rs;
	uint64		h = 0,
				bad = 0;

	pg_prng_seed(&rs, (uint64) PG_GETARG_INT64(0));
	for (int it = 0; it < PG_GETARG_INT32(1); it++)
	{
		Chunk		c;
		SiftCcPolicy pol;

		gen_chunk(&rs, &c);
		for (int p = 0; p < NPOL; p++)
			if (policy_at(p, &pol))
				h = h * 31 + chunk_digest(&c, &pol, &bad);
	}
	if (bad)
		elog(ERROR, "the codec disagrees with the plain values in %llu places", (unsigned long long) bad);
	PG_RETURN_INT64((int64) h);
}

/* The kernels of a level: the codec's table and the column engine's one it runs on */
static void
set_level(SiftSimdLevel level)
{
	sift_cck = sift_cc_kernels_for(level);
	sift_colk = sift_col_kernels_for(level);
}

/* sift._test_cc_equiv(seed int8, iters int4): chunk digests at every level the CPU supports that differ from the scalar level's */
Datum
sift_test_cc_equiv(PG_FUNCTION_ARGS)
{
	pg_prng_state rs;
	const SiftCcKernels *saved = sift_cck;
	const SiftColKernels *saved_col = sift_colk;
	uint64		mismatches = 0,
				bad = 0;

	pg_prng_seed(&rs, (uint64) PG_GETARG_INT64(0));
	for (int it = 0; it < PG_GETARG_INT32(1); it++)
	{
		Chunk		c;

		gen_chunk(&rs, &c);
		for (int p = 0; p < NPOL; p++)
		{
			SiftCcPolicy pol;
			uint64		want;

			if (!policy_at(p, &pol))
				continue;
			set_level(SIFT_SIMD_SCALAR);
			want = chunk_digest(&c, &pol, &bad);
			for (int l = SIFT_SIMD_AVX2; l <= SIFT_SIMD_AVX512; l++)
			{
				set_level((SiftSimdLevel) l);
				mismatches += chunk_digest(&c, &pol, &bad) != want;
			}
		}
	}
	sift_cck = saved;
	sift_colk = saved_col;
	PG_RETURN_INT64((int64) (mismatches + bad));
}

/* ---------- damaged blobs ---------- */

static void
try_blob(const uint8 *blob, Size len, bool verify, uint64 *raised, uint64 *decoded)
{
	MemoryContext oldcx = CurrentMemoryContext,
				cx = AllocSetContextCreate(oldcx, "cc corrupt", ALLOCSET_DEFAULT_SIZES);

	MemoryContextSwitchTo(cx);
	PG_TRY();
	{
		SiftCc	   *cc = sift_cc_open(cx, blob, len, verify);
		uint32		n = sift_cc_nrows(cc);
		int64		sum;

		if (n <= (1u << 20))
		{
			int64	   *dec = palloc(sizeof(int64) * (n + SIFT_COL_PAD / 8));
			uint64	   *bits = palloc0(sizeof(uint64) * SIFT_COL_NWORDS(n));

			sift_cc_decode(cc, dec);
			sift_cc_filter(cc, sift_cc_min(cc), sift_cc_min(cc) + 1000, bits);
			sift_cc_sum(cc, bits, &sum);
			for (uint32 k = 0; n && k < 16; k++)
				sift_cc_get(cc, (k * 2654435761u) % n);
		}
		(*decoded)++;
	}
	PG_CATCH();
	{
		ErrorData  *e;

		MemoryContextSwitchTo(oldcx);
		e = CopyErrorData();
		if (e->sqlerrcode != ERRCODE_DATA_CORRUPTED && e->sqlerrcode != ERRCODE_FEATURE_NOT_SUPPORTED)
			PG_RE_THROW();
		FlushErrorState();
		FreeErrorData(e);
		(*raised)++;
	}
	PG_END_TRY();
	MemoryContextSwitchTo(oldcx);
	MemoryContextDelete(cx);
}

/* sift._test_cc_corrupt(seed int8, iters int4, OUT raised int8, OUT decoded int8): damaged blobs raise the corruption errors or decode, never crash */
Datum
sift_test_cc_corrupt(PG_FUNCTION_ARGS)
{
	pg_prng_state rs;
	uint64		raised = 0,
				decoded = 0;
	TupleDesc	td;
	Datum		v[2];
	bool		nul[2] = {false};

	if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	pg_prng_seed(&rs, (uint64) PG_GETARG_INT64(0));
	for (int it = 0; it < PG_GETARG_INT32(1); it++)
	{
		Chunk		c;
		SiftCcPolicy pol;
		StringInfoData out;

		gen_chunk(&rs, &c);
		while (!policy_at(pg_prng_uint32(&rs) % NPOL, &pol))
			;
		initStringInfo(&out);
		sift_cc_encode(&out, c.kind, c.v, c.n, c.nulls, &pol, NULL);
		for (int m = 0; m < 100; m++)
		{
			uint8	   *b = palloc(out.len);
			Size		len = out.len;

			memcpy(b, out.data, len);
			for (int k = 1 + (int) (pg_prng_uint32(&rs) % 3); k; k--)
				b[pg_prng_uint32(&rs) % len] = (uint8) pg_prng_uint32(&rs);
			if (pg_prng_uint32(&rs) % 8 == 0)
				len = (pg_prng_uint32(&rs) % len) & ~(Size) 7;
			try_blob(b, len, pg_prng_uint32(&rs) % 4 == 0, &raised, &decoded);
			pfree(b);
		}
	}
	v[0] = Int64GetDatum((int64) raised);
	v[1] = Int64GetDatum((int64) decoded);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(td, v, nul)));
}

#endif							/* SIFT_TESTING */
