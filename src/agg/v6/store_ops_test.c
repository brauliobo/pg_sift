/*
 * agg/v6/store_ops_test.c - test-only SQL functions sift._test_store_*, compiled only with SIFT_TESTING and declared by the tests that use them: the storage seam of the unified engine
 * (core/store_ops.h) as the engine uses it, over whatever format owns the relation. _test_store_rows opens the source of the relation for some of its attributes, as the batches
 * the engine reads, and gives back the rows with the size of their batch, their tag and the text of their values, to be held to the heap's own answer; _test_store_ops gives what the owner
 * tells the planner.
 *
 * check-memory: allow-allocs the arrays follow the rows of a test table, a few thousand
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "storage/lmgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "agg/v6/v6_vexpr.h"
#include "core/filter.h"
#include "core/store_ops.h"

PG_FUNCTION_INFO_V1(sift_test_store_rows);
PG_FUNCTION_INFO_V1(sift_test_store_ops);

typedef struct StoreScan
{
	Oid			relid;
	bool		is_table;
	SiftStoreSchema schema;
	SiftV6Input in;
	int			nreq;
	Oid			outfn[SIFT_V6_MAX_COLS];
} StoreScan;

static SiftV6Class
class_of(const SiftStoreAtt *a)
{
	switch (a->vec_type)
	{
		case SVEC_BOOL:
			return SV6C_BOOL;
		case SVEC_I32:
			return SV6C_I32;
		case SVEC_I64:
		case SVEC_NUM64:
			return SV6C_I64;
		case SVEC_F64:
			return SV6C_F64;
		case SVEC_STRVIEW:
			return SV6C_TEXT;
		default:
			return SV6C_DATUM;
	}
}

static const SiftStoreOps *
ops_of(Oid relid, bool is_table)
{
	const SiftStoreOps *ops;

	LockRelationOid(relid, AccessShareLock);
	ops = sift_store_ops_for_oid(relid, is_table);

	if (ops == NULL)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("no storage format owns relation %u", relid)));
	return ops;
}

/* ---- the quals ---- */

/* column > value, over an integer attribute of the input */
static SiftV6Vexpr *
quals_gt(StoreScan *sc, int att, int64 value)
{
	const SiftStoreAtt *a = &sc->schema.atts[att - 1];
	Datum		d = a->typid == INT2OID ? Int16GetDatum((int16) value) : a->typid == INT4OID ? Int32GetDatum((int32) value) : Int64GetDatum(value);
	Var		   *var = makeVar(1, sc->is_table ? att : sc->in.keys[att - 1], a->typid, -1, InvalidOid, 0);
	Const	   *c = makeConst(a->typid, -1, InvalidOid, a->typid == INT2OID ? 2 : a->typid == INT4OID ? 4 : 8, d, false, true);
	OpExpr	   *op = (OpExpr *) make_opclause(OpernameGetOprid(list_make1(makeString(">")), a->typid, a->typid), BOOLOID, false, (Expr *) var, (Expr *) c, InvalidOid, InvalidOid);
	SiftV6Vexpr *out = palloc0(sizeof(SiftV6Vexpr));
	bool		ok;

	set_opfuncid(op);
	ok = sift_v6_vexpr_quals(CurrentMemoryContext, list_make1(op), 1, NULL, &sc->in, out);
	if (!ok)
		ereport(ERROR, (errmsg("the engine does not compile the qual on attribute %d", att)));
	return out;
}

/* ---- the match set of a hybrid scan ---- */

