/*
 * col/test/query.c - a scan from SQL for the test hooks: the parsed predicates
 * the tids, the rows, the zones and the types of a scan
 * what a test asks a segment
 */
#include "postgres.h"

#ifdef SIFT_TESTING
#include "access/table.h"
#include "funcapi.h"
#include "storage/lock.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#include "utils/float.h"
#include "col/am.h"
#endif							/* SIFT_TESTING */
#include "col/test/test_int.h"

#ifdef SIFT_TESTING
PG_FUNCTION_INFO_V1(sift_test_col_tids);

PG_FUNCTION_INFO_V1(sift_test_col_read);

PG_FUNCTION_INFO_V1(sift_test_col_zones);

PG_FUNCTION_INFO_V1(sift_test_col_types);

#define TEST_NOVET		2

#define TEST_POINTS	8		/* open the source with SIFT_COLO_POINTS: a segment with a point directory answers an equality or a short IN list from it */

#define TEST_POINT_TIDS	16	/* with TEST_POINTS: a directory of heap addresses returns the addresses (sift_col_point_tids), listed first by sift._test_col_tids, and its units are not read */

#define TEST_POINT_LAZY	32	/* with TEST_POINT_TIDS: the addresses come as sift_col_point_next hands them over, in chunks of the size in the bits above 8 (128 when 0) */

#define TEST_WAIT		4	/* after the source is open, wait for advisory lock (WAIT_KEY, 1): lets a test run VACUUM or DML mid-scan */

#define WAIT_KEY		0x51F7

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
	if (sift_col_def_value_lanes(def) > 1)
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

void
ctest_scan_open(TestScan *ts, Oid indexoid, const char *preds, int flags)
{
	ts->index = index_open(indexoid, AccessShareLock);
	if (!sift_col_is_index(ts->index))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_col index", RelationGetRelationName(ts->index))));
	ts->heap = table_open(ts->index->rd_index->indrelid, AccessShareLock);
	ts->src = sift_col_open(ts->heap, ts->index, GetActiveSnapshot(), CurrentMemoryContext,
							((flags & TEST_NOWBUF) ? SIFT_COLO_NOWBUF : 0) | ((flags & TEST_POINTS) ? SIFT_COLO_POINTS : 0) | ((flags & TEST_POINT_TIDS) ? SIFT_COLO_POINT_TIDS : 0) | ((flags & TEST_POINT_LAZY) ? SIFT_COLO_POINT_LAZY : 0));
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

void
ctest_scan_close(TestScan *ts)
{
	sift_col_close(ts->src);
	table_close(ts->heap, AccessShareLock);
	index_close(ts->index, AccessShareLock);
}

/* The next unit; an open source holds no buffer lock between calls (a lock holds off interrupts, so parallel workers could not be launched or awaited) */
bool
ctest_test_next(TestScan *ts, SiftColUnit *unit)
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
bool
ctest_unit_select(TestScan *ts, const SiftColUnit *unit, int flags, SiftColSel *sel)
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

static void
put_tids(ReturnSetInfo *rsi, const SiftKey *keys, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
	{
		ItemPointerData tid;
		Datum		v[2];
		bool		nulls[2] = {false, false};

		sift_key_to_tid(keys[i], &tid);
		v[0] = ItemPointerGetDatum(&tid);
		v[1] = Int32GetDatum(ItemPointerGetBlockNumber(&tid) >> SIFT_SB_SHIFT);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
}

Datum
sift_test_col_tids(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	int			flags = PG_GETARG_INT32(2);
	TestScan	ts;
	SiftColUnit unit;

	InitMaterializedSRF(fcinfo, 0);
	ctest_scan_open(&ts, PG_GETARG_OID(0), text_to_cstring(PG_GETARG_TEXT_PP(1)), flags);
	{
		uint32		n,
					m,
					chunk = flags >> 8 ? flags >> 8 : 128;
		const SiftKey *keys = sift_col_point_tids(ts.src, &n);
		SiftKey    *lazy = palloc(chunk * sizeof(SiftKey));

		put_tids(rsi, keys, n);
		while ((flags & TEST_POINT_LAZY) && (m = sift_col_point_next(ts.src, lazy, chunk)) > 0)
			put_tids(rsi, lazy, m);
	}
	while (ctest_test_next(&ts, &unit))
	{
		SiftColSel	sel;
		ItemPointerData *tids;

		if (!ctest_unit_select(&ts, &unit, flags, &sel))
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
	ctest_scan_close(&ts);
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
		out = DirectFunctionCall1(numeric_out, sift_col_w128_numeric(sift_col_wide_join(vecs[0].i[k], vecs[1].i[k]), def->scale));
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
	ctest_scan_open(&ts, PG_GETARG_OID(0), text_to_cstring(PG_GETARG_TEXT_PP(2)), flags);
	while (ctest_test_next(&ts, &unit))
	{
		SiftColSel	sel;
		SiftColVec	vec;
		SiftColVec	lanes[SIFT_COL_MAX_LANES];
		const SiftColDef *def = sift_col_def(ts.src, leaf);
		ItemPointerData *tids;

		if (!ctest_unit_select(&ts, &unit, flags, &sel))
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
		if (sift_col_def_value_lanes(def) > 1)
			sift_col_read_lanes(&unit, leaf, &sel, lanes);
		for (uint32 r = 0; r < sel.count; r++)
		{
			Datum		v[2];
			bool		nulls[2] = {false, false};
			StringInfoData buf;

			initStringInfo(&buf);
			if (sift_col_def_value_lanes(def) > 1 && !(vec.nulls && (vec.nulls[r / 64] >> (r % 64) & 1)))
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
	ctest_scan_close(&ts);
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
	ctest_scan_open(&ts, PG_GETARG_OID(0), text_to_cstring(PG_GETARG_TEXT_PP(2)), 0);
	while (ctest_test_next(&ts, &unit))
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
	ctest_scan_close(&ts);
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
	ctest_scan_open(&ts, PG_GETARG_OID(0), "", 0);
	while (ctest_test_next(&ts, &unit))
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
	ctest_scan_close(&ts);
	return (Datum) 0;
}
#endif							/* SIFT_TESTING */
