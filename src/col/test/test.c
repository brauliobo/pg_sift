/*
 * col/test/test.c - the test hooks of the column index: formats, counters, dictionary tests
 * what a regression test reads from a segment
 * and the selectivity and order checks
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "funcapi.h"
#include "agg/agg.h"
#include "col/am.h"
#include "col/gorder.h"
#include "col/pagg.h"
#include "core/sql.h"
#endif							/* SIFT_TESTING */
#include "col/test/test_int.h"

#ifdef SIFT_TESTING
PG_FUNCTION_INFO_V1(sift_test_col_gorder);

PG_FUNCTION_INFO_V1(sift_test_col_unit_of_block);

PG_FUNCTION_INFO_V1(sift_test_col_formats);

PG_FUNCTION_INFO_V1(sift_test_col_dict_count);

PG_FUNCTION_INFO_V1(sift_test_eq_sel);

PG_FUNCTION_INFO_V1(sift_test_eq_sel_calls);

PG_FUNCTION_INFO_V1(sift_test_key_sorts);

PG_FUNCTION_INFO_V1(sift_test_source_lookups);

PG_FUNCTION_INFO_V1(sift_test_prepass_nodes);

PG_FUNCTION_INFO_V1(sift_test_agg_outer_walks);

PG_FUNCTION_INFO_V1(sift_test_col_dict_tsig);

PG_FUNCTION_INFO_V1(sift_test_col_zone_flags);

/* sift._test_col_formats(index): the column format minor of every immutable segment, by generation, and whether it holds the leaf statistics block */
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
		Datum		v[3] = {Int64GetDatum(snap->segs[i].gen), Int32GetDatum(snap->segs[i].format),
							BoolGetDatum((snap->segs[i].flags & SIFT_SEG_LSTATS) != 0)};
		bool		nulls[3] = {false};

		if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_col_dict_tsig(index, leaf, needle): for each immutable segment whether the dictionary of the leaf has trigram signatures, and how many of its blocks a substring test of needle reads */
