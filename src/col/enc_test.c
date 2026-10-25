/*
 * col/enc_test.c - test-only SQL functions sift._test_col_enc_*, compiled only with SIFT_TESTING and registered by the pg_sift_test
 * extension. They build random batches (every storage class, NULLs, overflow, arrays, sparse and dense pages, strings) and check L1
 * (col/enc.c) against a naive evaluation of the same rows.
 * The checks are in gen_test.c, schema_test.c, batch_test.c, chunk_test.c and unit_test.c; what they share is in enc_test_int.h.
 */

#include "postgres.h"

#ifdef SIFT_TESTING

#include "common/pg_prng.h"
#include "funcapi.h"

#include "col/enc.h"
#include "col/enc_test_int.h"
#include "core/sql.h"

PG_FUNCTION_INFO_V1(sift_test_col_enc_roundtrip);

static void
run_round(TCtx *t, uint32 nrows)
{
	Case	   *c = gen_case(t);
	uint32		n;
	SiftKey    *keys = gen_keys(t, c->sb, nrows, &n);
	SiftColRow *rows = gen_rows(t, c, keys, n);
	SiftColBatch *b = batch_of(c, rows, n);
	SiftColBatch *copy = sift_cbatch_create(CurrentMemoryContext, c->schema);

	check_batch_rows(t, "batch", c, b, rows, n);

	sift_cbatch_reset(copy, c->sb);
	for (uint32 r = 0; r < n; r++)
		sift_cbatch_add_from(copy, b, r);
	check_batch_rows(t, "add_from", c, copy, rows, n);

	check_schema(t, c);
	check_dict(t);
	check_unit(t, c, b, rows, n, NULL);
	check_unit(t, c, b, rows, n, random_policy(t, c->schema->ncols));

	sift_cbatch_reset(b, c->sb + 1);
	CHECK(t, b->nrows == 0 && b->col[0].n == 0, "reset leaves rows");
	check_merge(t, c, keys, n);
	check_errors(t, c, rows, n);
	check_vers(t, c, rows, n);
}

Datum
sift_test_col_enc_roundtrip(PG_FUNCTION_ARGS)
{
	int32		seed = PG_GETARG_INT32(0),
				nrows = PG_GETARG_INT32(1),
				rounds = PG_GETARG_INT32(2);
	TCtx		t = {0};
	MemoryContext rctx = AllocSetContextCreate(CurrentMemoryContext, "col_enc round", ALLOCSET_DEFAULT_SIZES);
	Datum		values[3];
	bool		nulls[3] = {false, false, false};

	pg_prng_seed(&t.rng, (uint64) seed);
	for (int i = 0; i < rounds; i++)
	{
		MemoryContext old = MemoryContextSwitchTo(rctx);

		t.full = i == 0 && nrows >= 40000;
		t.wide = t.wide || t.full;
		run_round(&t, (uint32) Max(nrows, 1));
		MemoryContextSwitchTo(old);
		MemoryContextReset(rctx);
	}
	values[0] = Int64GetDatum(t.checks);
	values[1] = Int64GetDatum(t.failures);
	values[2] = Int64GetDatum(t.encodings);
	PG_RETURN_DATUM(sift_result_row(fcinfo, values, nulls));
}

#define SIZE_ROWS	(SIFT_SB_PAGES * MaxHeapTuplesPerPage)

typedef enum SizeShape
{
	SZ_DICT20, SZ_DICT3_NULLS, SZ_DATE_MONTH, SZ_DATE_YEAR, SZ_SMALL_INT, SZ_QTY, SZ_BOOL, SZ_TIMESTAMP, SZ_SEQ_ID, SZ_INT8, SZ_PRICE8,
	SZ_PRICE4, SZ_CONST, SZ_STORE_NULLS, SZ_ITEMS, SZ_NSHAPES
} SizeShape;

static const char *const size_names[] = {
	"status, dictionary of 20 values", "channel, dictionary of 3 values, 10% NULL", "order date, clustered in one month",
	"order date, random in a year", "discount percent, 0..100", "quantity, 1..5", "boolean", "timestamp, random in a day (microseconds)",
	"sequential id", "int8, random", "price, float8 with 2 decimals", "price, float4 with 2 decimals", "constant", "tenant_id, 0..50, 10% NULL",
	"items[]: 0..5 elements of (sku 200 values, qty 1..5, price float8)"
};

static const char *const size_status[] = {"new", "pending", "paid", "packed", "shipped", "delivered", "returned", "cancelled", "refunded",
	"on_hold", "backordered", "partial", "processing", "queued", "inspection", "fraud_check", "awaiting_pickup", "in_transit", "disputed", "closed"};

static void
size_value(TCtx *t, SizeShape shape, uint32 seq, SiftColValue *v)
{
	static const char *const channel[] = {"web", "store", "phone"};

	memset(v, 0, sizeof(*v));
	switch (shape)
	{
		case SZ_DICT20:
			v->v.s = size_status[RAND(t, 20)];
			break;
		case SZ_DICT3_NULLS:
			v->v.s = channel[RAND(t, 3)];
			break;
		case SZ_DATE_MONTH:
			v->v.i = 19000 + seq / 2400;
			break;
		case SZ_DATE_YEAR:
			v->v.i = 19000 + RAND(t, 365);
			break;
		case SZ_SMALL_INT:
			v->v.i = RAND(t, 101);
			break;
		case SZ_QTY:
			v->v.i = 1 + RAND(t, 5);
			break;
		case SZ_BOOL:
			v->v.i = RAND(t, 2);
			break;
		case SZ_TIMESTAMP:
			v->v.i = INT64CONST(1700000000000000) + (int64) RAND(t, 86400) * 1000000 + RAND(t, 1000000);
			break;
		case SZ_SEQ_ID:
			v->v.i = 1000000 + seq;
			break;
		case SZ_INT8:
			v->v.i = (int64) (pg_prng_uint64(&t->rng) >> 1);
			break;
		case SZ_PRICE8:
			v->v.f = RAND(t, 100000) / 100.0;
			break;
		case SZ_PRICE4:
			v->v.f = (double) (float4) (RAND(t, 100000) / 100.0);
			break;
		case SZ_CONST:
			v->v.i = 42;
			break;
		default:
			v->v.i = RAND(t, 51);
	}
	if (shape == SZ_DICT3_NULLS || shape == SZ_STORE_NULLS)
		v->isnull = RAND(t, 10) == 0;
	if (shape <= SZ_DICT3_NULLS)
		v->len = strlen(v->v.s);
}

