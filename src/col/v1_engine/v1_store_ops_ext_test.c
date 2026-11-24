/*
 * col/v1_engine/v1_store_ops_ext_test.c - test-only SQL functions sift._test_v1_*, compiled only with SIFT_TESTING and declared by the test that uses them: the order, jsonb and preagg sub-vtables of
 * the Format 1 store (core/store_ops.h) as the engine reaches them through sift_store_ops_for_oid. _test_v1_ops says what the owner hands over, _test_v1_topn runs scan_rows of an ORDER BY ..
 * LIMIT of one column, _test_v1_unnest runs unnest_run of a LATERAL unnest to groups of (parent column, element), and _test_v1_match opens the source on a match set of heap addresses.
 *
 * check-memory: allow-allocs the arrays follow the rows of a test table, a few thousand
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_ext.h"
#include "agg/v6/v6_rows_topn.h"
#include "agg/v6/v6_vexpr.h"
#include "col/v1/v1_store_ops.h"
#include "core/catname.h"
#include "core/compat.h"
#include "core/filter.h"
#include "col/v1/v1_reader.h"
#include "core/store_ops.h"
#include "table/v1/v1_tbl_ops.h"

PG_FUNCTION_INFO_V1(sift_test_v1_ops);
PG_FUNCTION_INFO_V1(sift_test_v1_topn);
PG_FUNCTION_INFO_V1(sift_test_v1_unnest);
PG_FUNCTION_INFO_V1(sift_test_v1_match);

static const SiftStoreOps *
ops_of(Oid relid, bool is_table)
{
	const SiftStoreOps *ops = sift_store_ops_for_oid(relid, is_table);

	if (ops == NULL || strcmp(ops->name, "v1") != 0)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("Format 1 does not own relation %u", relid)));
	return ops;
}

static int
leaf_att(Oid relid, bool is_table, int leaf)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	const int	att = leaf < 0 ? 0 : sift_v1_reader_of(is_table)->layout(r.rel)->leaf[leaf].attno;

	sift_v1_rel_close(&r);
	return att;
}

Datum
sift_test_v1_ops(PG_FUNCTION_ARGS)
{
	const Oid	relid = PG_GETARG_OID(0);
	const bool	is_table = PG_GETARG_BOOL(1);
	const SiftStoreOps *ops = ops_of(relid, is_table);
	const int	att = PG_GETARG_INT32(2);
	TupleDesc	desc;
	Datum		values[7];
	bool		nulls[7] = {false};
	int			dim = -1,
				meas = -1;
	const bool	cube = ops->preagg->cube_leaves(relid, is_table, &dim, &meas);

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	values[0] = BoolGetDatum(ops->order != NULL && ops->jsonb != NULL && ops->preagg != NULL);
	values[1] = BoolGetDatum(ops->order->sorted_by(relid, is_table, &att, 1));
	values[2] = Int32GetDatum(is_table ? -1 : ops->order->lead_att(relid));
	values[3] = Int32GetDatum(ops->jsonb->key_leaf(relid, att, "k", 1));
	values[4] = BoolGetDatum(ops->jsonb->unnest_run != NULL);
	values[5] = Int32GetDatum(cube ? leaf_att(relid, is_table, dim) : -1);
	values[6] = Int32GetDatum(cube ? leaf_att(relid, is_table, meas) : -1);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}

/* ---- the row Top-N ---- */

static Oid
order_op(Oid typid, bool desc)
{
	return OpernameGetOprid(list_make1(makeString(desc ? ">" : "<")), typid, typid);
}

/* column > value over the columns of the index (INDEX_VAR), as the scan hands its quals to the format */
static List *
index_qual_gt(int att, Oid typid, int64 value)
{
	Var		   *var = makeVar(INDEX_VAR, att, typid, -1, InvalidOid, 0);
	Const	   *c = makeConst(typid, -1, InvalidOid, typid == INT4OID ? 4 : 8, typid == INT4OID ? Int32GetDatum((int32) value) : Int64GetDatum(value), false, true);
	OpExpr	   *op = (OpExpr *) make_opclause(OpernameGetOprid(list_make1(makeString(">")), typid, typid), BOOLOID, false, (Expr *) var, (Expr *) c, InvalidOid, InvalidOid);

	set_opfuncid(op);
	return list_make1(op);
}

