/*
 * col/test.c - sift._test_col_*: the column source API (col/col.h section 3) from SQL, for the regression and differential tests.
 * Predicates are written "leaf:op:v1|v2;..." (ops eq ne lt le gt ge in notin null notnull prefix); "gN:leaf:op:v" is a same-element
 * predicate of group N, "gN!" requires that no element satisfies the group's predicates.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lock.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#include "utils/float.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "agg/colsrc.h"
#include "col/pagg.h"
#include "col/runs.h"
#include "col/setsum.h"

PG_FUNCTION_INFO_V1(sift_test_col_dict_cache);
PG_FUNCTION_INFO_V1(sift_test_col_tids);
PG_FUNCTION_INFO_V1(sift_test_col_read);
PG_FUNCTION_INFO_V1(sift_test_col_zones);
PG_FUNCTION_INFO_V1(sift_test_col_types);
PG_FUNCTION_INFO_V1(sift_test_col_formats);
PG_FUNCTION_INFO_V1(sift_test_col_counters);
PG_FUNCTION_INFO_V1(sift_test_col_leaf_stats);
PG_FUNCTION_INFO_V1(sift_test_col_pagg_fold);
PG_FUNCTION_INFO_V1(sift_test_col_cube_damage);
PG_FUNCTION_INFO_V1(sift_test_col_damage);
PG_FUNCTION_INFO_V1(sift_test_col_pre_stats);
PG_FUNCTION_INFO_V1(sift_test_col_set_damage);
PG_FUNCTION_INFO_V1(sift_test_col_super_damage);
PG_FUNCTION_INFO_V1(sift_test_col_run_damage);
PG_FUNCTION_INFO_V1(sift_test_col_run_file_damage);
PG_FUNCTION_INFO_V1(sift_test_col_zone_flags);
PG_FUNCTION_INFO_V1(sift_test_col_set_file_damage);
PG_FUNCTION_INFO_V1(sift_test_col_point_file_damage);

#define TEST_NOWBUF	1
#define TEST_NOVET		2
#define TEST_POINTS	8		/* open the source with SIFT_COLO_POINTS: a segment with a point directory answers an equality or a short IN list from it */
#define TEST_WAIT		4	/* after the source is open, wait for advisory lock (WAIT_KEY, 1): lets a test run VACUUM or DML mid-scan */
#define WAIT_KEY		0x51F7

typedef struct TestQuery
{
	SiftColPred *top;
	int			ntop;
	SiftColGroupPred *grp;
	int			ngrp;
	SiftColPred *gpreds;		/* the predicates of all groups, grouped */
} TestQuery;

static SiftColOp
parse_op(const char *s)
{
	static const struct
	{
		const char *name;
		SiftColOp	op;
	}			ops[] = {{"eq", SIFT_COLOP_EQ}, {"ne", SIFT_COLOP_NE}, {"lt", SIFT_COLOP_LT}, {"le", SIFT_COLOP_LE},
	{"gt", SIFT_COLOP_GT}, {"ge", SIFT_COLOP_GE}, {"in", SIFT_COLOP_IN}, {"notin", SIFT_COLOP_NOTIN},
	{"null", SIFT_COLOP_ISNULL}, {"notnull", SIFT_COLOP_NOTNULL}, {"prefix", SIFT_COLOP_PREFIX}};

	for (int i = 0; i < lengthof(ops); i++)
		if (strcmp(ops[i].name, s) == 0)
			return ops[i].op;
	elog(ERROR, "sift test: unknown operator \"%s\"", s);
	return 0;
}

static SiftColValue
parse_value(const SiftColDef *def, char *s)
{
	SiftColValue v = {0};

	/* the head of a wide value: the text of the type, as a constant of the predicate */
	if (def->lanes > 1)
	{
		if (def->type == SIFT_COLT_INTERVAL)
		{
			Interval   *iv = DatumGetIntervalP(DirectFunctionCall3(interval_in, CStringGetDatum(s), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)));
			int64		md;

			sift_col_interval_lanes(iv, &v.v.i, &v.lo, &md);
		}
		else
		{
			Datum		d = DirectFunctionCall3(numeric_in, CStringGetDatum(s), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));

			if (def->store == SIFT_COLS_STR)
			{
				int32		dscale;

				v.v.s = (const char *) sift_col_numeric_image(d, &v.len, &dscale);
			}
			else if (sift_col_numeric_wide_key(def->scale, d, SIFT_COL_ROUND_NEAREST, &v.v.i, &v.lo) == 2)
				elog(ERROR, "sift test: \"%s\" is not a number of a wide numeric leaf", s);
		}
		return v;
	}
	switch (def->store)
	{
		case SIFT_COLS_INT:
			v.v.i = strtoll(s, NULL, 10);
			break;
		case SIFT_COLS_F4:
		case SIFT_COLS_F8:
			v.v.f = strtod(s, NULL);
			break;
		default:
			if (def->type == SIFT_COLT_UUID)
			{
				v.v.s = (const char *) DatumGetUUIDP(DirectFunctionCall1(uuid_in, CStringGetDatum(s)));
				v.len = UUID_LEN;
				break;
			}
			v.v.s = s;
			v.len = strlen(s);
	}
	return v;
}

static SiftColPred
parse_pred(const SiftColSchema *sc, char *item)
{
	char	   *leaf = item,
			   *op = strchr(item, ':'),
			   *vals = NULL;
	SiftColPred p = {0};
	int			cap = 1;

	if (op == NULL)
		elog(ERROR, "sift test: bad predicate \"%s\"", item);
	*op++ = 0;
	if ((vals = strchr(op, ':')) != NULL)
		*vals++ = 0;
	p.col = atoi(leaf);
	if (p.col < 0 || p.col >= sc->ncols)
		elog(ERROR, "sift test: no leaf %d", p.col);
	p.op = parse_op(op);
	if (vals)
	{
		SiftColValue *arr = palloc(cap * sizeof(SiftColValue));

		/* '|' separates the constants; an empty constant is a value (the empty string) */
		for (char *tok = vals, *next; tok; tok = next)
		{
			next = strchr(tok, '|');
			if (next)
				*next++ = 0;
			if (p.nvals == cap)
				arr = repalloc(arr, (cap *= 2) * sizeof(SiftColValue));
			arr[p.nvals++] = parse_value(&sc->cols[p.col], tok);
		}
		p.vals = arr;
	}
	return p;
}

static void
parse_query(const SiftColSchema *sc, const char *text, TestQuery *q)
{
	char	   *copy = pstrdup(text),
			   *item,
			   *save = NULL;
	SiftColPred *all = palloc(Max(strlen(text), 1) * sizeof(SiftColPred));
	int		   *grpof = palloc(Max(strlen(text), 1) * sizeof(int));
	bool	   *neg = palloc(Max(strlen(text), 1) * sizeof(bool));
	int			n = 0;

	memset(q, 0, sizeof(*q));
	q->top = all;
	for (item = strtok_r(copy, ";", &save); item; item = strtok_r(NULL, ";", &save))
	{
		if (item[0] == 'g')
		{
			char	   *rest = strchr(item, ':');
			int			g = atoi(item + 1);

			if (rest == NULL)
				elog(ERROR, "sift test: bad group predicate \"%s\"", item);
			*rest++ = 0;
			grpof[n] = g;
			neg[n] = strchr(item, '!') != NULL;
			all[n++] = parse_pred(sc, rest);
		}
		else
		{
			grpof[n] = -1;
			all[n++] = parse_pred(sc, item);
		}
	}
	/* top level predicates first, then each group's in one run */
	q->gpreds = palloc(Max(n, 1) * sizeof(SiftColPred));
	q->top = palloc(Max(n, 1) * sizeof(SiftColPred));
	q->grp = palloc(Max(n, 1) * sizeof(SiftColGroupPred));
	for (int i = 0; i < n; i++)
		if (grpof[i] < 0)
			q->top[q->ntop++] = all[i];
	for (int i = 0, ng = 0; i < n; i++)
	{
		if (grpof[i] < 0)
			continue;
		for (int j = 0; j < q->ngrp; j++)
			if (q->grp[j].group == grpof[i] && q->grp[j].negate == neg[i])
				goto have;
		q->grp[q->ngrp++] = (SiftColGroupPred) {.group = grpof[i],.preds = &q->gpreds[ng],.negate = neg[i]};
		for (int k = i; k < n; k++)
			if (grpof[k] == grpof[i] && neg[k] == neg[i])
			{
				q->gpreds[ng++] = all[k];
				q->grp[q->ngrp - 1].npreds++;
			}
have:	;
	}
}

