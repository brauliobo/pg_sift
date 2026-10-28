/*
 * agg/stub_test.c - test-only SQL functions sift._test_aggstub_*, compiled only with SIFT_TESTING and registered by the
 * pg_sift_test extension: what the sift_colstub column source (agg/stub_src.c) presents, as rows.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/genam.h"
#include "access/table.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"

#include "utils/fmgroids.h"

#include "agg/kern.h"
#include "agg/pred_in_int.h"
#include "agg/spec.h"
#include "agg/stub.h"
#include "core/sql.h"

PG_FUNCTION_INFO_V1(sift_test_aggstub_is_index);
PG_FUNCTION_INFO_V1(sift_test_aggstub_layout);
PG_FUNCTION_INFO_V1(sift_test_aggstub_dump);
PG_FUNCTION_INFO_V1(sift_test_aggstub_units);
PG_FUNCTION_INFO_V1(sift_test_aggstub_dicts);
PG_FUNCTION_INFO_V1(sift_test_aggstub_counters);
PG_FUNCTION_INFO_V1(sift_test_aggstub_sb);
PG_FUNCTION_INFO_V1(sift_test_aggstub_memory);
PG_FUNCTION_INFO_V1(sift_test_aggstub_stats);
PG_FUNCTION_INFO_V1(sift_test_pattern_lits);
PG_FUNCTION_INFO_V1(sift_test_prefilter);

typedef struct TestScan
{
	Relation	index;
	Relation	heap;
	MemoryContext cxt;
	SiftSrcSource *src;
	const SiftSrcLayout *layout;
} TestScan;

static void
ts_open(TestScan *ts, Oid indexoid)
{
	ts->index = index_open(indexoid, AccessShareLock);
	if (!sift_aggstub_owns_index(ts->index->rd_rel->relam))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_colstub index", RelationGetRelationName(ts->index))));
	ts->heap = table_open(ts->index->rd_index->indrelid, AccessShareLock);
	ts->cxt = AllocSetContextCreate(CurrentMemoryContext, "sift_colstub test", ALLOCSET_DEFAULT_SIZES);
	ts->src = sift_colsrc_open(ts->heap, ts->index, GetTransactionSnapshot(), ts->cxt);
	ts->layout = sift_colsrc_source_layout(ts->src);
}

static void
ts_close(TestScan *ts)
{
	sift_colsrc_close(ts->src);
	MemoryContextDelete(ts->cxt);
	table_close(ts->heap, AccessShareLock);
	index_close(ts->index, AccessShareLock);
}

static void
put_row(ReturnSetInfo *rsi, Datum *values, bool *nulls)
{
	tuplestore_putvalues(rsi->setResult, rsi->setDesc, values, nulls);
}

static Datum
text_or_null(const char *s, bool *isnull)
{
	*isnull = s == NULL;
	return s ? CStringGetTextDatum(s) : (Datum) 0;
}

/* (relam oid) */
Datum
sift_test_aggstub_is_index(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(sift_aggstub_owns_index(PG_GETARG_OID(0)));
}

static const char *
kind_name(SiftSrcLeafKind k)
{
	return k == SCL_SCALAR ? "scalar" : k == SCL_ARRAY ? "array" : "object";
}

static const char *
vtype_name(SiftSrcVecType t)
{
	static const char *const names[] = {"i32", "i64", "f64", "code"};

	return names[t];
}

static char *
path_text(List *path)
{
	StringInfoData buf;

	if (path == NIL)
		return NULL;
	initStringInfo(&buf);
	foreach_ptr(String, s, path)
		appendStringInfo(&buf, buf.len ? ".%s" : "%s", strVal(s));
	return buf.data;
}

