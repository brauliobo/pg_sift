/*
 * col/test/cache.c - the caches and counters of the column index as the tests see them
 * dictionary cache, leaf statistics, the folded pre-aggregates
 * read only
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "access/relation.h"
#include "funcapi.h"
#include "col/am.h"
#include "col/dictshm.h"
#include "agg/colsrc.h"
#include "col/pagg.h"
#include "core/sql.h"
#endif							/* SIFT_TESTING */
#include "col/test/test_int.h"

#ifdef SIFT_TESTING
/*
 * col/test/test.c - sift._test_col_*: the column source API (col/col.h section 3) from SQL, for the regression and differential tests.
 * Predicates are written "leaf:op:v1|v2;..." (ops eq ne lt le gt ge in notin null notnull prefix); "gN:leaf:op:v" is a same-element
 * predicate of group N, "gN!" requires that no element satisfies the group's predicates.
 */

PG_FUNCTION_INFO_V1(sift_test_col_dict_cache);

PG_FUNCTION_INFO_V1(sift_test_col_dict_pinned);

PG_FUNCTION_INFO_V1(sift_test_col_counters);

PG_FUNCTION_INFO_V1(sift_test_col_leaf_stats);

PG_FUNCTION_INFO_V1(sift_test_col_pagg_fold);

/* (reset bool) The dictionaries and segment heads this backend parsed from segments and found in its cache since the last reset, and the bytes it keeps */
Datum
sift_test_col_dict_cache(PG_FUNCTION_ARGS)
{
	Datum		v[7] = {Int64GetDatum(sift_cdict_cache_stats.loads), Int64GetDatum(sift_cdict_cache_stats.hits), Int64GetDatum(sift_cdict_cache_bytes()),
						Int64GetDatum(sift_cdict_cache_stats.head_loads), Int64GetDatum(sift_cdict_cache_stats.head_hits),
						Int64GetDatum(sift_cdict_cache_stats.zone_loads), Int64GetDatum(sift_cdict_cache_stats.zone_hits)};
	bool		nulls[7] = {false};

	if (PG_GETARG_BOOL(0))
		memset(&sift_cdict_cache_stats, 0, sizeof(sift_cdict_cache_stats));
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

/* () The records of the shared dictionary ring that backends read in place, and the pins on them */
Datum
sift_test_col_dict_pinned(PG_FUNCTION_ARGS)
{
	int64		records,
				pins;
	Datum		v[2];
	bool		nulls[2] = {false};

	sift_dictshm_pinned(&records, &pins);
	v[0] = Int64GetDatum(records);
	v[1] = Int64GetDatum(pins);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

/* The counters of a scan that selects the rows of the query */
Datum
sift_test_col_counters(PG_FUNCTION_ARGS)
{
	int			flags = PG_GETARG_INT32(2);
	TestScan	ts;
	SiftColUnit unit;
	Datum		v[10];
	bool		nulls[10] = {false};
	const SiftColCounters *c;
	int64		rows = 0;

	ctest_scan_open(&ts, PG_GETARG_OID(0), text_to_cstring(PG_GETARG_TEXT_PP(1)), flags);
	while (ctest_test_next(&ts, &unit))
	{
		SiftColSel	sel;

		if (ctest_unit_select(&ts, &unit, flags, &sel))
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
	ctest_scan_close(&ts);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
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
		Datum		v[6] = {Int32GetDatum(i), Float8GetDatum(st.ndistinct[i]), Float8GetDatum(st.min[i]), Float8GetDatum(st.max[i]),
		Float8GetDatum(st.elems_per_row[i]), Float8GetDatum(st.allvisible_frac)};
		bool		nulls[6] = {false, false, false, false, false, false};

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
	bool		nulls[7] = {false};
	char		buf[64];

	ctest_scan_open(&ts, PG_GETARG_OID(0), "", TEST_NOWBUF);
	while (ctest_test_next(&ts, &unit))
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
	ctest_scan_close(&ts);
	snprintf(buf, sizeof(buf), "%lld", (long long) total);
	v[0] = Int64GetDatum(units);
	v[1] = Int64GetDatum(answered);
	v[2] = Int32GetDatum(ng);
	v[3] = Int64GetDatum(rows);
	v[4] = DirectFunctionCall3(numeric_in, CStringGetDatum(buf), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}
#endif							/* SIFT_TESTING */
