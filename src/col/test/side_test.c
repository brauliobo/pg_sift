/*
 * col/test/side_test.c - test hooks of the scope sidecar (col/side.h): write it from a query of the pairs, and list the cells of every run
 */
/* check-memory: allow-allocs the arrays follow the pairs of one call and the runs of one segment, not the rows of a table */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "catalog/pg_type_d.h"
#include "access/table.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#include "bm25/maint.h"
#include "col/col.h"
#include "col/pagg.h"
#include "col/am.h"
#include "col/side.h"
#include "col/test/test_int.h"
#include "core/store.h"

PG_FUNCTION_INFO_V1(sift_test_side_fill);
PG_FUNCTION_INFO_V1(sift_test_side_cells);
PG_FUNCTION_INFO_V1(sift_test_side_agg);
PG_FUNCTION_INFO_V1(sift_test_side_valid);

static int
pair_cmp(const void *a, const void *b)
{
	const SiftScopePair *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return x->member < y->member ? -1 : x->member > y->member;
}

/* The pairs the query (key, member) returns, distinct, by key and then member */
static SiftScopePair *
read_pairs(const char *sql, MemoryContext cx, uint64 *n)
{
	SiftScopePair *p;
	uint64		k = 0;

	SPI_connect();
	if (SPI_execute(sql, true, 0) != SPI_OK_SELECT || SPI_tuptable->tupdesc->natts != 2)
		elog(ERROR, "the pairs query must return (key, member)");
	p = MemoryContextAlloc(cx, Max(SPI_processed, 1) * sizeof(SiftScopePair));
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;

		p[i].key = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull));
		p[i].member = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2, &isnull));
	}
	*n = SPI_processed;
	SPI_finish();
	qsort(p, *n, sizeof(SiftScopePair), pair_cmp);
	for (uint64 i = 0; i < *n; i++)
		if (k == 0 || pair_cmp(&p[k - 1], &p[i]) != 0)
			p[k++] = p[i];
	*n = k;
	return p;
}

/* sift._test_side_fill(index, scope, pairs): writes the scope sidecar of every entity segment of 16 units or more that has none; the counts */
Datum
sift_test_side_fill(PG_FUNCTION_ARGS)
{
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), RowExclusiveLock, "sift_col");
	uint64		npairs;
	SideScopeInput in = {.scope = PG_GETARG_INT32(1),.name = "test"};
	SideCounts	cnt = {0};
	int			done;
	TupleDesc	td;
	Datum		v[9];
	bool		nulls[9] = {false};

	if (sift_scope_take(index, in.scope, GetTransactionSnapshot(), in.stamp) != SIFT_CONN_OK)
		memset(in.stamp, 0, sizeof(in.stamp));
	in.pairs = read_pairs(text_to_cstring(PG_GETARG_TEXT_PP(2)), CurrentMemoryContext, &npairs);
	in.npairs = npairs;

	if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	sift_maint_lock(index, true);
	done = sift_col_side_fill(index, &in, 1, &cnt);
	sift_maint_unlock(index);
	index_close(index, RowExclusiveLock);
	v[0] = Int32GetDatum(done);
	v[1] = Int64GetDatum(cnt.keys);
	v[2] = Int64GetDatum(cnt.runs);
	v[3] = Int64GetDatum(cnt.units);
	v[4] = Int64GetDatum(cnt.impure_units);
	v[5] = Int64GetDatum(cnt.rows);
	v[6] = Int64GetDatum(cnt.cells);
	v[7] = Int64GetDatum(cnt.cell_bytes);
	v[8] = Int64GetDatum(cnt.bytes);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(td, v, nulls)));
}

/*
 * sift._test_side_cells(index, scope, dim_att, meas_att): one row for each group of the dim's histogram in each cell of each run of the sidecar of each entity segment:
 * (generation, key, run, units in the run, cell, dim value, rows, sum of the measure). dim_att and meas_att are attribute numbers (0: no measure)
 */