Datum
sift_test_col_dict_tsig(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	int			leaf = PG_GETARG_INT32(1);
	text	   *needle = PG_GETARG_TEXT_PP(2);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		uint32		passed = 0,
					total = 0;
		bool		signed_;
		Datum		v[4];
		bool		nulls[4] = {false};

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		signed_ = sift_cseg_dict_tsig(seg, leaf, VARDATA_ANY(needle), VARSIZE_ANY_EXHDR(needle), &passed, &total);
		v[0] = Int64GetDatum(snap->segs[i].gen);
		v[1] = BoolGetDatum(signed_);
		v[2] = Int32GetDatum(passed);
		v[3] = Int32GetDatum(total);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_col_dict_count(index, leaf, op, value): for each immutable segment whether its dictionary of the leaf holds row counts and the rows it says pass op ('eq', 'lt', 'le', 'gt', 'ge', 'prefix') against value */
Datum
sift_test_col_dict_count(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	int			leaf = PG_GETARG_INT32(1);
	char	   *opname = text_to_cstring(PG_GETARG_TEXT_PP(2));
	text	   *val = PG_GETARG_TEXT_PP(3);
	SiftColValue c = {.len = VARSIZE_ANY_EXHDR(val), .v.s = VARDATA_ANY(val)};
	SiftColOp	op = strcmp(opname, "eq") == 0 ? SIFT_COLOP_EQ : strcmp(opname, "lt") == 0 ? SIFT_COLOP_LT : strcmp(opname, "le") == 0 ? SIFT_COLOP_LE :
		strcmp(opname, "gt") == 0 ? SIFT_COLOP_GT : strcmp(opname, "ge") == 0 ? SIFT_COLOP_GE : SIFT_COLOP_PREFIX;
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		uint64		rows = 0;
		bool		counted;
		Datum		v[3];
		bool		nulls[3] = {false};

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		counted = sift_cseg_dict_count(seg, leaf, op, &c, &rows);
		v[0] = Int64GetDatum(snap->segs[i].gen);
		v[1] = BoolGetDatum(counted);
		v[2] = Int64GetDatum(rows);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		sift_cseg_close(seg);
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
	int64		units = 0,
				jnull = 0;
	Datum		v[2];
	bool		nulls[2] = {false};

	ctest_scan_open(&ts, PG_GETARG_OID(0), "", TEST_NOWBUF);
	while (ctest_test_next(&ts, &unit))
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
	ctest_scan_close(&ts);
	v[0] = Int64GetDatum(units);
	v[1] = Int64GetDatum(jnull);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

/* sift._test_col_unit_of_block(index, blk): for every immutable segment, the unit that holds the rows of heap block blk: 'unit', 'none' (a segment cut by superblock with no row there) or 'unknown' (a segment cut by key) */
Datum
sift_test_col_unit_of_block(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	BlockNumber blk = (BlockNumber) PG_GETARG_INT64(1);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		Datum		v[4];
		bool		nulls[4] = {false};
		uint32		u;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		u = sift_cseg_unit_of_block(seg, blk);
		v[0] = Int32GetDatum(i);
		v[1] = BoolGetDatum(sift_cseg_keyed(&seg->entry));
		v[2] = Int64GetDatum(u);
		nulls[2] = u >= seg->nsb;
		v[3] = CStringGetTextDatum(u == SIFT_CUNIT_UNKNOWN ? "unknown" : u >= seg->nsb ? "none" : "unit");
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* (seg, pos, key, tid, null_row): the entries of the global order of leaf (by its column number) in every immutable segment that has one, in the order stored */
Datum
sift_test_col_gorder(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	int			leaf = PG_GETARG_INT32(1);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	int64		kv[SIFT_CGORD_CAP];
	uint64		av[SIFT_CGORD_CAP];

	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		uint32		entries,
					values;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		if (sift_cseg_gorder(seg, leaf, &entries, &values))
			for (uint32 c = 0, base = 0; base < entries; c++)
			{
				uint32		m = sift_cseg_gorder_chunk(seg, leaf, c, kv, av);

				for (uint32 k = 0; k < m; k++)
				{
					ItemPointerData tid;
					Datum		v[5];
					bool		nulls[5] = {false};
					uint32		pos = base + k;

					ItemPointerSet(&tid, (BlockNumber) (av[k] >> 16), (OffsetNumber) (av[k] & 0xFFFF));
					v[0] = Int64GetDatum(i);
					v[1] = Int32GetDatum(pos);
					v[2] = Int64GetDatum(kv[k]);
					v[3] = ItemPointerGetDatum(&tid);
					v[4] = BoolGetDatum(pos >= values);
					nulls[2] = pos >= values;
					tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
				}
				base += m;
			}
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* the restriction estimator of the operator sift._test_eq (= with a count): scalararraysel calls it once for each element each time it estimates the clause */
static int64 eq_sel_calls;

Datum
sift_test_eq_sel(PG_FUNCTION_ARGS)
{
	eq_sel_calls++;
	return DirectFunctionCall4Coll(eqsel, PG_GET_COLLATION(), PG_GETARG_DATUM(0), PG_GETARG_DATUM(1), PG_GETARG_DATUM(2), PG_GETARG_DATUM(3));
}

/* the calls since the last read; the argument true resets the count */
Datum
sift_test_eq_sel_calls(PG_FUNCTION_ARGS)
{
	int64		n = eq_sel_calls;

	if (PG_GETARG_BOOL(0))
		eq_sel_calls = 0;
	PG_RETURN_INT64(n);
}

/* the sorts of an IN list by the cost estimates since the last read; the argument true resets the count */
Datum
sift_test_key_sorts(PG_FUNCTION_ARGS)
{
	int64		n = sift_col_key_sorts;

	if (PG_GETARG_BOOL(0))
		sift_col_key_sorts = 0;
	PG_RETURN_INT64(n);
}

/* {the questions, the walks made to answer them} about references to a query above that the planner asked since the last read; the argument true resets the counts */
Datum
sift_test_agg_outer_walks(PG_FUNCTION_ARGS)
{
	Datum		d[2] = {Int64GetDatum(sift_agg_outer_asks), Int64GetDatum(sift_agg_outer_walks)};
	ArrayType  *a = construct_array_builtin(d, 2, INT8OID);

	if (PG_GETARG_BOOL(0))
		sift_agg_outer_asks = sift_agg_outer_walks = 0;
	PG_RETURN_ARRAYTYPE_P(a);
}

/* the asks of has_column_source that went to the catalog since the last read; the argument true resets the count */
Datum
sift_test_source_lookups(PG_FUNCTION_ARGS)
{
	int64		n = sift_source_lookups;

	if (PG_GETARG_BOOL(0))
		sift_source_lookups = 0;
	PG_RETURN_INT64(n);
}

/* the nodes the passes that rewrite a query before planning visited since the last read; the argument true resets the count */
Datum
sift_test_prepass_nodes(PG_FUNCTION_ARGS)
{
	int64		n = sift_prepass_nodes;

	if (PG_GETARG_BOOL(0))
		sift_prepass_nodes = 0;
	PG_RETURN_INT64(n);
}
#endif							/* SIFT_TESTING */