typedef struct TestScan
{
	Relation	heap;
	Relation	index;
	SiftColSource *src;
	TestQuery	q;
} TestScan;

static void
scan_open(TestScan *ts, Oid indexoid, const char *preds, int flags)
{
	ts->index = index_open(indexoid, AccessShareLock);
	if (!sift_col_is_index(ts->index))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_col index", RelationGetRelationName(ts->index))));
	ts->heap = table_open(ts->index->rd_index->indrelid, AccessShareLock);
	ts->src = sift_col_open(ts->heap, ts->index, GetActiveSnapshot(), CurrentMemoryContext,
							((flags & TEST_NOWBUF) ? SIFT_COLO_NOWBUF : 0) | ((flags & TEST_POINTS) ? SIFT_COLO_POINTS : 0));
	parse_query(sift_col_rel(ts->index)->schema, preds, &ts->q);
	sift_col_begin(ts->src, ts->q.top, ts->q.ntop, 0, PG_UINT32_MAX);
	if (flags & TEST_WAIT)
	{
		LOCKTAG		tag;

		SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, WAIT_KEY, 1, 2);
		(void) LockAcquire(&tag, ShareLock, false, false);
		LockRelease(&tag, ShareLock, false);
	}
}

static void
scan_close(TestScan *ts)
{
	sift_col_close(ts->src);
	table_close(ts->heap, AccessShareLock);
	index_close(ts->index, AccessShareLock);
}

/* The next unit; an open source holds no buffer lock between calls (a lock holds off interrupts, so parallel workers could not be launched or awaited) */
static bool
test_next(TestScan *ts, SiftColUnit *unit)
{
	bool		more;

	if (InterruptHoldoffCount != 0)
		elog(ERROR, "the column source holds a buffer lock between calls");
	more = sift_col_next(ts->src, unit);
	if (InterruptHoldoffCount != 0)
		elog(ERROR, "the column source holds a buffer lock after reading a unit");
	return more;
}

/* The visible rows of the unit that satisfy the query */
static bool
unit_select(TestScan *ts, const SiftColUnit *unit, int flags, SiftColSel *sel)
{
	bool		used;

	sift_col_sel_all(unit, sel);
	if (!sift_col_filter_points(unit, ts->q.top, ts->q.ntop, sel, &used))
		return false;
	for (int g = 0; g < ts->q.ngrp; g++)
		if (!sift_col_filter_group(unit, &ts->q.grp[g], sel))
			return false;
	return (flags & TEST_NOVET) ? true : sift_col_vet(unit, sel);
}

Datum
sift_test_col_tids(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	int			flags = PG_GETARG_INT32(2);
	TestScan	ts;
	SiftColUnit unit;

	InitMaterializedSRF(fcinfo, 0);
	scan_open(&ts, PG_GETARG_OID(0), text_to_cstring(PG_GETARG_TEXT_PP(1)), flags);
	while (test_next(&ts, &unit))
	{
		SiftColSel	sel;
		ItemPointerData *tids;

		if (!unit_select(&ts, &unit, flags, &sel))
			continue;
		tids = palloc(sel.count * sizeof(ItemPointerData));
		sift_col_tids(&unit, &sel, tids);
		for (uint32 i = 0; i < sel.count; i++)
		{
			Datum		v[2] = {ItemPointerGetDatum(&tids[i]), Int32GetDatum(unit.sb)};
			bool		nulls[2] = {false, false};

			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		}
		pfree(tids);
	}
	scan_close(&ts);
	return (Datum) 0;
}

/* A wide value from its lanes (vecs[0] is the head), as the text PostgreSQL prints for it */
static void
append_wide(StringInfo buf, const SiftColDef *def, const SiftColVec *vecs, uint32 k)
{
	Datum		out;

	if (def->type == SIFT_COLT_INTERVAL)
		out = DirectFunctionCall1(interval_out, IntervalPGetDatum(sift_col_lanes_interval(vecs[0].i[k], vecs[1].i[k], vecs[2].i[k])));
	else if (def->store == SIFT_COLS_STR)
		out = DirectFunctionCall1(numeric_out, sift_col_image_numeric((const uint8 *) SIFT_COL_DICT_STR(vecs[0].dict, vecs[0].codes[k]),
																	  SIFT_COL_DICT_LEN(vecs[0].dict, vecs[0].codes[k]), (int32) vecs[1].i[k]));
	else
		out = DirectFunctionCall1(numeric_out, sift_col_wide_numeric(def->scale, vecs[0].i[k], vecs[1].i[k]));
	appendStringInfoString(buf, DatumGetCString(out));
}

static void
append_value(StringInfo buf, const SiftColVec *vec, uint32 k)
{
	if (vec->nulls && (vec->nulls[k / 64] >> (k % 64) & 1))
	{
		appendStringInfoString(buf, "NULL");
		return;
	}
	switch (vec->store)
	{
		case SIFT_COLS_INT:
			appendStringInfo(buf, INT64_FORMAT, vec->i[k]);
			break;
		case SIFT_COLS_F4:
		case SIFT_COLS_F8:
			appendStringInfoString(buf, float8out_internal(vec->f[k]));
			break;
		default:
			appendBinaryStringInfo(buf, SIFT_COL_DICT_STR(vec->dict, vec->codes[k]), SIFT_COL_DICT_LEN(vec->dict, vec->codes[k]));
	}
}

/* (tid, text): the leaf's value of every visible row that satisfies the query; an element leaf as {e1,e2,...} */
Datum
sift_test_col_read(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	int			leaf = PG_GETARG_INT32(1);
	int			flags = PG_GETARG_INT32(3);
	TestScan	ts;
	SiftColUnit unit;

	InitMaterializedSRF(fcinfo, 0);
	scan_open(&ts, PG_GETARG_OID(0), text_to_cstring(PG_GETARG_TEXT_PP(2)), flags);
	while (test_next(&ts, &unit))
	{
		SiftColSel	sel;
		SiftColVec	vec;
		SiftColVec	lanes[SIFT_COL_MAX_LANES];
		const SiftColDef *def = sift_col_def(ts.src, leaf);
		ItemPointerData *tids;

		if (!unit_select(&ts, &unit, flags, &sel))
			continue;
		tids = palloc(sel.count * sizeof(ItemPointerData));
		sift_col_tids(&unit, &sel, tids);
		/*
		 * The selection's positions are cached for the leaves read through it: every other leaf is read first (the group offsets are
		 * built by a sibling), then a selection with one row less (which replaces the cache), then this one again, which must read as if
		 * it were the first.
		 */
		for (int c = 0; c < sift_col_ncols(ts.src); c++)
			if (c != leaf)
				sift_col_read(&unit, c, &sel, &vec);
		if (sel.count > 1)
		{
			SiftColSel	less = sel;
			uint32	   *first = palloc((sel.count + SIFT_COL_IDXPAD) * sizeof(uint32));

			less.bits = palloc(sel.nwords * sizeof(uint64));
			memcpy(less.bits, sel.bits, sel.nwords * sizeof(uint64));
			sift_col_sel_idx(&sel, first);
			sift_col_bit_clear(less.bits, first[0]);
			sift_col_sel_count(&less);
			sift_col_read(&unit, leaf, &less, &vec);
		}
		sift_col_read(&unit, leaf, &sel, &vec);
		if (def->lanes > 1)
			sift_col_read_lanes(&unit, leaf, &sel, lanes);
		for (uint32 r = 0; r < sel.count; r++)
		{
			Datum		v[2];
			bool		nulls[2] = {false, false};
			StringInfoData buf;

			initStringInfo(&buf);
			if (def->lanes > 1 && !(vec.nulls && (vec.nulls[r / 64] >> (r % 64) & 1)))
				append_wide(&buf, def, lanes, r);
			else if (vec.offsets)
			{
				appendStringInfoChar(&buf, '{');
				for (uint32 e = vec.offsets[r]; e < vec.offsets[r + 1]; e++)
				{
					if (e > vec.offsets[r])
						appendStringInfoChar(&buf, ',');
					append_value(&buf, &vec, e);
				}
				appendStringInfoChar(&buf, '}');
			}
			else if (vec.nulls && (vec.nulls[r / 64] >> (r % 64) & 1))
				nulls[1] = true;
			else
				append_value(&buf, &vec, r);
			v[0] = ItemPointerGetDatum(&tids[r]);
			v[1] = CStringGetTextDatum(buf.data);
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		}
	}
	scan_close(&ts);
	return (Datum) 0;
}