Datum
sift_test_side_cells(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);
	int			ord = PG_GETARG_INT32(1),
				dim = r->first[PG_GETARG_INT32(2)],
				meas = PG_GETARG_INT32(3) > 0 ? r->first[PG_GETARG_INT32(3)] : -1;
	SiftDirSnapshot *snap;

	InitMaterializedSRF(fcinfo, 0);
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		const SiftSide *sd;
		const SideScope *sc;
		const SideKey *recs;
		const int64 *keys;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE || snap->segs[i].format < SIFT_COL_FORMAT_SIDE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		sd = sift_cseg_side(seg);
		sc = sd ? sift_side_scope(sd, ord) : NULL;
		keys = sc ? sift_side_keys(seg, sd, sc, &recs) : NULL;
		for (uint32 k = 0; sc != NULL && k < sc->nkeys; k++)
		{
			uint32		nruns;
			const SideRun *runs = sift_side_runs(seg, sd, sc, keys[k], &nruns);

			for (uint32 ri = 0; ri < nruns; ri++)
			{
				const SideCell *cells = sift_side_run_cells(seg, sd, &runs[ri]);

				for (uint32 c = 0; c < runs[ri].ncells; c++)
				{
					const uint8 *blob = sift_side_cell_cube(seg, sd, &cells[c]);
					SiftCHist	h;

					if (cells[c].len == 0 || !sift_cpagg_decode(seg->schema, blob, cells[c].len, CurrentMemoryContext, &h))
						continue;
					for (int d = 0; d < h.ndims; d++)
					{
						int			m = -1;

						if (h.dim[d].leaf != dim)
							continue;
						for (int q = 0; q < h.dim[d].nmeas; q++)
							if (h.dim[d].meas[q] == meas)
								m = q;
						for (uint32 g = 0; g < h.dim[d].ngroups; g++)
						{
							Datum		v[8] = {Int64GetDatum(snap->segs[i].gen), Int64GetDatum(keys[k]), Int32GetDatum((int32) ri), Int32GetDatum((int32) runs[ri].nunits),
												Int64GetDatum(cells[c].cell), Int64GetDatum(h.dim[d].key[g]), Int64GetDatum(h.dim[d].count[g]), 0};
							bool		nulls[8] = {false, false, false, false, false, h.dim[d].has_null && g == h.dim[d].ngroups - 1, false, m < 0};

							if (m >= 0 && h.dim[d].sum[m] != NULL)
								v[7] = Int64GetDatum((int64) h.dim[d].sum[m][g]);
							else
								nulls[7] = true;
							tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
						}
					}
				}
			}
		}
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}
typedef struct AggGroup
{
	int64		key;
	int64		rows;
	int64		sum;
} AggGroup;

/* The ungrouped total of a measure: the rows with a value, their sum, minimum and maximum (the bounds of a month only when exact is false) */
typedef struct AggTotal
{
	int64		n;
	SiftCI128	sum;
	int64		min;
	int64		max;
	bool		exact;
} AggTotal;

typedef struct AggAcc
{
	AggGroup   *g;
	int			n;
	int			cap;
} AggAcc;

static void
acc_add(AggAcc *a, int64 key, int64 rows, int64 sum)
{
	int			i = 0;

	while (i < a->n && a->g[i].key != key)
		i++;
	if (i == a->n)
	{
		if (a->n == a->cap)
			a->g = repalloc(a->g, (a->cap *= 2) * sizeof(AggGroup));
		a->g[a->n++] = (AggGroup) {key, 0, 0};
	}
	a->g[i].rows += rows;
	a->g[i].sum += sum;
}

static bool
run_always(void *arg, SiftColSegment *seg, const SideRun *run)
{
	return true;
}

static void
total_add(AggTotal *t, int64 n, SiftCI128 sum, int64 min, int64 max, bool exact)
{
	t->n += n;
	t->sum += sum;
	if (n > 0)
	{
		t->min = Min(t->min, min);
		t->max = Max(t->max, max);
	}
	t->exact &= exact;
}

/* A unit read as the by-reference path reads it: the live rows whose member is in the key's, vetted, grouped by the dim */
static void
acc_unit(AggAcc *acc, const SiftColUnit *unit, const SiftColPred *preds, int npreds, int dim, int meas)
{
	SiftColSel	sel;
	SiftColVec	dv,
				mv;

	sift_col_sel_all(unit, &sel);
	if (!sift_col_filter(unit, preds, npreds, &sel) || !sift_col_vet(unit, &sel) || sel.count == 0)
		return;
	sift_col_read(unit, dim, &sel, &dv);
	if (meas >= 0)
		sift_col_read(unit, meas, &sel, &mv);
	for (uint32 i = 0; i < dv.n; i++)
		acc_add(acc, dv.i[i], 1, meas >= 0 ? mv.i[i] : 0);
}