/* (index regclass) returns the leaves and the expression of the attribute they are read from */
Datum
sift_test_aggstub_layout(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftSrcLayout *l = sift_colsrc_layout(index);
	List	   *dpcontext = deparse_context_for(RelationGetRelationName(index), l->heapoid);

	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < l->nleaves; i++)
	{
		const SiftSrcLeaf *lf = &l->leaf[i];
		Datum		values[9];
		bool		nulls[9] = {false};

		values[0] = Int32GetDatum(lf->id);
		values[1] = Int32GetDatum(lf->attno);
		values[2] = Int32GetDatum(lf->parent);
		values[3] = CStringGetTextDatum(kind_name(lf->kind));
		values[4] = CStringGetTextDatum(vtype_name(lf->vtype));
		values[5] = ObjectIdGetDatum(lf->typid);
		values[6] = text_or_null(path_text(lf->path), &nulls[6]);
		values[7] = text_or_null(lf->name[0] ? lf->name : NULL, &nulls[7]);
		values[8] = CStringGetTextDatum(deparse_expression(list_nth(l->attexprs, lf->attno - 1), dpcontext, false, false));
		put_row(rsi, values, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* The text of value i of a decoded leaf, as the type's output function writes it; *code: its dictionary code */
typedef struct ValText
{
	const SiftSrcLeaf *lf;
	Oid			outfn;
} ValText;

static void
valtext_init(ValText *vt, const SiftSrcLeaf *lf)
{
	bool		isvarlena;

	vt->lf = lf;
	getTypeOutputInfo(lf->typid, &vt->outfn, &isvarlena);
}

static char *
valtext(const ValText *vt, const SiftSrcVec *v, int64 i, bool *isnull, int64 *code)
{
	Datum		d = 0;

	*code = -1;
	*isnull = v->isnull && v->isnull[i];
	if (*isnull)
		return NULL;
	switch (vt->lf->vtype)
	{
		case SCV_I32:
			d = Int32GetDatum(((const int32 *) v->data)[i]);
			if (vt->lf->typid == INT2OID)
				d = Int16GetDatum((int16) DatumGetInt32(d));
			else if (vt->lf->typid == BOOLOID)
				d = BoolGetDatum(DatumGetInt32(d) != 0);
			break;
		case SCV_I64:
			d = Int64GetDatum(((const int64 *) v->data)[i]);
			break;
		case SCV_F64:
			d = vt->lf->typid == FLOAT4OID ? Float4GetDatum((float4) ((const double *) v->data)[i]) : Float8GetDatum(((const double *) v->data)[i]);
			break;
		case SCV_CODE:
			*code = ((const uint32 *) v->data)[i];
			{
				uint32		len;
				char		buf[SIFT_SRC_DICT_ENTRY];
				const char *str = sift_srcdict_str(v->dict, *code, &len, buf);

				return pnstrdup(str, len);
			}
	}
	return OidOutputFunctionCall(vt->outfn, d);
}

static ItemPointerData
row_tid(const SiftSrcSource *src, int row)
{
	ItemPointerData tid;

	sift_key_to_tid(sift_aggstub_keys(src)[row], &tid);
	return tid;
}

static uint32
sb_arg(FunctionCallInfo fcinfo, int n, uint32 dflt)
{
	return PG_ARGISNULL(n) ? dflt : (uint32) PG_GETARG_INT32(n);
}

/*
 * (index regclass, leaf int4, sb_lo int4, sb_hi int4) returns the decoded leaf of every unit of [sb_lo, sb_hi): one row per
 * row of the unit (or, for a child leaf, per element: ord is then the ordinal of the row the element belongs to and elem its
 * number in the unit). code is the dictionary code of a text value, val the value as text.
 */
Datum
sift_test_aggstub_dump(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	int			leaf = PG_GETARG_INT32(1);
	TestScan	ts;
	SiftSrcUnit unit;
	int			unitno = 0;
	ValText		vt;

	InitMaterializedSRF(fcinfo, 0);
	ts_open(&ts, PG_GETARG_OID(0));
	if (leaf < 0 || leaf >= ts.layout->nleaves)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("leaf %d does not exist", leaf)));
	valtext_init(&vt, &ts.layout->leaf[leaf]);
	sift_colsrc_begin(ts.src, sb_arg(fcinfo, 2, 0), sb_arg(fcinfo, 3, PG_UINT32_MAX), NULL, 0);
	while (sift_colsrc_next(ts.src, &unit))
	{
		SiftSrcVec	v;
		SiftSrcVec	counts;
		int64		elem = 0;

		sift_colsrc_read(ts.src, &unit, leaf, &v);
		if (vt.lf->parent >= 0)
			sift_colsrc_read(ts.src, &unit, vt.lf->parent, &counts);
		for (int r = 0; r < unit.nrow; r++)
		{
			int64		n = 1;

			if (vt.lf->parent >= 0)
				n = (counts.isnull && counts.isnull[r]) ? 0 : ((const int32 *) counts.data)[r];
			for (int64 e = 0; e < n; e++)
			{
				ItemPointerData tid = row_tid(ts.src, r);
				Datum		values[7];
				bool		nulls[7] = {false};
				bool		isnull;
				int64		code;
				char	   *txt = valtext(&vt, &v, vt.lf->parent >= 0 ? elem : r, &isnull, &code);

				values[0] = Int32GetDatum(unitno);
				values[1] = Int32GetDatum(unit.sb);
				values[2] = Int32GetDatum(r);
				values[3] = Int32GetDatum((int32) elem);
				nulls[3] = vt.lf->parent < 0;
				values[4] = ItemPointerGetDatum(&tid);
				values[5] = Int64GetDatum(code);
				nulls[5] = code < 0;
				values[6] = text_or_null(txt, &nulls[6]);
				put_row(rsi, values, nulls);
				elem += vt.lf->parent >= 0;
			}
		}
		unitno++;
	}
	ts_close(&ts);
	return (Datum) 0;
}