static int
tid_cmp(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

static SiftFilter *
match_set(ArrayType *tids)
{
	int			n = ArrayGetNItems(ARR_NDIM(tids), ARR_DIMS(tids));
	ItemPointerData *t = palloc(n * sizeof(ItemPointerData));
	SiftFilter *f = sift_filter_create(CurrentMemoryContext);

	memcpy(t, ARR_DATA_PTR(tids), n * sizeof(ItemPointerData));
	qsort(t, n, sizeof(ItemPointerData), tid_cmp);
	for (int i = 0; i < n;)
	{
		uint64		offmask[SIFT_OFFMASK_WORDS] = {0};
		const BlockNumber blk = ItemPointerGetBlockNumber(&t[i]);

		for (; i < n && ItemPointerGetBlockNumber(&t[i]) == blk; i++)
			offmask[(ItemPointerGetOffsetNumber(&t[i]) - 1) >> 6] |= UINT64CONST(1) << ((ItemPointerGetOffsetNumber(&t[i]) - 1) & 63);
		sift_filter_add_page(f, blk, offmask);
	}
	return f;
}

/* ---- the rows ---- */

static Datum
value_datum(const SiftStoreAtt *a, const SiftVec *v, int r)
{
	switch (v->type)
	{
		case SVEC_BOOL:
			return BoolGetDatum(((const uint8 *) v->data)[r] != 0);
		case SVEC_I32:
			return a->typid == INT2OID ? Int16GetDatum((int16) ((const int32 *) v->data)[r]) : Int32GetDatum(((const int32 *) v->data)[r]);
		case SVEC_I64:
			return Int64GetDatum(((const int64 *) v->data)[r]);
		case SVEC_F64:
			return a->typid == FLOAT4OID ? Float4GetDatum((float4) ((const double *) v->data)[r]) : Float8GetDatum(((const double *) v->data)[r]);
		case SVEC_NUM64:
			return NumericGetDatum(int64_div_fast_to_numeric(((const int64 *) v->data)[r], v->scale));
		case SVEC_STRVIEW:
			{
				const SiftStrView *sv = &((const SiftStrView *) v->data)[r];
				char	   *s = palloc(sv->len + 1);

				if (sv->len > 12)
					memcpy(s, sv->u.ptr, sv->len);
				else
				{
					memcpy(s, sv->prefix, Min(sv->len, 4));
					if (sv->len > 4)
						memcpy(s + 4, sv->u.tail, sv->len - 4);
				}
				s[sv->len] = '\0';
				return CStringGetTextDatum(s);
			}
		default:
			return ((const Datum *) v->data)[r];
	}
}

static char *
datum_text(StoreScan *sc, int i, Datum d)
{
	if (sc->in.col[i].type == SVEC_BOOL)
		return DatumGetBool(d) ? "true" : "false";
	return OidOutputFunctionCall(sc->outfn[i], d);
}

static ArrayType *
row_values(StoreScan *sc, const SiftV6Batch *b, int r)
{
	Datum		vals[SIFT_V6_MAX_COLS];
	bool		nulls[SIFT_V6_MAX_COLS];
	int			dims[1] = {sc->nreq};
	int			lbs[1] = {1};

	for (int i = 0; i < sc->nreq; i++)
	{
		const SiftVec *v = &b->col[i];
		const bool	valid = (v->valid[r >> 6] >> (r & 63)) & 1;
		const bool	nan = v->nan != NULL && ((v->nan[r >> 6] >> (r & 63)) & 1);

		nulls[i] = !valid && !nan;
		if (nan)
			vals[i] = CStringGetTextDatum("NaN");
		else if (!valid)
			vals[i] = (Datum) 0;
		else
		{
			const Datum d = value_datum(&sc->schema.atts[sc->in.col[i].attno - 1], v, r);

			vals[i] = v->type == SVEC_STRVIEW ? d : CStringGetTextDatum(datum_text(sc, i, d));
		}
	}
	return construct_md_array(vals, nulls, 1, dims, lbs, TEXTOID, -1, false, TYPALIGN_INT);
}

/* The same attributes of the heap row the tag names, by the source's own fetch: NULL when it finds none */
static ArrayType *
fetched_values(StoreScan *sc, SiftV6Source *src, uint64 tag)
{
	Datum		vals[SIFT_V6_MAX_COLS];
	bool		nulls[SIFT_V6_MAX_COLS];
	AttrNumber	attnos[SIFT_V6_MAX_COLS];
	int			dims[1] = {sc->nreq};
	int			lbs[1] = {1};

	for (int i = 0; i < sc->nreq; i++)
		attnos[i] = sc->is_table ? sc->in.col[i].attno : sc->in.keys[sc->in.col[i].attno - 1];
	if (!src->fetch(src, tag, attnos, sc->nreq, vals, nulls))
		return NULL;
	for (int i = 0; i < sc->nreq; i++)
		if (!nulls[i])
			vals[i] = CStringGetTextDatum(datum_text(sc, i, vals[i]));
	return construct_md_array(vals, nulls, 1, dims, lbs, TEXTOID, -1, false, TYPALIGN_INT);
}

static void
scan_start(StoreScan *sc, Oid relid, bool is_table, ArrayType *atts)
{
	const SiftStoreOps *ops = ops_of(relid, is_table);
	Relation	rel = is_table ? table_open(relid, AccessShareLock) : index_open(relid, AccessShareLock);
	const int32 *want = (const int32 *) ARR_DATA_PTR(atts);

	sc->relid = relid;
	sc->is_table = is_table;
	sc->nreq = ArrayGetNItems(ARR_NDIM(atts), ARR_DIMS(atts));
	ops->schema(relid, is_table, &sc->schema);
	if (is_table)
		sift_v6_input_init_table(&sc->in, rel);
	else
		sift_v6_input_init_rel(&sc->in, rel);
	for (int i = 0; i < sc->nreq; i++)
	{
		const SiftStoreAtt *a = &sc->schema.atts[want[i] - 1];
		bool		out_is_varlena;

		if (!ops->leaf_holds(relid, is_table, want[i], class_of(a)))
			ereport(ERROR, (errmsg("attribute %d of relation %u is not a leaf the format decodes as class %d", want[i], relid, (int) class_of(a))));
		sc->in.col[i] = (SiftV6InCol) {.attno = want[i],.typid = a->typid,.type = a->vec_type,.scale = a->scale,.prec = a->prec};
		getTypeOutputInfo(a->typid, &sc->outfn[i], &out_is_varlena);
	}
	sc->in.ncols = sc->nreq;
	relation_close(rel, NoLock);
}

Datum
sift_test_store_rows(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			relid = PG_GETARG_OID(0);
	bool		is_table = PG_GETARG_BOOL(1);
	StoreScan		sc;
	const SiftStoreOps *ops = ops_of(relid, is_table);
	EState	   *estate = CreateExecutorState();
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	SiftV6Vexpr *quals = NULL;
	SiftV6Source *src;
	SiftV6Batch b;
	int			batch = 0;

	InitMaterializedSRF(fcinfo, 0);
	estate->es_snapshot = GetActiveSnapshot();
	scan_start(&sc, relid, is_table, PG_GETARG_ARRAYTYPE_P(2));
	sift_v6_input_bind(&sc.in, estate);
	if (!PG_ARGISNULL(3))
		quals = quals_gt(&sc, PG_GETARG_INT32(3), PG_GETARG_INT64(4));
	if (PG_ARGISNULL(5))
		src = ops->open(estate, &sc.in, quals, false);
	else
	{
		SIFT_INVARIANT(ops->hybrid != NULL && ops->hybrid->can_hybrid(relid, is_table), "relation %u takes no match set", relid);
		src = ops->hybrid->open_hybrid(estate, &sc.in, quals, match_set(PG_GETARG_ARRAYTYPE_P(5)), false);
	}
	while (src->next(src, &b))
	{
		batch++;
		for (int r = 0; r < b.n; r++)
		{
			Datum		values[5];
			bool		nulls[5] = {false, false, false, false, false};
			ArrayType  *fetched;
			ItemPointerData *tid = palloc(sizeof(ItemPointerData));

			if (b.mask != NULL && !((b.mask[r >> 6] >> (r & 63)) & 1))
				continue;
			ItemPointerSet(tid, (BlockNumber) (b.tag[r] >> 16), (OffsetNumber) (b.tag[r] & 0xFFFF));
			values[0] = Int32GetDatum(batch);
			values[1] = Int32GetDatum(b.n);
			values[2] = PointerGetDatum(tid);
			values[3] = PointerGetDatum(row_values(&sc, &b, r));
			fetched = fetched_values(&sc, src, b.tag[r]);
			values[4] = PointerGetDatum(fetched);
			nulls[4] = fetched == NULL;
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, values, nulls);
		}
	}
	src->close(src);
	MemoryContextSwitchTo(old);
	FreeExecutorState(estate);
	PG_RETURN_NULL();
}