/* One row per candidate unit: the zone of the leaf and whether the unit answers from its zone alone */
Datum
sift_test_col_zones(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	int			leaf = PG_GETARG_INT32(1);
	TestScan	ts;
	SiftColUnit unit;

	InitMaterializedSRF(fcinfo, 0);
	scan_open(&ts, PG_GETARG_OID(0), text_to_cstring(PG_GETARG_TEXT_PP(2)), 0);
	while (test_next(&ts, &unit))
	{
		SiftColZone z;
		Datum		v[9];
		bool		nulls[9] = {false};

		sift_col_zone(&unit, leaf, &z);
		v[0] = Int32GetDatum(unit.sb);
		v[1] = BoolGetDatum(unit.in_wbuf);
		v[2] = Int32GetDatum(unit.nrows);
		v[3] = Int64GetDatum(z.nvals);
		v[4] = Int64GetDatum(z.nnull);
		v[5] = Int64GetDatum(z.min);
		v[6] = Int64GetDatum(z.max);
		v[7] = Int64GetDatum(z.sum_i);
		v[8] = BoolGetDatum(sift_col_unit_exact(&unit));
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	scan_close(&ts);
	return (Datum) 0;
}

/*
 * One row per candidate unit: the rows whose value of the leaf has a JSON type in tagmask (bit t for SIFT_COLTAG_t; deftag: the type of the
 * stored values, mixed: an astext leaf), whether the unit knows the type of every row, and how many values of the leaf overflowed
 */
Datum
sift_test_col_types(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	int			leaf = PG_GETARG_INT32(1);
	uint32		tagmask = PG_GETARG_INT32(2);
	int			deftag = PG_GETARG_INT32(3);
	bool		mixed = PG_GETARG_BOOL(4);
	TestScan	ts;
	SiftColUnit unit;

	InitMaterializedSRF(fcinfo, 0);
	scan_open(&ts, PG_GETARG_OID(0), "", 0);
	while (test_next(&ts, &unit))
	{
		SiftColSel	sel;
		uint64	   *ovf = palloc(SIFT_COL_NWORDS(unit.nrows) * sizeof(uint64));
		bool		known;
		Datum		v[6];
		bool		nulls[6] = {false};
		int64		novf = 0;

		sift_col_sel_all(&unit, &sel);
		sift_col_filter_types(&unit, leaf, tagmask, deftag, mixed, &sel, &known);
		sift_col_ovf_bits(&unit, leaf, ovf);
		for (uint32 w = 0; w < SIFT_COL_NWORDS(unit.nrows); w++)
			novf += pg_popcount64(ovf[w]);
		v[0] = Int32GetDatum(unit.sb);
		v[1] = BoolGetDatum(unit.in_wbuf);
		v[2] = Int32GetDatum(unit.nrows);
		v[3] = Int64GetDatum(sel.count);
		v[4] = BoolGetDatum(known);
		v[5] = Int64GetDatum(novf);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	scan_close(&ts);
	return (Datum) 0;
}

/* (reset bool) The dictionaries this backend parsed from segments and found in its cache since the last reset, and the bytes it keeps */
Datum
sift_test_col_dict_cache(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		v[3] = {Int64GetDatum(sift_cdict_cache_stats.loads), Int64GetDatum(sift_cdict_cache_stats.hits), Int64GetDatum(sift_cdict_cache_bytes())};
	bool		nulls[3] = {false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (PG_GETARG_BOOL(0))
		memset(&sift_cdict_cache_stats, 0, sizeof(sift_cdict_cache_stats));
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

/* The counters of a scan that selects the rows of the query */
Datum
sift_test_col_counters(PG_FUNCTION_ARGS)
{
	int			flags = PG_GETARG_INT32(2);
	TestScan	ts;
	SiftColUnit unit;
	TupleDesc	tupdesc;
	Datum		v[10];
	bool		nulls[10] = {false};
	const SiftColCounters *c;
	int64		rows = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	scan_open(&ts, PG_GETARG_OID(0), text_to_cstring(PG_GETARG_TEXT_PP(1)), flags);
	while (test_next(&ts, &unit))
	{
		SiftColSel	sel;

		if (unit_select(&ts, &unit, flags, &sel))
			rows += sel.count;
	}
	c = sift_col_counters(ts.src);
	v[0] = Int64GetDatum(rows);
	v[1] = Int64GetDatum(c->segments);
	v[2] = Int64GetDatum(c->wbuf_rows);
	v[3] = Int64GetDatum(c->units);
	v[4] = Int64GetDatum(c->units_pruned);
	v[5] = Int64GetDatum(c->rows_scanned);
	v[6] = Int64GetDatum(c->rows_vetted);
	v[7] = Int64GetDatum(c->heap_pages);
	v[8] = CStringGetTextDatum(sift_simd_level_name(c->simd));
	v[9] = Int64GetDatum(c->tests_made);
	scan_close(&ts);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

/* One row per leaf of an index or sift_table: the planner statistics its column source reports (agg/colsrc.h SiftSrcStats), 0 where unknown */
Datum
sift_test_col_leaf_stats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	int			nleaves = sift_colsrc_layout(rel)->nleaves;
	SiftSrcStats st;

	InitMaterializedSRF(fcinfo, 0);
	sift_colsrc_stats(rel, &st);
	for (int i = 0; i < nleaves; i++)
	{
		Datum		v[5] = {Int32GetDatum(i), Float8GetDatum(st.ndistinct[i]), Float8GetDatum(st.min[i]), Float8GetDatum(st.max[i]),
		Float8GetDatum(st.elems_per_row[i])};
		bool		nulls[5] = {false, false, false, false, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	relation_close(rel, AccessShareLock);
	return (Datum) 0;
}

/*
 * What a consumer of the unit histograms does (docs/COL-PREAGG.md section 4): for every unit that may answer, fold its groups of dimension
 * dim into one table keyed by value (a text group by its bytes, so units of different dictionaries meet), counts and the sum of measure meas
 * (-1: none); the others are counted as scanned. Returns the units, those that answered, the groups of the result, the rows they cover and
 * the sum of the measure over them (numeric): the benchmark of the metadata path without any SQL tuple per group.
 */
Datum
sift_test_col_pagg_fold(PG_FUNCTION_ARGS)
{
	int			dim = PG_GETARG_INT32(1),
				meas = PG_GETARG_INT32(2);
	TestScan	ts;
	SiftColUnit unit;
	TupleDesc	tupdesc;
	MemoryContext unitcx = AllocSetContextCreate(CurrentMemoryContext, "pagg fold", ALLOCSET_DEFAULT_SIZES);
	struct
	{
		uint64		id;
		int64		count;
		SiftCI128	sum;
	}		   *grp = palloc(SIFT_PAGG_CUBE_GROUPS * sizeof(*grp));
	int			ng = 0;
	int64		units = 0,
				answered = 0,
				rows = 0;
	SiftCI128	total = 0;
	Datum		v[5];
	bool		nulls[5] = {false};
	char		buf[64];

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	scan_open(&ts, PG_GETARG_OID(0), "", TEST_NOWBUF);
	while (test_next(&ts, &unit))
	{
		SiftCHistDim d;
		SiftColDict dict;
		int			slot;

		units++;
		MemoryContextReset(unitcx);
		if (!sift_col_unit_hist(&unit, dim, unitcx, &d, &dict))
			continue;
		answered++;
		slot = meas >= 0 ? sift_cpagg_meas_slot(&d, meas) : -1;
		for (uint32 g = 0; g < d.ngroups; g++)
		{
			uint64		id = (d.has_null && g == d.ngroups - 1) ? PG_UINT64_MAX :
				sift_col_rel(ts.index)->schema->cols[dim].store == SIFT_COLS_STR ?
				hash_bytes_extended((const unsigned char *) SIFT_COL_DICT_STR(&dict, (uint32) d.key[g]), SIFT_COL_DICT_LEN(&dict, (uint32) d.key[g]), 0) :
				(uint64) d.key[g];
			int			k = 0;

			while (k < ng && grp[k].id != id)
				k++;
			if (k == ng)
			{
				grp[ng].id = id;
				grp[ng].count = 0;
				grp[ng++].sum = 0;
			}
			grp[k].count += d.count[g];
			rows += d.count[g];
			if (slot >= 0 && d.sum[slot])
			{
				grp[k].sum += d.sum[slot][g];
				total += d.sum[slot][g];
			}
		}
	}
	scan_close(&ts);
	snprintf(buf, sizeof(buf), "%lld", (long long) total);
	v[0] = Int64GetDatum(units);
	v[1] = Int64GetDatum(answered);
	v[2] = Int32GetDatum(ng);
	v[3] = Int64GetDatum(rows);
	v[4] = DirectFunctionCall3(numeric_in, CStringGetDatum(buf), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

/* does a decoded cube keep what the decoder promises: min <= max in every group that has a value, and the groups of a calendar dim are the first days of cells of its grain, in ascending order */
static bool
cube_sound(const SiftColSchema *sc, const SiftCHist *h)
{
	for (int d = 0; d < h->ndims; d++)
	{
		const SiftCHistDim *hd = &h->dim[d];

		for (uint32 g = 0; hd->leaf >= SIFT_PAGG_CAL && g < hd->ngroups - (hd->has_null ? 1 : 0); g++)
			if (!sift_cpagg_cal_key_ok(sc->cols[hd->leaf % SIFT_PAGG_CAL].type, hd->leaf / SIFT_PAGG_CAL, hd->key[g]) || (g > 0 && hd->key[g] <= hd->key[g - 1]))
				return false;
		for (int m = 0; m < h->dim[d].nmeas; m++)
			for (uint32 g = 0; h->dim[d].min[m] != NULL && g < h->dim[d].ngroups; g++)
				if (h->dim[d].nn[m][g] > 0 && h->dim[d].min[m][g] > h->dim[d].max[m][g])
					return false;
	}
	return true;
}

/* does the census of the sidecar (sift_col_refresh) call a unit with these bytes odd (it must never read outside them either) */
static int
census_odd_units(const SiftColSchema *sc, const SiftCPaggSpec *spec, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen)
{
	SiftCPaggCensus c;

	memset(&c, 0, sizeof(c));
	sift_cpagg_census(sc, spec, spec->minmax, blob, len, eblob, elen, &c);
	return c.odd;
}

/*
 * One cube blob decoded as stored, cut at every length, and with every byte set to some other values (one at a time). A decode must never read
 * outside the blob (meaningful under SANITIZE=1) and must refuse what it cannot trust: a cut blob is always refused, and a damaged one that is
 * accepted still obeys min <= max and the cells of a calendar dim. The census that sift.col_refresh takes of the same bytes (spec given: a sift_col index) reads headers only: it
 * must stay inside them, and call every cut blob odd (census_cut_odd; the empty cut is no blob).
 */
void
sift_test_cube_damage(const SiftColSchema *sc, const SiftCPaggSpec *spec, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen,
					  MemoryContext cx, SiftTestDamage *r)
{
	static const uint8 vals[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFF};
	uint8	   *copy = palloc(len);
	SiftCHist	h;

	r->blobs++;
	MemoryContextReset(cx);
	if (sift_cpagg_decode(sc, blob, len, cx, &h))
	{
		r->intact++;
		r->minmax += h.dim[0].minmax;
		for (int d = 0; d < h.ndims; d++)
			if (h.dim[d].leaf >= SIFT_PAGG_CAL)
			{
				r->calendar++;
				break;
			}
	}
	for (uint32 l = 0; l < len; l++)
	{
		MemoryContextReset(cx);
		memcpy(copy, blob, l);
		r->cut++;
		r->cut_accepted += sift_cpagg_decode(sc, copy, l, cx, &h);
		if (spec)
			r->census_cut_odd += census_odd_units(sc, spec, copy, l, eblob, eblob ? elen : 0);
	}
	for (uint32 i = 0; i < len; i++)
		for (int k = 0; k < lengthof(vals); k++)
		{
			memcpy(copy, blob, len);
			if (copy[i] == vals[k])
				continue;
			copy[i] = vals[k];
			MemoryContextReset(cx);
			r->damaged++;
			if (spec)
				r->census_odd += census_odd_units(sc, spec, copy, len, eblob, eblob ? elen : 0);
			if (sift_cpagg_decode(sc, copy, len, cx, &h))
			{
				r->accepted++;
				r->unsound += !cube_sound(sc, &h);
			}
		}
	pfree(copy);
}

/* The rollup of the rows of a unit decoded as stored, cut at every length and with every byte set to other values: a cut is always refused, and the decoder never reads outside the blob */
static void
rows_damage(const SiftColSchema *sc, const uint8 *blob, uint32 len, MemoryContext cx, SiftTestDamage *r)
{
	static const uint8 vals[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFF};
	uint8	   *copy = palloc(len);
	SiftCRollup ro;

	r->blobs++;
	MemoryContextReset(cx);
	r->intact += sift_cpagg_rollup_decode(sc, blob, len, cx, &ro);
	for (uint32 l = 0; l < len; l++)
	{
		MemoryContextReset(cx);
		memcpy(copy, blob, l);
		r->cut++;
		r->cut_accepted += sift_cpagg_rollup_decode(sc, copy, l, cx, &ro);
	}
	for (uint32 i = 0; i < len; i++)
		for (int k = 0; k < lengthof(vals); k++)
		{
			memcpy(copy, blob, len);
			if (copy[i] == vals[k])
				continue;
			copy[i] = vals[k];
			MemoryContextReset(cx);
			r->damaged++;
			r->accepted += sift_cpagg_rollup_decode(sc, copy, len, cx, &ro);
		}
	pfree(copy);
}

/*
 * sift._test_col_cube_damage(index, units, rows): sift_test_cube_damage of the cube blob of the first units of the index (rows: rows_damage of their rollup of the rows)
 */
Datum
sift_test_col_cube_damage(PG_FUNCTION_ARGS)
{
	int			maxunits = PG_GETARG_INT32(1);
	bool		rows = PG_GETARG_BOOL(2);
	TestScan	ts;
	SiftColUnit unit;
	TupleDesc	tupdesc;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "cube damage", ALLOCSET_DEFAULT_SIZES);
	SiftTestDamage r = {0};
	Datum		v[10];
	bool		nulls[10] = {false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	scan_open(&ts, PG_GETARG_OID(0), "", TEST_NOWBUF);
	while (r.blobs < maxunits && test_next(&ts, &unit))
	{
		SiftColSegment *seg;
		uint32		u,
					len,
					elen;
		const uint8 *blob;

		if (!sift_col_unit_stored(&unit, &seg, &u) || (blob = rows ? sift_cseg_rollup(seg, u, -1, &len) : sift_cseg_cube(seg, u, &len)) == NULL)
			continue;
		if (rows)
			rows_damage(sift_col_rel(ts.index)->schema, blob, len, cx, &r);
		else
			sift_test_cube_damage(sift_col_rel(ts.index)->schema, &sift_col_rel(ts.index)->pagg, blob, len, sift_cseg_ecube(seg, u, &elen), elen, cx, &r);
	}
	scan_close(&ts);
	v[0] = Int64GetDatum(r.blobs);
	v[1] = Int64GetDatum(r.intact);
	v[2] = Int64GetDatum(r.minmax);
	v[3] = Int64GetDatum(r.cut);
	v[4] = Int64GetDatum(r.cut_accepted);
	v[5] = Int64GetDatum(r.damaged);
	v[6] = Int64GetDatum(r.accepted);
	v[7] = Int64GetDatum(r.unsound);
	v[8] = Int64GetDatum(r.census_cut_odd);
	v[9] = Int64GetDatum(r.census_odd);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

typedef enum DamageSection
{
	DMG_UNIT,
	DMG_CHUNK,
	DMG_SCHEMA
} DamageSection;

typedef struct Damage
{
	DamageSection section;
	const SiftColSchema *sc;
	MemoryContext cx;
	uint32		needed;			/* the bytes a read of the intact object touches: a cut below it must be refused */
	uint32		flags;			/* the chunk flags and (1 << 8 + enc) seen in the objects that parsed */
	bool		sound;			/* what a reader trusts after a parse still holds, and a refusal was a corruption error */
} Damage;

/* the kinds, nulls and overflow bitmaps of a parsed chunk, checked again from the bits: no kind outside its slots, none in two, none beyond n */
static bool
chunk_sound(const SiftCChunk *c)
{
	uint32		words = (c->n + 63) / 64;
	uint64		tail = c->n % 64 ? ~UINT64CONST(0) << (c->n % 64) : 0;

	for (uint32 w = 0; w < words; w++)
	{
		uint64		nul = c->nulls ? c->nulls[w] : 0,
					ovf = c->ovf ? c->ovf[w] : 0,
					seen = 0;

		if ((ovf & ~nul) || (w == words - 1 && ((nul | ovf) & tail)))
			return false;
		for (int tag = 1; tag < lengthof(c->kind); tag++)
		{
			uint64		m = c->kind[tag] ? c->kind[tag][w] : 0;

			if ((m & (seen | ovf)) || (tag == SIFT_COLTAG_JNULL ? (m & ~nul) : (m & nul)) || (w == words - 1 && (m & tail)))
				return false;
			seen |= m;
		}
	}
	return true;
}

/* A chunk the way a scan reads it: decoded, and its strings taken */
static void
chunk_read(Damage *d, const SiftCChunk *c)
{
	d->flags |= c->flags | 1U << (8 + c->enc);
	d->sound &= chunk_sound(c);
	if (c->n > (1U << 20))
		return;
	if (c->enc == SIFT_COLENC_STRS)
	{
		uint32	   *off;
		char	   *bytes;

		sift_cchunk_strs(c, CurrentMemoryContext, &off, &bytes);
	}
	else
		sift_cchunk_decode(c, palloc(sizeof(int64) * (c->n + SIFT_COL_PAD / 8)));
}

/* One stored object read as a scan reads it; false when it is refused, by a failed parse or by an error (a corruption one, or d->sound is false) */
static bool
damage_read(Damage *d, const uint8 *p, uint32 len, const SiftColSchema *orig)
{
	volatile bool ok = false;
	MemoryContext old;

	MemoryContextReset(d->cx);
	old = MemoryContextSwitchTo(d->cx);
	d->sound = true;
	PG_TRY();
	{
		SiftCChunk	c;

		switch (d->section)
		{
			case DMG_CHUNK:
				if ((ok = sift_cchunk_parse(p, len, &c)))
				{
					d->needed = (uint32) (c.payload + c.paylen - p);
					chunk_read(d, &c);
				}
				break;
			case DMG_UNIT:
				{
					SiftCUnitView v;

					if ((ok = sift_cunit_parse(p, len, d->sc, &v)))
					{
						SiftKey    *keys = palloc(sizeof(SiftKey) * (v.nrows + 1));

						d->needed = 0;
						for (int col = 0; col < d->sc->ncols; col++)
							if (sift_cunit_chunk(&v, col, &c))
							{
								d->needed = Max(d->needed, v.chunk_off[col] + v.chunk_len[col]);
								chunk_read(d, &c);
							}
						sift_cunit_keys(&v, 7, keys);
						for (uint32 r = 1; r < v.nrows; r++)
							d->sound &= keys[r - 1] < keys[r];
					}
				}
				break;
			case DMG_SCHEMA:
				{
					SiftColSchema *s = sift_cschema_parse(d->cx, p, len);

					if ((ok = s != NULL))
					{
						d->needed = len;
						/* the hash covers every field but the flags: an accepted schema is the stored one */
						for (int i = 0; i < s->ncols; i++)
						{
							const SiftColDef *a = &s->cols[i],
									   *b = &orig->cols[i];

							d->sound &= i < orig->ncols && a->id == b->id && a->parent == b->parent && a->kind == b->kind && a->store == b->store &&
								a->attno == b->attno && a->type == b->type && a->scale == b->scale && a->lanes == b->lanes && a->lane == b->lane &&
								strcmp(a->path, b->path) == 0;
						}
						d->sound &= s->ncols == orig->ncols;
					}
				}
				break;
		}
	}
	PG_CATCH();
	{
		ErrorData  *e;

		MemoryContextSwitchTo(d->cx);
		e = CopyErrorData();
		FlushErrorState();
		d->sound = e->sqlerrcode == ERRCODE_DATA_CORRUPTED || e->sqlerrcode == ERRCODE_INDEX_CORRUPTED || e->sqlerrcode == ERRCODE_FEATURE_NOT_SUPPORTED;
		ok = false;
	}
	PG_END_TRY();
	MemoryContextSwitchTo(old);
	return ok;
}

#define DAMAGE_POSITIONS	600	/* the cuts and the damaged bytes of an object: all of the first and last 64, then every nth */

/* The object intact, cut at the positions, and with a byte set to other values or a bit flipped at them; the tally is the columns of sift._test_col_damage */
static void
damage_object(Damage *d, const uint8 *blob, uint32 len, const SiftColSchema *orig, int64 *t)
{
	static const uint8 vals[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFF};
	uint32		stride = 1 + len / DAMAGE_POSITIONS,
				needed;
	uint8	   *copy = palloc0(len + SIFT_COL_PAD);

	enum {OBJECTS, INTACT, CUT, CUT_ACCEPTED, DAMAGED, ACCEPTED, UNSOUND};

	t[OBJECTS]++;
	memcpy(copy, blob, len);
	if (damage_read(d, copy, len, orig) && d->sound)
		t[INTACT]++;
	else
		t[UNSOUND]++;
	needed = d->needed;
	for (uint32 l = 0; l < len; l += (l < 64 || l + 64 >= len) ? 1 : stride)
	{
		memset(copy, 0, len + SIFT_COL_PAD);
		memcpy(copy, blob, l);
		t[CUT]++;
		if (damage_read(d, copy, l, orig))
		{
			t[CUT_ACCEPTED]++;
			t[UNSOUND] += l < needed || !d->sound;
		}
		else
			t[UNSOUND] += !d->sound;
	}
	for (uint32 i = 0; i < len; i += (i < 64 || i + 64 >= len) ? 1 : stride)
		for (int k = 0; k < lengthof(vals) + 2; k++)
		{
			uint8		v = k < lengthof(vals) ? vals[k] : blob[i] ^ (k == lengthof(vals) ? 0x01 : 0x80);

			if (v == blob[i])
				continue;
			memcpy(copy, blob, len);
			copy[i] = v;
			t[DAMAGED]++;
			if (damage_read(d, copy, len, orig))
			{
				t[ACCEPTED]++;
				t[UNSOUND] += !d->sound;
			}
			else
				t[UNSOUND] += !d->sound;
		}
	pfree(copy);
}

/*
 * sift._test_col_damage(index, section, units): the stored objects of a section ('unit' the unit record with its chunks, 'chunk' each chunk alone,
 * 'schema' the leaf list) of the first units of the index, read as a scan reads them, cut at the positions of damage_object and with bytes changed,
 * one at a time. A reader must never read outside the object (meaningful under SANITIZE=1), must refuse a cut one (below the bytes it needs) and
 * every refusal must be a corruption error (or the unsupported-feature one of an encoding this build does not know); what it accepts keeps what the format promises (the kinds of a chunk, the order of the keys of a unit,
 * the stored leaf list). flags: the chunk flags (1 nulls, 2 overflow, 4 tags, 8 kinds) and, from bit 8, the encodings seen in the intact objects.
 */
Datum
sift_test_col_damage(PG_FUNCTION_ARGS)
{
	char	   *section = text_to_cstring(PG_GETARG_TEXT_PP(1));
	int			maxunits = PG_GETARG_INT32(2);
	TestScan	ts;
	SiftColUnit unit;
	TupleDesc	tupdesc;
	Damage		d = {0};
	int64		t[7] = {0};
	Datum		v[8];
	bool		nulls[8] = {false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (strcmp(section, "unit") == 0)
		d.section = DMG_UNIT;
	else if (strcmp(section, "chunk") == 0)
		d.section = DMG_CHUNK;
	else if (strcmp(section, "schema") == 0)
		d.section = DMG_SCHEMA;
	else
		elog(ERROR, "sift test: unknown section \"%s\"", section);
	d.cx = AllocSetContextCreate(CurrentMemoryContext, "damage", ALLOCSET_DEFAULT_SIZES);
	scan_open(&ts, PG_GETARG_OID(0), "", TEST_NOWBUF);
	d.sc = sift_col_rel(ts.index)->schema;
	if (d.section == DMG_SCHEMA)
	{
		StringInfoData buf;

		initStringInfo(&buf);
		sift_cschema_serialize(d.sc, &buf);
		damage_object(&d, (const uint8 *) buf.data, buf.len, d.sc, t);
	}
	for (int64 units = 0; d.section != DMG_SCHEMA && units < maxunits && test_next(&ts, &unit);)
	{
		SiftColSegment *seg;
		uint32		u,
					len;
		const uint8 *rec;
		SiftCUnitView uv;

		if (!sift_col_unit_stored(&unit, &seg, &u))
			continue;
		units++;
		rec = sift_cseg_unit(seg, u, &len);
		if (d.section == DMG_UNIT)
			damage_object(&d, rec, len, NULL, t);
		else if (sift_cunit_parse(rec, len, d.sc, &uv))
			for (int col = 0; col < d.sc->ncols; col++)
				if (uv.chunk_len[col] > 0)
				{
					/* a chunk is 8-aligned in the record, as the reader's own parse needs it */
					uint8	   *c = palloc(uv.chunk_len[col] + SIFT_COL_PAD);

					memset(c + uv.chunk_len[col], 0, SIFT_COL_PAD);
					memcpy(c, rec + uv.chunk_off[col], uv.chunk_len[col]);
					damage_object(&d, c, uv.chunk_len[col], NULL, t);
					pfree(c);
				}
	}
	scan_close(&ts);
	for (int i = 0; i < 7; i++)
		v[i] = Int64GetDatum(t[i]);
	v[7] = Int32GetDatum((int32) d.flags);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

/* sift._test_col_pre_stats(index): the units that hold the histogram (with min and max) of a leaf, or the rollup of a list, as the planner counts them */
Datum
sift_test_col_pre_stats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	const SiftColPreStats *pre = sift_col_pre_stats(index);

	InitMaterializedSRF(fcinfo, 0);
	for (int c = 0; c < sift_col_index_defs(index)->ncols; c++)
	{
		Datum		v[5] = {Int32GetDatum(c), Float8GetDatum(pre->units), Float8GetDatum(pre->dim[c]), Float8GetDatum(pre->mm[c]), Float8GetDatum(pre->roll[c])};
		bool		nulls[5] = {false};

		if (pre->dim[c] > 0 || pre->roll[c] > 0)
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/*
 * sift._test_col_set_damage(index, units): the small-set summaries of the first units of the index, opened as stored, cut at every length, with
 * each byte set to some other values (one at a time) and with the fields of the header the decoder must check set by name (flag bits, kind, count,
 * width, probes, bits, order). A decode must never read outside the blob (meaningful under SANITIZE=1) and must refuse a cut one and each named
 * damage; a damaged blob it accepts is a valid summary of other keys, which the verifier's comparison with the chunk (sift_cset_matches) refuses.
 */
Datum
sift_test_col_set_damage(PG_FUNCTION_ARGS)
{
	static const uint8 vals[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFF};
	int			maxunits = PG_GETARG_INT32(1);
	TestScan	ts;
	SiftColUnit unit;
	TupleDesc	tupdesc;
	int64		blobs = 0,
				exact = 0,
				bloom = 0,
				intact = 0,
				matching = 0,
				cut = 0,
				cut_accepted = 0,
				named = 0,
				named_accepted = 0,
				damaged = 0,
				accepted = 0,
				missed = 0;
	Datum		v[12];
	bool		nulls[12] = {false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	scan_open(&ts, PG_GETARG_OID(0), "", TEST_NOWBUF);
	while (blobs < maxunits && test_next(&ts, &unit))
	{
		SiftColSegment *seg;
		uint32		u,
					len,
					rlen;
		const uint8 *rec;
		SiftCUnitView uv;
		const SiftColSchema *sc = sift_col_rel(ts.index)->schema;

		if (!sift_col_unit_stored(&unit, &seg, &u))
			continue;
		rec = sift_cseg_unit(seg, u, &rlen);
		if (!sift_cunit_parse(rec, rlen, sc, &uv))
			elog(ERROR, "unit does not parse");
		for (int c = 0; c < sc->ncols && blobs < maxunits; c++)
		{
			const uint8 *blob = sift_cseg_set(seg, c, u, &len);
			uint8	   *copy,
					   *orig;
			SiftCSet	s;
			SiftCChunk	ch;
			int64	   *keys;
			uint32		nd;
			SiftCPaggCol col;

			if (blob == NULL)
				continue;
			blobs++;
			orig = palloc(len);
			memcpy(orig, blob, len);		/* the next call on seg reuses the buffer */
			copy = palloc(len);
			if (!sift_cunit_chunk(&uv, c, &ch))
				elog(ERROR, "unit has no chunk");
			keys = palloc((Size) ch.n * sizeof(int64) + SIFT_COL_PAD);
			sift_cchunk_decode(&ch, keys);
			col = (SiftCPaggCol) {keys, ch.nulls, ch.ovf, ch.n, true};
			{
				int64	   *k2 = palloc(Max(ch.n, 1) * sizeof(int64));

				nd = sift_cset_keys(&col, k2);
				pfree(keys);
				keys = k2;
			}
			if (sift_cset_open(orig, len, &s))
			{
				intact++;
				exact += s.kind == SIFT_CSET_EXACT;
				bloom += s.kind == SIFT_CSET_BLOOM;
				matching += sift_cset_matches(&s, keys, nd);
			}
			for (uint32 l = 0; l < len; l++)
			{
				memcpy(copy, orig, l);
				cut++;
				cut_accepted += sift_cset_open(copy, l, &s);
			}
			for (uint32 i = 0; i < len; i++)
				for (int k = 0; k < lengthof(vals); k++)
				{
					memcpy(copy, orig, len);
					if (copy[i] == vals[k])
						continue;
					/* a list read as an exact summary of the same layout is one, and a correct one while it holds at most SIFT_CSET_LIMIT_MAX keys */
					if (i == 0 && orig[0] == SIFT_CSET_PLIST && vals[k] == SIFT_CSET_EXACT)
						continue;
					copy[i] = vals[k];
					damaged++;
					if (sift_cset_open(copy, len, &s))
					{
						accepted++;
						missed += sift_cset_matches(&s, keys, nd);
					}
				}
			/* the named damages of the header: flag bits, unknown kinds, counts, and (exact) width, order; (bloom) probes and bits */
			for (int f = 0; f < 11; f++)
			{
				uint32		n = 0;
				uint64		big = UINT64_MAX;

				memcpy(copy, orig, len);
				switch (f)
				{
					case 0: copy[2] = 1; break;							/* flag bits */
					case 1: copy[3] = 0x80; break;
					case 2: copy[0] = 0; break;							/* kind */
					case 3: copy[0] = 5; break;
					case 4: memcpy(copy + 4, &n, 4); break;				/* no keys */
					case 5: n = UINT32_MAX; memcpy(copy + 4, &n, 4); break;	/* a count that overflows 32 bits when multiplied */
					case 6: if (orig[0] == SIFT_CSET_PRES) continue; copy[1] = 0; break;							/* width or probes 0 */
					case 7: copy[1] = orig[0] == SIFT_CSET_EXACT || orig[0] == SIFT_CSET_PLIST ? 3 : 17; break;
					case 8: memcpy(copy + 8, &big, 8); break;			/* the bits of a filter; the base of an exact set (whose first offset then is not its smallest key) is not damage */
					case 9:
						if (orig[0] == SIFT_CSET_EXACT || orig[0] == SIFT_CSET_PLIST)	/* unsorted: the first two offsets swapped, the larger first */
						{
							if (orig[1] > 8 || len < SIFT_CSET_HDR + 2 * orig[1])
								continue;
							memcpy(copy + SIFT_CSET_HDR, orig + SIFT_CSET_HDR + orig[1], orig[1]);
							memcpy(copy + SIFT_CSET_HDR + orig[1], orig + SIFT_CSET_HDR, orig[1]);
						}
						else
							memset(copy + SIFT_CSET_HDR, 0, len - SIFT_CSET_HDR);	/* an empty filter */
						break;
					default:											/* one byte of the body gone */
						named++;
						named_accepted += sift_cset_open(copy, len - 1, &s);
						continue;
				}
				if (f == 8 && (orig[0] == SIFT_CSET_EXACT || orig[0] == SIFT_CSET_PLIST))
					continue;
				named++;
				named_accepted += sift_cset_open(copy, len, &s);
			}
			pfree(orig);
			pfree(copy);
			pfree(keys);
		}
	}
	scan_close(&ts);
	v[0] = Int64GetDatum(blobs);
	v[1] = Int64GetDatum(exact);
	v[2] = Int64GetDatum(bloom);
	v[3] = Int64GetDatum(intact);
	v[4] = Int64GetDatum(matching);
	v[5] = Int64GetDatum(cut);
	v[6] = Int64GetDatum(cut_accepted);
	v[7] = Int64GetDatum(named);
	v[8] = Int64GetDatum(named_accepted);
	v[9] = Int64GetDatum(damaged);
	v[10] = Int64GetDatum(accepted);
	v[11] = Int64GetDatum(missed);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

/*
 * sift._test_col_super_damage(index): every byte of the superunit block of every segment of the index, read as itself with a bit flipped (two flips), one
 * damage at a time, through a fresh open of the segment and the check of sift.col_verify. A damage must never read outside the block (meaningful under
 * SANITIZE=1) and every one must be refused, by the reader or by the check: `unnoticed` counts the ones that were not.
 */
Datum
sift_test_col_super_damage(PG_FUNCTION_ARGS)
{
	static const uint8 masks[] = {0x01, 0x80};
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TupleDesc	tupdesc;
	int64		supers = 0,
				damaged = 0,
				refused = 0;
	Datum		v[4];
	bool		nulls[4] = {false};
	MemoryContext old = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		uint64		off,
					len;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		if (!sift_cseg_test_super_span(seg, &off, &len))
		{
			sift_cseg_close(seg);
			continue;
		}
		sift_cseg_close(seg);
		supers++;
		for (uint64 b = 0; b < len; b++)
			for (int m = 0; m < lengthof(masks); m++)
			{
				MemoryContext cx = AllocSetContextCreate(old, "super damage", ALLOCSET_DEFAULT_SIZES);

				damaged++;
				BeginInternalSubTransaction(NULL);
				MemoryContextSwitchTo(cx);
				PG_TRY();
				{
					SiftColSegment *s2;

					sift_test_seg_flip_at = off + b;
					sift_test_seg_flip_mask = masks[m];
					s2 = sift_cseg_open(index, &snap->segs[i], cx);
					sift_cseg_verify_super(s2);
					sift_test_seg_flip_at = UINT64_MAX;
					sift_cseg_close(s2);
					MemoryContextSwitchTo(old);
					ReleaseCurrentSubTransaction();
					CurrentResourceOwner = owner;
				}
				PG_CATCH();
				{
					sift_test_seg_flip_at = UINT64_MAX;
					MemoryContextSwitchTo(old);
					FlushErrorState();
					RollbackAndReleaseCurrentSubTransaction();
					CurrentResourceOwner = owner;
					refused++;
				}
				PG_END_TRY();
				MemoryContextDelete(cx);
			}
	}
	index_close(index, AccessShareLock);
	v[0] = Int64GetDatum(supers);
	v[1] = Int64GetDatum(damaged);
	v[2] = Int64GetDatum(refused);
	v[3] = Int64GetDatum(damaged - refused);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

/*
 * sift._test_col_run_file_damage(index, stride): the bytes of the sorted runs of every segment of the index (all of the structure, every stride-th entry of
 * a run), read as themselves with a bit flipped (two flips), one damage at a time, through a fresh open of the segment and the check of sift.col_verify. A damage
 * must never read outside the section (meaningful under SANITIZE=1) and every one in a byte the reader or the check uses must be refused: `unnoticed`
 * counts the ones that were not. The padding between the blocks is not counted.
 */
static Datum
section_file_damage(PG_FUNCTION_ARGS, bool (*span) (SiftColSegment *, uint64 *, uint64 *, uint8 **))
{
	static const uint8 masks[] = {0x01, 0x80};
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	int32		stride = Max(PG_GETARG_INT32(1), 1);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	TupleDesc	tupdesc;
	int64		runs = 0,
				damaged = 0,
				refused = 0,
				unnoticed = 0;
	Datum		v[4];
	bool		nulls[4] = {false};
	MemoryContext old = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		uint64		off,
					len;
		uint8	   *used;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		if (!span(seg, &off, &len, &used))
		{
			sift_cseg_close(seg);
			continue;
		}
		sift_cseg_close(seg);
		runs++;
		for (uint64 b = 0; b < len; b++)
			for (int m = 0; m < lengthof(masks) && (used[b] == 2 || (used[b] == 1 && b % stride == 0)); m++)
			{
				MemoryContext cx = AllocSetContextCreate(old, "run file damage", ALLOCSET_DEFAULT_SIZES);

				damaged++;
				BeginInternalSubTransaction(NULL);
				MemoryContextSwitchTo(cx);
				PG_TRY();
				{
					SiftColSegment *s2;

					sift_test_seg_flip_at = off + b;
					sift_test_seg_flip_mask = masks[m];
					s2 = sift_cseg_open(index, &snap->segs[i], cx);
					sift_cseg_verify(s2);
					sift_test_seg_flip_at = UINT64_MAX;
					sift_cseg_close(s2);
					unnoticed += used[b] != 0;
					MemoryContextSwitchTo(old);
					ReleaseCurrentSubTransaction();
					CurrentResourceOwner = owner;
				}
				PG_CATCH();
				{
					sift_test_seg_flip_at = UINT64_MAX;
					MemoryContextSwitchTo(old);
					FlushErrorState();
					RollbackAndReleaseCurrentSubTransaction();
					CurrentResourceOwner = owner;
					refused++;
				}
				PG_END_TRY();
				MemoryContextDelete(cx);
			}
	}
	index_close(index, AccessShareLock);
	v[0] = Int64GetDatum(runs);
	v[1] = Int64GetDatum(damaged);
	v[2] = Int64GetDatum(refused);
	v[3] = Int64GetDatum(unnoticed);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

Datum
sift_test_col_run_file_damage(PG_FUNCTION_ARGS)
{
	return section_file_damage(fcinfo, sift_cseg_test_run_span);
}

/* sift._test_col_set_file_damage(index, stride): the same for the small-set summaries and the key presence */
Datum
sift_test_col_set_file_damage(PG_FUNCTION_ARGS)
{
	return section_file_damage(fcinfo, sift_cseg_test_set_span);
}

/* sift._test_col_point_file_damage(index, stride): the same for the point directories at the end of the dictionary section */
Datum
sift_test_col_point_file_damage(PG_FUNCTION_ARGS)
{
	return section_file_damage(fcinfo, sift_cseg_test_point_span);
}

/*
 * sift._test_col_run_damage(index, units): the sorted runs of the first units of the index, opened as stored, cut at every length, with each byte set
 * to some other values (one at a time) and with the fields of the header set by name. The header decoder must refuse a cut run and each named damage;
 * a damaged run whose header it accepts has its entries read (an ordinal that is no row of the unit is refused) and, if they are read, is another run
 * that the comparison with the chunk (what sift.col_verify does) refuses.
 */
Datum
sift_test_col_run_damage(PG_FUNCTION_ARGS)
{
	static const uint8 vals[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFF};
	int			maxunits = PG_GETARG_INT32(1);
	TestScan	ts;
	SiftColUnit unit;
	TupleDesc	tupdesc;
	int64		blobs = 0,
				ord = 0,
				ident = 0,
				intact = 0,
				matching = 0,
				cut = 0,
				cut_accepted = 0,
				named = 0,
				named_accepted = 0,
				damaged = 0,
				accepted = 0,
				missed = 0;
	Datum		v[12];
	bool		nulls[12] = {false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	scan_open(&ts, PG_GETARG_OID(0), "", TEST_NOWBUF);
	while (blobs < maxunits && test_next(&ts, &unit))
	{
		SiftColSegment *seg;
		uint32		u,
					rlen;
		const uint8 *rec;
		SiftCUnitView uv;
		const SiftColSchema *sc = sift_col_rel(ts.index)->schema;

		if (!sift_col_unit_stored(&unit, &seg, &u))
			continue;
		rec = sift_cseg_unit(seg, u, &rlen);
		if (!sift_cunit_parse(rec, rlen, sc, &uv))
			elog(ERROR, "unit does not parse");
		for (int c = 0; c < sc->ncols && blobs < maxunits; c++)
		{
			SiftCRun	r,
						r2;
			uint64		at;
			uint8	   *orig,
					   *copy;
			uint32		len,
					   *ords;
			SiftCChunk	ch;
			SiftCPaggCol col;
			int64	   *keys;

			if (!sift_cseg_run(seg, c, u, &r, &at))
				continue;
			blobs++;
			len = sift_crun_bytes(r.kind, r.n);
			orig = palloc(len);
			copy = palloc(len);
			ords = palloc(r.n * sizeof(uint32) + 1);
			sift_cseg_run_blob(seg, at, &r, orig);
			if (!sift_cunit_chunk(&uv, c, &ch))
				elog(ERROR, "unit has no chunk");
			keys = palloc((Size) ch.n * sizeof(int64) + SIFT_COL_PAD);
			sift_cchunk_decode(&ch, keys);
			col = (SiftCPaggCol) {keys, ch.nulls, ch.ovf, ch.n, true};
			ord += r.kind == SIFT_CRUN_ORD;
			ident += r.kind == SIFT_CRUN_IDENT;
			if (sift_crun_head(orig, len, r.n, &r2))
			{
				intact++;
				matching += sift_crun_matches(&sc->cols[c], &col, orig, len);
			}
			for (uint32 l = 0; l < len; l++)
			{
				cut++;
				cut_accepted += sift_crun_head(orig, l, r.n, &r2);
			}
			for (uint32 i = 0; i < len; i++)
				for (int k = 0; k < lengthof(vals); k++)
				{
					memcpy(copy, orig, len);
					if (copy[i] == vals[k])
						continue;
					copy[i] = vals[k];
					damaged++;
					if (sift_crun_head(copy, len, r.n, &r2) && sift_crun_entries(&r2, copy + SIFT_CRUN_HDR, 0, r2.n, ords))
					{
						accepted++;
						missed += sift_crun_matches(&sc->cols[c], &col, copy, len);
					}
				}
			/* the named damages of the header: flag bits, kind, the row count, counts that do not fit, the width */
			for (int f = 0; f < 8; f++)
			{
				uint32		n = r.n + 1,
							big = UINT32_MAX;

				memcpy(copy, orig, len);
				switch (f)
				{
					case 0: copy[2] = 1; break;
					case 1: copy[3] = 0x80; break;
					case 2: copy[0] = 0; break;
					case 3: copy[0] = 3; break;
					case 4: memcpy(copy + 4, &n, 4); break;
					case 5: memcpy(copy + 8, &big, 4); break;
					case 6: memcpy(copy + 12, &big, 4); break;
					default: copy[1] = copy[1] == 4 ? 1 : 4; break;
				}
				named++;
				named_accepted += sift_crun_head(copy, len, r.n, &r2);
			}
			pfree(orig);
			pfree(copy);
			pfree(ords);
			pfree(keys);
		}
	}
	scan_close(&ts);
	v[0] = Int64GetDatum(blobs);
	v[1] = Int64GetDatum(ord);
	v[2] = Int64GetDatum(ident);
	v[3] = Int64GetDatum(intact);
	v[4] = Int64GetDatum(matching);
	v[5] = Int64GetDatum(cut);
	v[6] = Int64GetDatum(cut_accepted);
	v[7] = Int64GetDatum(named);
	v[8] = Int64GetDatum(named_accepted);
	v[9] = Int64GetDatum(damaged);
	v[10] = Int64GetDatum(accepted);
	v[11] = Int64GetDatum(missed);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

/* sift._test_col_formats(index): the column format minor of every immutable segment, by generation */
Datum
sift_test_col_formats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		Datum		v[2] = {Int64GetDatum(snap->segs[i].gen), Int32GetDatum(snap->segs[i].format)};
		bool		nulls[2] = {false};

		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_col_zone_flags(index): the stored units, and how many of them have a zone of some leaf flagged SIFT_COLZ_JNULL (a flag of column format minor 6 and later) */
Datum
sift_test_col_zone_flags(PG_FUNCTION_ARGS)
{
	TestScan	ts;
	SiftColUnit unit;
	TupleDesc	tupdesc;
	int64		units = 0,
				jnull = 0;
	Datum		v[2];
	bool		nulls[2] = {false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	scan_open(&ts, PG_GETARG_OID(0), "", TEST_NOWBUF);
	while (test_next(&ts, &unit))
	{
		SiftColSegment *seg;
		uint32		u;
		bool		flagged = false;

		if (!sift_col_unit_stored(&unit, &seg, &u))
			continue;
		units++;
		for (int col = 0; col < sift_col_rel(ts.index)->schema->ncols; col++)
		{
			SiftCZone	z;

			sift_cseg_zone(seg, col, u, &z);
			flagged |= (z.flags & SIFT_COLZ_JNULL) != 0;
		}
		jnull += flagged;
	}
	scan_close(&ts);
	v[0] = Int64GetDatum(units);
	v[1] = Int64GetDatum(jnull);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

#endif							/* SIFT_TESTING */