static void
prune_args(FunctionCallInfo fcinfo, SiftSrcPrune *p, int *nprune)
{
	*nprune = !PG_ARGISNULL(3);
	if (*nprune)
	{
		*p = (SiftSrcPrune) {PG_GETARG_INT32(3), PG_GETARG_INT64(4), PG_GETARG_INT64(5)};
	}
}

/* (index regclass, sb_lo int4, sb_hi int4, prune_leaf int4, prune_lo int8, prune_hi int8) returns the units */
Datum
sift_test_aggstub_units(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	TestScan	ts;
	SiftSrcUnit unit;
	SiftSrcPrune prune;
	int			nprune;
	int			unitno = 0;

	InitMaterializedSRF(fcinfo, 0);
	ts_open(&ts, PG_GETARG_OID(0));
	prune_args(fcinfo, &prune, &nprune);
	sift_colsrc_begin(ts.src, sb_arg(fcinfo, 1, 0), sb_arg(fcinfo, 2, PG_UINT32_MAX), &prune, nprune);
	while (sift_colsrc_next(ts.src, &unit))
	{
		ItemPointerData first = row_tid(ts.src, 0);
		ItemPointerData last = row_tid(ts.src, unit.nrow - 1);
		Datum		values[7] = {Int32GetDatum(unitno++), Int32GetDatum(unit.sb), Int32GetDatum(unit.nrow),
								 ItemPointerGetDatum(&first), ItemPointerGetDatum(&last),
								 BoolGetDatum(true), BoolGetDatum(false)};
		bool		nulls[7] = {false};

		put_row(rsi, values, nulls);
	}
	ts_close(&ts);
	return (Datum) 0;
}

/* (index regclass, only_leaf int4) returns the dictionary of the leaf (NULL: of every text leaf) in every unit: its id (a
 * pointer), entries and bytes */
Datum
sift_test_aggstub_dicts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	TestScan	ts;
	SiftSrcUnit unit;
	int			unitno = 0;

	InitMaterializedSRF(fcinfo, 0);
	ts_open(&ts, PG_GETARG_OID(0));
	sift_colsrc_begin(ts.src, 0, PG_UINT32_MAX, NULL, 0);
	while (sift_colsrc_next(ts.src, &unit))
	{
		for (int leaf = 0; leaf < ts.layout->nleaves; leaf++)
		{
			SiftSrcVec	v;
			Datum		values[5];
			bool		nulls[5] = {false};

			if (ts.layout->leaf[leaf].vtype != SCV_CODE || (!PG_ARGISNULL(1) && PG_GETARG_INT32(1) != leaf))
				continue;
			sift_colsrc_read(ts.src, &unit, leaf, &v);
			values[0] = Int32GetDatum(unitno);
			values[1] = Int32GetDatum(leaf);
			values[2] = CStringGetTextDatum(psprintf("%p", v.dict->id));
			values[3] = Int32GetDatum(v.dict->n);
			values[4] = Int32GetDatum(sift_srcdict_whole(v.dict)->off[v.dict->n]);
			put_row(rsi, values, nulls);
		}
		unitno++;
	}
	ts_close(&ts);
	return (Datum) 0;
}