/* The owner of the relation and what it tells the planner: its name, the vector type of each attribute, the rows, the stored bytes, the share the heap need not vet, and whether it holds each attribute */
Datum
sift_test_store_ops(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	bool		is_table = PG_GETARG_BOOL(1);
	const SiftStoreOps *ops;
	SiftStoreSchema schema;
	SiftStoreStats stats;
	StringInfoData types,
				holds;
	TupleDesc	desc;
	Datum		values[6];
	bool		nulls[6] = {false, false, false, false, false, false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	LockRelationOid(relid, AccessShareLock);
	ops = sift_store_ops_for_oid(relid, is_table);
	if (ops == NULL)
		PG_RETURN_NULL();
	ops->schema(relid, is_table, &schema);
	ops->stats(relid, is_table, &stats);
	initStringInfo(&types);
	initStringInfo(&holds);
	for (int a = 1; schema.atts != NULL && a <= schema.natts; a++)
	{
		const SiftStoreAtt *s = &schema.atts[a - 1];

		appendStringInfo(&types, "%s%d%s", a > 1 ? "," : "", (int) s->vec_type, s->is_wide ? "w" : "");
		appendStringInfoChar(&holds, ops->leaf_holds(relid, is_table, a, class_of(s)) ? '1' : '0');
	}
	values[0] = CStringGetTextDatum(ops->name);
	nulls[1] = nulls[5] = schema.atts == NULL;
	values[1] = CStringGetTextDatum(types.data);
	values[2] = Float8GetDatum(stats.live_rows);
	values[3] = Int64GetDatum((int64) stats.stored_bytes);
	values[4] = Float8GetDatum(stats.allvis_frac);
	values[5] = CStringGetTextDatum(holds.data);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}

#endif							/* SIFT_TESTING */