/*
 * sift._test_side_agg(index, scope, key, pairs, dim_att, meas_att, mode): the rows and the sum of the measure by the dim of the rows of the members of the key, read as a scan reads them:
 * mode bit 0: the runs of the scope sidecar as superunits (answered from the cube of all their rows), the units of the others by reference; bit 1: with the filter amount >= 9900 (no run can answer);
 * bit 2: one member of the key only (a point); bit 3: with t_lo <= time < t_hi (a range of the time leaf, in its keys: microseconds since 2000-01-01); bit 5: the runs a journal check allows for the pairs as they are now (sift_side_run_valid; else every run); bit 4: the ungrouped total of the
 * measure (rows with a value, sum; tag 'g' with grp the minimum, and a row 'max'; min and max NULL when only bounds). One row for each group
 * (tag 'g', key, rows, sum) and the counts of the read (tag 'runs', 'run_units', 'split_runs', 'ref_units', 'mem_units')
 */
Datum
sift_test_side_agg(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);
	int			ord = PG_GETARG_INT32(1),
				member = r->first[r->schema->cols[r->scope_leaf[ord]].attno],
				dim = r->first[PG_GETARG_INT32(4)],
				meas = PG_GETARG_INT32(5) > 0 ? r->first[PG_GETARG_INT32(5)] : -1,
				mode = PG_GETARG_INT32(6);
	int64		key = PG_GETARG_INT64(2);
	uint64		npairs;
	SiftScopePair *pairs = read_pairs(text_to_cstring(PG_GETARG_TEXT_PP(3)), CurrentMemoryContext, &npairs);
	SiftColValue *vals = palloc(Max(npairs, 1) * sizeof(SiftColValue));
	int			nvals = 0;
	SiftColPred member_in,
				preds[4];
	int			npreds = 1;
	SiftColValue amount_min = {.v.i = PG_GETARG_INT64(7)},
				t_lo = {.v.i = PG_GETARG_INT64(8)},
				t_hi = {.v.i = PG_GETARG_INT64(9)};
	int			time = sift_scope_time_leaf(r->schema->cols, r->schema->ncols);
	AggAcc		acc = {palloc(1024 * sizeof(AggGroup)), 0, 1024};
	AggTotal	total = {0, 0, PG_INT64_MAX, PG_INT64_MIN, true};
	SiftColSource *src;
	SiftColUnit unit;
	int64		runs = 0,
				run_units = 0,
				split_runs = 0,
				ref_units = 0,
				mem_units = 0;
	const char *tags[] = {"runs", "run_units", "split_runs", "ref_units", "mem_units"};
	int64		counts[5];

	for (uint64 i = 0; i < npairs; i++)
		if (pairs[i].key == key)
			vals[nvals++] = (SiftColValue) {.v.i = pairs[i].member};
	member_in = (SiftColPred) {member, SIFT_COLOP_IN, (mode & 4) ? Min(nvals, 1) : nvals, vals};
	preds[0] = member_in;
	if (mode & 2)
		preds[npreds++] = (SiftColPred) {meas, SIFT_COLOP_GE, 1, &amount_min};
	if (mode & 8)
	{
		preds[npreds++] = (SiftColPred) {time, SIFT_COLOP_GE, 1, &t_lo};
		preds[npreds++] = (SiftColPred) {time, SIFT_COLOP_LT, 1, &t_hi};
	}
	src = sift_col_open(heap, index, GetActiveSnapshot(), CurrentMemoryContext, 0, NULL, NULL);
	if (mode & 1)
	{
		SiftColSuperTest tests[2] = {{meas, amount_min.v.i, PG_INT64_MAX, 0, NULL}, {time, t_lo.v.i, t_hi.v.i - 1, 0, NULL}};
		int			nt = (mode & 2) ? 1 : 0;

		if (mode & 8)
			tests[nt++] = tests[1];
		sift_col_super(src, true);
		sift_col_super_tests(src, tests, nt, dim, -1, 0);
		if (mode & 32)
			sift_col_side_read(src, ord, key, sift_side_run_valid, sift_side_valid_open(index, ord, GetActiveSnapshot(), NULL, &key, 1, CurrentMemoryContext));
		else
			sift_col_side_read(src, ord, key, run_always, NULL);
	}
	InitMaterializedSRF(fcinfo, 0);
	sift_col_begin(src, preds, npreds, 0, PG_UINT32_MAX);
	while (sift_col_next(src, &unit))
	{
		SiftColSegment *seg;
		const SideRun *run;
		const SideCell *cell;

		if (unit.nsu > 0 && sift_col_unit_run(&unit, &seg, &run, &cell) && (mode & 16))
		{
			SiftCAgg	a;

			if (sift_col_unit_agg(&unit, meas, &a))
			{
				total_add(&total, a.nvals - a.nnull, a.sum, a.min, a.max, !(a.flags & SIFT_CAGG_LOOSE));
				runs++;
				run_units += unit.nsu;
				continue;
			}
			sift_col_unit_split(src, &unit);
			split_runs++;
			continue;
		}
		if (unit.nsu > 0 && sift_col_unit_run(&unit, &seg, &run, &cell))
		{
			SiftCHistDim h;
			SiftColDict dict;
			int			m = -1;

			if (sift_col_unit_hist(&unit, dim, CurrentMemoryContext, NULL, &h, &dict))
			{
				for (int q = 0; q < h.nmeas; q++)
					if (h.meas[q] == meas)
						m = q;
				for (uint32 g = 0; g < h.ngroups; g++)
					acc_add(&acc, h.key[g], h.count[g], m >= 0 && h.sum[m] != NULL ? (int64) h.sum[m][g] : 0);
				runs++;
				run_units += unit.nsu;
				continue;
			}
			sift_col_unit_split(src, &unit);
			split_runs++;
			continue;
		}
		if (unit.nsu > 0)
		{
			sift_col_unit_split(src, &unit);
			continue;
		}
		if (unit.in_wbuf)
			mem_units++;
		else
			ref_units++;
		if (mode & 16)
		{
			SiftColSel	sel;
			SiftColVec	mv;

			sift_col_sel_all(&unit, &sel);
			if (!sift_col_filter(&unit, preds, npreds, &sel) || !sift_col_vet(&unit, &sel) || sel.count == 0)
				continue;
			sift_col_read(&unit, meas, &sel, &mv);
			for (uint32 i = 0; i < mv.n; i++)
				total_add(&total, 1, mv.i[i], mv.i[i], mv.i[i], true);
			continue;
		}
		acc_unit(&acc, &unit, preds, npreds, dim, meas);
	}
	sift_col_close(src);
	if (mode & 16)
	{
		Datum		v[4] = {CStringGetTextDatum("g"), Int64GetDatum(total.min), Int64GetDatum(total.n), Int64GetDatum((int64) total.sum)};
		bool		nulls[4] = {false, !total.exact || total.n == 0, false, false};
		Datum		w[4] = {CStringGetTextDatum("max"), Int64GetDatum(0), Int64GetDatum(total.max), Int64GetDatum(0)};
		bool		wnulls[4] = {false, false, !total.exact || total.n == 0, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, w, wnulls);
	}
	for (int i = 0; i < acc.n; i++)
	{
		Datum		v[4] = {CStringGetTextDatum("g"), Int64GetDatum(acc.g[i].key), Int64GetDatum(acc.g[i].rows), Int64GetDatum(acc.g[i].sum)};
		bool		nulls[4] = {false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	counts[0] = runs, counts[1] = run_units, counts[2] = split_runs, counts[3] = ref_units, counts[4] = mem_units;
	for (int i = 0; i < 5; i++)
	{
		Datum		v[4] = {CStringGetTextDatum(tags[i]), Int64GetDatum(0), Int64GetDatum(counts[i]), Int64GetDatum(0)};
		bool		nulls[4] = {false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	table_close(heap, AccessShareLock);
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* sift._test_side_valid(index, scope, key): for each run of the key in each entity segment with a sidecar: (generation, run, lo, hi, whether a read of the key under the current pairs may take it) */
Datum
sift_test_side_valid(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = sift_test_open_index_am(PG_GETARG_OID(0), AccessShareLock, "sift_col");
	int			ord = PG_GETARG_INT32(1);
	int64		key = PG_GETARG_INT64(2);
	SiftDirSnapshot *snap;
	SideValid  *sv = sift_side_valid_open(index, ord, GetActiveSnapshot(), NULL, &key, 1, CurrentMemoryContext);

	InitMaterializedSRF(fcinfo, 0);
	snap = sift_dir_read(index);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftColSegment *seg;
		const SiftSide *sd;
		const SideScope *sc;
		uint32		nruns;
		const SideRun *runs;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE || snap->segs[i].format < SIFT_COL_FORMAT_SIDE)
			continue;
		seg = sift_cseg_open(index, &snap->segs[i], CurrentMemoryContext);
		sd = sift_cseg_side(seg);
		sc = sd ? sift_side_scope(sd, ord) : NULL;
		runs = sc ? sift_side_runs(seg, sd, sc, key, &nruns) : NULL;
		for (uint32 r = 0; runs != NULL && r < nruns; r++)
		{
			Datum		v[5] = {Int64GetDatum(snap->segs[i].gen), Int32GetDatum((int32) r), Int64GetDatum(runs[r].lo), Int64GetDatum(runs[r].hi), BoolGetDatum(sift_side_run_valid(sv, seg, &runs[r]))};
			bool		nulls[5] = {false};

			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
		}
		sift_cseg_close(seg);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}
#endif