/* The padding rules of agg/kern.h: aligned data, SIFT_AGG_PAD readable bytes behind (read here, so ASan or valgrind see it) */
static volatile uint8 padding_sink;

static void
check_padding(const void *p, int64 bytes, const char *what)
{
	if (!p || (uintptr_t) p % SIFT_SIMD_ALIGN != 0)
		elog(ERROR, "sift_colstub: %s is not %d aligned", what, SIFT_SIMD_ALIGN);
	for (int64 i = 0; i < bytes + SIFT_AGG_PAD; i += 8)
		padding_sink += ((const volatile uint8 *) p)[i];
}

static void
read_all(TestScan *ts, const SiftSrcUnit *unit)
{
	check_padding(sift_aggstub_keys(ts->src), sizeof(SiftKey) * unit->nrow, "key");
	for (int leaf = 0; leaf < ts->layout->nleaves; leaf++)
	{
		SiftSrcVec	v;
		SiftSrcVec	again;
		static const int size[] = {4, 8, 8, 4};

		sift_colsrc_read(ts->src, unit, leaf, &v);
		check_padding(v.data, v.n * size[ts->layout->leaf[leaf].vtype], "data");
		if (v.isnull)
			check_padding(v.isnull, v.n, "isnull");
		if ((ts->layout->leaf[leaf].vtype == SCV_CODE) != (v.dict != NULL))
			elog(ERROR, "sift_colstub: dictionary of leaf %d", leaf);
		sift_colsrc_read(ts->src, unit, leaf, &again);
		if (again.data != v.data || again.n != v.n)
			elog(ERROR, "sift_colstub: leaf %d read twice differs", leaf);
	}
}

/* (index regclass, sb_lo int4, sb_hi int4, prune_leaf int4, prune_lo int8, prune_hi int8): reads every leaf of the units
 * returned and gives the counters */
Datum
sift_test_aggstub_counters(PG_FUNCTION_ARGS)
{
	TestScan	ts;
	SiftSrcUnit unit;
	SiftSrcPrune prune;
	int			nprune;
	SiftSrcCounters c;
	Datum		values[4];
	bool		nulls[4] = {false};

	ts_open(&ts, PG_GETARG_OID(0));
	prune_args(fcinfo, &prune, &nprune);
	sift_colsrc_begin(ts.src, sb_arg(fcinfo, 1, 0), sb_arg(fcinfo, 2, PG_UINT32_MAX), &prune, nprune);
	while (sift_colsrc_next(ts.src, &unit))
		read_all(&ts, &unit);
	sift_colsrc_counters(ts.src, &c);
	values[0] = Int64GetDatum(c.units);
	values[1] = Int64GetDatum(c.units_pruned);
	values[2] = Int64GetDatum(c.rows);
	values[3] = Int64GetDatum(c.bytes);
	ts_close(&ts);
	PG_RETURN_DATUM(sift_result_row(fcinfo, values, nulls));
}

/* (index regclass, sb int4) returns the rows of one superblock, a checksum of their TIDs and whether a parallel worker read it */
Datum
sift_test_aggstub_sb(PG_FUNCTION_ARGS)
{
	TestScan	ts;
	SiftSrcUnit unit;
	int64		rows = 0;
	int64		sum = 0;
	Datum		values[3];
	bool		nulls[3] = {false};

	ts_open(&ts, PG_GETARG_OID(0));
	sift_colsrc_begin(ts.src, PG_GETARG_INT32(1), PG_GETARG_INT32(1) + 1, NULL, 0);
	while (sift_colsrc_next(ts.src, &unit))
	{
		rows += unit.nrow;
		for (int r = 0; r < unit.nrow; r++)
			sum += sift_aggstub_keys(ts.src)[r] % 1000003;
		read_all(&ts, &unit);
	}
	values[0] = Int64GetDatum(rows);
	values[1] = Int64GetDatum(sum);
	values[2] = BoolGetDatum(IsParallelWorker());
	ts_close(&ts);
	PG_RETURN_DATUM(sift_result_row(fcinfo, values, nulls));
}

