/*
 * sift_col_test.c - sift._test_col_*: the column source API (sift_col.h section 3) from SQL, for the regression and differential tests.
 * Predicates are written "leaf:op:v1|v2;..." (ops eq ne lt le gt ge in notin null notnull prefix); "gN:leaf:op:v" is a same-element
 * predicate of group N, "gN!" requires that no element satisfies the group's predicates.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/relation.h"
#include "access/table.h"
#include "catalog/index.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lock.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/float.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "sift_col_am.h"
#include "sift_colsrc.h"
#include "sift_col_pagg.h"

PG_FUNCTION_INFO_V1(sift_test_col_tids);
PG_FUNCTION_INFO_V1(sift_test_col_read);
PG_FUNCTION_INFO_V1(sift_test_col_zones);
PG_FUNCTION_INFO_V1(sift_test_col_types);
PG_FUNCTION_INFO_V1(sift_test_col_formats);
PG_FUNCTION_INFO_V1(sift_test_col_counters);
PG_FUNCTION_INFO_V1(sift_test_col_leaf_stats);
PG_FUNCTION_INFO_V1(sift_test_col_pagg_fold);

#define TEST_NOWBUF	1
#define TEST_NOVET		2
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
							(flags & TEST_NOWBUF) ? SIFT_COLO_NOWBUF : 0);
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
	sift_col_sel_all(unit, sel);
	if (!sift_col_filter(unit, ts->q.top, ts->q.ntop, sel))
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

/* The counters of a scan that selects the rows of the query */
Datum
sift_test_col_counters(PG_FUNCTION_ARGS)
{
	int			flags = PG_GETARG_INT32(2);
	TestScan	ts;
	SiftColUnit unit;
	TupleDesc	tupdesc;
	Datum		v[9];
	bool		nulls[9] = {false};
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
	scan_close(&ts);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(tupdesc), v, nulls)));
}

/* One row per leaf of an index or sift_table: the planner statistics its column source reports (sift_colsrc.h SiftSrcStats), 0 where unknown */
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

#endif							/* SIFT_TESTING */