Datum
sift_test_v1_topn(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	const Oid	relid = PG_GETARG_OID(0);
	const bool	is_table = PG_GETARG_BOOL(1);
	const int	att = PG_GETARG_INT32(2);
	const bool	desc = PG_GETARG_BOOL(3);
	const bool	nulls_first = PG_GETARG_BOOL(4);
	const int	k = PG_GETARG_INT32(5);
	const SiftStoreOps *ops = ops_of(relid, is_table);
	Relation	rel = is_table ? table_open(sift_rel_must_exist(relid), AccessShareLock) : index_open(sift_rel_must_exist(relid), AccessShareLock);
	Relation	heap = is_table ? rel : table_open(rel->rd_index->indrelid, AccessShareLock);
	const AttrNumber heap_att = is_table ? att : rel->rd_index->indkey.values[att - 1];
	const Oid	typid = TupleDescAttr(RelationGetDescr(heap), heap_att - 1)->atttypid;
	SiftV6Spec *spec = palloc0(sizeof(SiftV6Spec));
	SiftV6ScanStats stats = {0};
	SiftV6Rows *rows;
	IndexFetchTableData *fetch;
	TupleTableSlot *slot;
	Oid			outfn;
	bool		varlena;
	uint64		addr;
	int			rank = 0;

	InitMaterializedSRF(fcinfo, 0);
	*spec = (SiftV6Spec) {.relid = relid,.is_table = is_table,.nkeys = 1,.key = palloc0(sizeof(SiftV6Col)),.rows = true,.topk = k,.ntopk = 1};
	spec->key[0] = (SiftV6Col) {.attno = att,.typid = typid};
	spec->topk_item[0] = (SiftV6TopKItem) {.pos = 0,.sortop = order_op(typid, desc),.collid = typid == TEXTOID ? DEFAULT_COLLATION_OID : InvalidOid,.nulls_first = nulls_first};
	if (!PG_ARGISNULL(6))
		spec->indexqual = index_qual_gt(att, typid, PG_GETARG_INT64(6));
	rows = sift_v6_rows_new(CurrentMemoryContext, spec);
	ops->order->scan_rows(spec, rows, heap, is_table ? NULL : rel, NULL, 0, GetActiveSnapshot(), &stats);
	sift_v6_rows_finish(rows);
	getTypeOutputInfo(typid, &outfn, &varlena);
	fetch = sift_table_index_fetch_begin(heap);
	slot = table_slot_create(heap, NULL);
	while (sift_v6_rows_next(rows, &addr))
	{
		ItemPointerData tid;
		bool		again = false,
					dead = false,
					isnull;
		Datum		values[2],
					key;
		bool		nulls[2] = {false, false};

		sift_f2_tid_set(&tid, addr);
		if (!table_index_fetch_tuple(fetch, &tid, GetActiveSnapshot(), slot, &again, &dead))
			elog(ERROR, "a row the scan kept is not in the heap");
		key = slot_getattr(slot, heap_att, &isnull);
		values[0] = Int32GetDatum(++rank);
		values[1] = isnull ? (Datum) 0 : CStringGetTextDatum(OidOutputFunctionCall(outfn, key));
		nulls[1] = isnull;
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, values, nulls);
	}
	ExecDropSingleTupleTableSlot(slot);
	table_index_fetch_end(fetch);
	if (!is_table)
		table_close(heap, NoLock);
	relation_close(rel, NoLock);
	PG_RETURN_NULL();
}

/* ---- the unnest ---- */