/* (index regclass, sb_lo int4, sb_hi int4): bytes the source's memory context holds after each unit was read in full */
Datum
sift_test_aggstub_memory(PG_FUNCTION_ARGS)
{
	TestScan	ts;
	SiftSrcUnit unit;
	int64		units = 0;
	int64		peak = 0;
	int64		maxrows = 0;
	Datum		values[3];
	bool		nulls[3] = {false};

	ts_open(&ts, PG_GETARG_OID(0));
	sift_colsrc_begin(ts.src, sb_arg(fcinfo, 1, 0), sb_arg(fcinfo, 2, PG_UINT32_MAX), NULL, 0);
	while (sift_colsrc_next(ts.src, &unit))
	{
		read_all(&ts, &unit);
		units++;
		maxrows = Max(maxrows, unit.nrow);
		peak = Max(peak, (int64) MemoryContextMemAllocated(ts.cxt, true));
	}
	values[0] = Int64GetDatum(units);
	values[1] = Int64GetDatum(peak);
	values[2] = Int64GetDatum(maxrows);
	ts_close(&ts);
	PG_RETURN_DATUM(sift_result_row(fcinfo, values, nulls));
}

/* (index regclass) returns the planner statistics, one row per leaf */
Datum
sift_test_aggstub_stats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftSrcLayout *l = sift_colsrc_layout(index);
	SiftSrcStats *st = palloc(sizeof(SiftSrcStats));

	InitMaterializedSRF(fcinfo, 0);
	sift_colsrc_stats(index, st);
	for (int i = 0; i < l->nleaves; i++)
	{
		Datum		values[8] = {Int32GetDatum(i), Float8GetDatum(st->nrows), Float8GetDatum(st->nunits),
								 Float8GetDatum(st->allvisible_frac), Float8GetDatum(st->ndistinct[i]),
								 Float8GetDatum(st->min[i]), Float8GetDatum(st->max[i]), Float8GetDatum(st->elems_per_row[i])};
		bool		nulls[8] = {false};

		put_row(rsi, values, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* (op text, pattern text) the literals the dictionary prefilter takes from the pattern test, '' for none: "a, b/i" (the second folded), "NOT " first when a string without one passes */
Datum
sift_test_pattern_lits(PG_FUNCTION_ARGS)
{
	static const struct
	{
		const char *name;
		Oid			funcid;
	}			ops[] = {{"like", F_TEXTLIKE}, {"nlike", F_TEXTNLIKE}, {"ilike", F_TEXTICLIKE}, {"nilike", F_TEXTICNLIKE},
	{"~", F_TEXTREGEXEQ}, {"!~", F_TEXTREGEXNE}, {"~*", F_TEXTICREGEXEQ}, {"!~*", F_TEXTICREGEXNE}};
	char	   *op = text_to_cstring(PG_GETARG_TEXT_PP(0));
	text	   *pat = PG_GETARG_TEXT_PP(1);
	SiftPatLits pl;
	StringInfoData buf;

	for (int i = 0; i < lengthof(ops); i++)
		if (strcmp(op, ops[i].name) == 0)
		{
			initStringInfo(&buf);
			if (!sift_pattern_lits_of(ops[i].funcid, PG_GET_COLLATION(), VARDATA_ANY(pat), VARSIZE_ANY_EXHDR(pat), &pl))
				PG_RETURN_TEXT_P(cstring_to_text(""));
			if (pl.neg)
				appendStringInfoString(&buf, "NOT ");
			for (int j = 0; j < pl.n; j++)
				appendStringInfo(&buf, "%s%.*s%s", j ? ", " : "", (int) pl.len[j], pl.lit[j], pl.fold[j] ? "/i" : "");
			PG_RETURN_TEXT_P(cstring_to_text(buf.data));
		}
	ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("unknown pattern operator \"%s\"", op)));
	PG_RETURN_NULL();
}

/* (reset bool) The dictionaries this backend ran a pattern test on the candidates of since the last reset, and the entries that were not tested */
Datum
sift_test_prefilter(PG_FUNCTION_ARGS)
{
	Datum		v[2] = {Int64GetDatum(sift_agg_prefilter_dicts), Int64GetDatum(sift_agg_prefilter_skipped)};
	bool		nulls[2] = {false};

	if (PG_GETARG_BOOL(0))
		sift_agg_prefilter_dicts = sift_agg_prefilter_skipped = 0;
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

#endif							/* SIFT_TESTING */