static SiftColBatch *
size_batch(TCtx *t, SizeShape shape, SiftColSchema **schema)
{
	SiftColDef	defs[4];
	int			n = 1;
	SiftColStore store = shape <= SZ_DICT3_NULLS ? SIFT_COLS_STR : shape == SZ_PRICE8 ? SIFT_COLS_F8 : shape == SZ_PRICE4 ? SIFT_COLS_F4 : SIFT_COLS_INT;
	SiftColBatch *b;
	SiftColValue val[4],
				elem[3][8];
	SiftColValue *elems[4];
	SiftColRow	row = {.ncols = 1, .val = val, .elem = elems};
	char		skus[200][12];

	defs[0] = (SiftColDef) {0, -1, SIFT_COLK_SCALAR, store, 0, 1, store_type(store), "col"};
	if (shape == SZ_ITEMS)
	{
		defs[0] = (SiftColDef) {0, -1, SIFT_COLK_GROUP, SIFT_COLS_INT, 0, 1, SIFT_COLT_INT4, "items[]"};
		defs[1] = (SiftColDef) {1, 0, SIFT_COLK_SCALAR, SIFT_COLS_STR, 0, 1, SIFT_COLT_TEXT, "items[].sku"};
		defs[2] = (SiftColDef) {2, 0, SIFT_COLK_SCALAR, SIFT_COLS_INT, 0, 1, SIFT_COLT_INT4, "items[].qty"};
		defs[3] = (SiftColDef) {3, 0, SIFT_COLK_SCALAR, SIFT_COLS_F8, 0, 1, SIFT_COLT_FLOAT8, "items[].price"};
		n = 4;
		for (int i = 0; i < 200; i++)
			snprintf(skus[i], sizeof(skus[i]), "SKU-%05d", i * 37);
	}
	*schema = sift_cschema_create(CurrentMemoryContext, defs, n);
	b = sift_cbatch_create(CurrentMemoryContext, *schema);
	sift_cbatch_reset(b, 7);
	row.ncols = n;
	memset(elems, 0, sizeof(elems));
	for (uint32 i = 0; i < SIZE_ROWS; i++)
	{
		sift_key_to_tid(((uint64) (7 * SIFT_SB_PAGES + i / MaxHeapTuplesPerPage) << 16) | (1 + i % MaxHeapTuplesPerPage), &row.tid);
		if (shape == SZ_ITEMS)
		{
			memset(val, 0, sizeof(val));
			val[0].v.i = RAND(t, 6);
			for (int k = 0; k < val[0].v.i; k++)
			{
				const char *s = skus[RAND(t, 200)];

				memset(&elem[0][k], 0, sizeof(SiftColValue));
				memset(&elem[1][k], 0, sizeof(SiftColValue));
				memset(&elem[2][k], 0, sizeof(SiftColValue));
				elem[0][k].v.s = s;
				elem[0][k].len = strlen(s);
				elem[1][k].v.i = 1 + RAND(t, 5);
				elem[2][k].v.f = RAND(t, 100000) / 100.0;
			}
			elems[1] = elem[0];
			elems[2] = elem[1];
			elems[3] = elem[2];
		}
		else
			size_value(t, shape, i, &val[0]);
		sift_cbatch_add(b, &row);
	}
	return b;
}

PG_FUNCTION_INFO_V1(sift_test_col_enc_sizes);

/* (shape text, rows int8, bytes int8, bytes_per_row float8): the encoded unit record of each shape in a full superblock */
Datum
sift_test_col_enc_sizes(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TCtx		t = {0};

	InitMaterializedSRF(fcinfo, 0);
	pg_prng_seed(&t.rng, 42);
	for (int shape = 0; shape < SZ_NSHAPES; shape++)
	{
		MemoryContext sctx = AllocSetContextCreate(CurrentMemoryContext, "col_enc size", ALLOCSET_DEFAULT_SIZES);
		MemoryContext old = MemoryContextSwitchTo(sctx);
		SiftColSchema *schema;
		SiftColBatch *b = size_batch(&t, (SizeShape) shape, &schema);
		StringInfoData buf;
		Size		start;
		Datum		values[4];
		bool		nulls[4] = {false, false, false, false};

		initStringInfo(&buf);
		sift_cunit_encode(b, NULL, NULL, NULL, true, &buf, &start, NULL);
		MemoryContextSwitchTo(old);
		values[0] = CStringGetTextDatum(size_names[shape]);
		values[1] = Int64GetDatum(b->nrows);
		values[2] = Int64GetDatum((int64) (buf.len - start));
		values[3] = Float8GetDatum((double) (buf.len - start) / b->nrows);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		MemoryContextDelete(sctx);
	}
	return (Datum) 0;
}

#endif							/* SIFT_TESTING */