/* GROUP BY [parent column,] element columns, count(*) over the elements of the array (or jsonb recordset) column att */
Datum
sift_test_v1_unnest(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	const Oid	relid = PG_GETARG_OID(0);
	const bool	is_table = PG_GETARG_BOOL(1);
	const int	att = PG_GETARG_INT32(2);
	const int	group_att = PG_GETARG_INT32(3);
	const bool	left = PG_GETARG_BOOL(4);
	const bool	recordset = PG_GETARG_BOOL(5);
	const SiftStoreOps *ops = ops_of(relid, is_table);
	Relation	rel = is_table ? table_open(sift_rel_must_exist(relid), AccessShareLock) : index_open(sift_rel_must_exist(relid), AccessShareLock);
	Relation	heap = is_table ? rel : table_open(rel->rd_index->indrelid, AccessShareLock);
	const AttrNumber heap_att = is_table ? att : rel->rd_index->indkey.values[att - 1];
	const Oid	atype = TupleDescAttr(RelationGetDescr(heap), heap_att - 1)->atttypid;
	const Oid	etype = recordset ? INT4OID : get_element_type(atype);
	const Oid	gtype = group_att > 0 ? TupleDescAttr(RelationGetDescr(heap), (is_table ? group_att : rel->rd_index->indkey.values[group_att - 1]) - 1)->atttypid : InvalidOid;
	SiftV6Spec *spec = palloc0(sizeof(SiftV6Spec));
	SiftV6Ext  *x = palloc0(sizeof(SiftV6Ext));
	SiftV6ScanStats stats = {0};
	const int	nelem = recordset ? 2 : 1;
	const int	nkeys = nelem + (group_att > 0 ? 1 : 0);
	TupleDesc	desc = CreateTemplateTupleDesc(nkeys + 1);
	SiftV6Group *g;
	Datum		values[4];
	bool		nulls[4];
	int			at = 0;

	InitMaterializedSRF(fcinfo, 0);
	x->unnest = true;
	x->elem = (SiftV6XElem) {.fn = recordset ? SV6E_RECORDSET : SV6E_UNNEST,.attno = att,.type = etype,.left = left,.ncols = recordset ? 2 : 1};
	x->elem.col_type[0] = etype;
	x->elem.col_type[1] = TEXTOID;
	if (recordset)
		x->elem_names = list_make2(makeString("a"), makeString("b"));
	*spec = (SiftV6Spec) {.relid = relid,.is_table = is_table,.nkeys = nkeys,.key = palloc0(nkeys * sizeof(SiftV6Col)),.naggs = 1,.agg = palloc0(sizeof(SiftV6Agg)),.ext = x};
	spec->agg[0] = (SiftV6Agg) {.kind = SV6_COUNT_STAR,.restype = INT8OID};
	if (group_att > 0)
		spec->key[at++] = (SiftV6Col) {.attno = group_att,.typid = gtype};
	for (int c = 0; c < nelem; c++)
		spec->key[at++] = (SiftV6Col) {.src = SV6SRC_ELEM,.slot = c,.typid = x->elem.col_type[c]};
	for (int i = 0; i < nkeys; i++)
		TupleDescInitEntry(desc, i + 1, NULL, spec->key[i].typid, -1, 0);
	TupleDescInitEntry(desc, nkeys + 1, NULL, INT8OID, -1, 0);
	sift_tupdesc_finalize(desc);
	BlessTupleDesc(desc);
	g = sift_v6_group_new(spec, CurrentMemoryContext, desc);
	ops->jsonb->unnest_run(spec, g, heap, is_table ? NULL : rel, GetActiveSnapshot(), &stats, NULL);
	sift_v6_group_finish(g);
	while (sift_v6_group_next(g, values, nulls))
	{
		Datum		out[4] = {0};
		bool		onull[4] = {true, true, true, false};
		int			slot = group_att > 0 ? 0 : 1;

		for (int i = 0; i < nkeys; i++, slot++)
		{
			Oid			fn;
			bool		varlena;

			getTypeOutputInfo(spec->key[i].typid, &fn, &varlena);
			onull[slot] = nulls[i];
			out[slot] = nulls[i] ? (Datum) 0 : CStringGetTextDatum(OidOutputFunctionCall(fn, values[i]));
		}
		out[3] = values[nkeys];
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, out, onull);
	}
	if (!is_table)
		table_close(heap, NoLock);
	relation_close(rel, NoLock);
	PG_RETURN_NULL();
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

/* the integer attribute att (1-based) of the rows of the source opened on the match set, by the source's own fetch of each row */
Datum
sift_test_v1_match(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	const Oid	relid = PG_GETARG_OID(0);
	const bool	is_table = PG_GETARG_BOOL(1);
	const int	att = PG_GETARG_INT32(2);
	Relation	rel = is_table ? table_open(sift_rel_must_exist(relid), AccessShareLock) : index_open(sift_rel_must_exist(relid), AccessShareLock);
	SiftV6Input in;
	SiftV6Source *src;
	SiftV6Batch b;
	const AttrNumber heap_att = is_table ? att : rel->rd_index->indkey.values[att - 1];

	ops_of(relid, is_table);
	InitMaterializedSRF(fcinfo, 0);
	if (is_table)
		sift_v6_input_init_table(&in, rel);
	else
		sift_v6_input_init_rel(&in, rel);
	in.col[0] = (SiftV6InCol) {.attno = att,.typid = INT4OID,.type = SVEC_I32};
	in.ncols = 1;
	src = sift_v1_source_open(CurrentMemoryContext, GetActiveSnapshot(), &in, NULL, NULL, match_set(PG_GETARG_ARRAYTYPE_P(3)));
	while (src->next(src, &b))
		for (int r = 0; r < b.n; r++)
		{
			Datum		value;
			bool		isnull;

			if (b.mask != NULL && !((b.mask[r >> 6] >> (r & 63)) & 1))
				continue;
			if (!src->fetch(src, b.tag[r], &heap_att, 1, &value, &isnull))
				continue;
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, (Datum[]) {Int32GetDatum(((const int32 *) b.col[0].data)[r]), value}, (bool[]) {false, isnull});
		}
	src->close(src);
	relation_close(rel, NoLock);
	PG_RETURN_NULL();
}

#endif							/* SIFT_TESTING */
