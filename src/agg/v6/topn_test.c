/*
 * agg/v6/topn_test.c - test-only SQL functions of the Top-N tournament (agg/topn_v6.h), compiled only with SIFT_TESTING and registered by the pg_sift_test
 * extension. sift._test_topn runs the tournament over the rows of arrays cut into granules, parts and partitions (and a slab of the last rows, which no zone
 * describes), with the order, offset, limit and ties given, with one or several participants that share the bound of the queue and take turns; it gives the
 * rows it outputs in order, and sift._test_topn_stats the counts of the units it did not read. The SQL test holds the key tuples of the rows to those of
 * PostgreSQL's own ORDER BY .. LIMIT over the same table.
 *
 * check-memory: allow-allocs the arrays follow the rows of the SQL arrays a test passes, a few thousand
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/tuplestore.h"

#include "agg/topn_v6.h"

PG_FUNCTION_INFO_V1(sift_test_topn);
PG_FUNCTION_INFO_V1(sift_test_topn_stats);
PG_FUNCTION_INFO_V1(sift_test_f64_order);

#define MAX_WORKERS 8

typedef struct Data
{
	int			nkeys;
	int			nrows;
	int			unit_rows;			/* rows covered by granules, the rest is the slab */
	int			granule_rows;
	int			granules_per_part;
	int			parts_per_partition;
	bool		frontier;
	Datum	   *key[SIFT_TOURN_MAX_KEYS];
	bool	   *key_null[SIFT_TOURN_MAX_KEYS];
	bool	   *keep;
	SiftTournSpec spec;
} Data;

static SiftTournStats last_stats;

static void
row_of(const Data *d, int r, SiftTopNWinRow *row)
{
	memset(row, 0, sizeof(*row));
	for (int j = 0; j < d->nkeys; j++)
	{
		row->nulls |= (uint8) (d->key_null[j][r] << j);
		row->key[j] = d->key_null[j][r] ? 0 : DatumGetInt64(d->key[j][r]);
	}
	row->tid = (uint64) r;
	row->part_slot = (uint32) (r / (d->granule_rows * d->granules_per_part));
	row->local_g = (uint16) (r / d->granule_rows);
	row->row_ord = (uint16) (r % d->granule_rows);
}

static SiftTournBound
bound_of(const Data *d, int j, int from, int to)
{
	SiftTournBound b = {.known = true, .all_null = true, .min = INT64_MAX, .max = INT64_MIN};

	for (int r = from; r < Min(to, d->unit_rows); r++)
	{
		if (d->key_null[j][r])
			b.has_null = true;
		else
		{
			b.all_null = false;
			b.min = Min(b.min, DatumGetInt64(d->key[j][r]));
			b.max = Max(b.max, DatumGetInt64(d->key[j][r]));
		}
	}
	return b;
}

static void
push_range(SiftTournament *t, const Data *d, SiftTournKind kind, int slot, int g, int idx, int from, int to)
{
	const SiftTournBound b0 = bound_of(d, 0, from, to);
	const SiftTournBound b1 = d->nkeys > 1 ? bound_of(d, 1, from, to) : (SiftTournBound) {0};

	sift_tourn_push(t, kind, (uint32) slot, (uint32) g, (uint32) idx, &b0, d->nkeys > 1 ? &b1 : NULL);
}

static void
push_granule(SiftTournament *t, const Data *d, SiftTournKind kind, int part, int g)
{
	push_range(t, d, kind, part, g, 0, g * d->granule_rows, (g + 1) * d->granule_rows);
}

static void
test_expand(void *src, SiftTournament *t, const SiftTopNSchedTok *tok)
{
	const Data *d = src;
	const int	part_rows = d->granule_rows * d->granules_per_part;

	if (tok->kind == STOK_PARTITION)
	{
		for (int q = 0; q < d->parts_per_partition; q++)
		{
			const int	part = (int) tok->part_slot * d->parts_per_partition + q;

			if (part * part_rows < d->unit_rows)
				push_range(t, d, STOK_PART, part, 0, 0, part * part_rows, (part + 1) * part_rows);
		}
		return;
	}
	{
		const int	first = (int) tok->part_slot * d->granules_per_part;
		const int	last = Min(first + d->granules_per_part, (d->unit_rows + d->granule_rows - 1) / d->granule_rows) - 1;

		if (d->frontier)
		{
			const int	g = d->spec.keys[0].desc ? last : first;

			push_granule(t, d, STOK_KEY_FRONTIER, (int) tok->part_slot, g);
		}
		else
			for (int g = first; g <= last; g++)
				push_granule(t, d, STOK_GRANULE, (int) tok->part_slot, g);
	}
}

static void
test_frontier_next(void *src, SiftTournament *t, const SiftTopNSchedTok *tok)
{
	const Data *d = src;
	const int	first = (int) tok->part_slot * d->granules_per_part;
	const int	last = Min(first + d->granules_per_part, (d->unit_rows + d->granule_rows - 1) / d->granule_rows) - 1;
	const int	next = (int) tok->local_g + (d->spec.keys[0].desc ? -1 : 1);

	if (next >= first && next <= last)
		push_granule(t, d, STOK_KEY_FRONTIER, (int) tok->part_slot, next);
}

static void
test_scan(void *src, SiftTournament *t, const SiftTopNSchedTok *tok)
{
	const Data *d = src;
	int			from = tok->kind == STOK_L0_SLAB ? d->unit_rows : (int) tok->local_g * d->granule_rows;
	const int	to = tok->kind == STOK_L0_SLAB ? d->nrows : Min(from + d->granule_rows, d->unit_rows);

	for (; from < to; from++)
	{
		SiftTopNWinRow row;

		if (!d->keep[from])
			continue;
		row_of(d, from, &row);
		sift_tourn_offer(t, &row);
	}
}

static const SiftTournOps test_ops = {test_expand, test_frontier_next, test_scan};

static void
load_keys(Data *d, int j, ArrayType *a)
{
	int			n;

	deconstruct_array(a, INT8OID, 8, true, 'd', &d->key[j], &d->key_null[j], &n);
	if (n != d->nrows)
		ereport(ERROR, (errmsg("the key arrays differ in length")));
}

static void
add_stats(SiftTournStats *to, const SiftTournStats *from)
{
	to->partitions_pruned += from->partitions_pruned;
	to->parts_pruned += from->parts_pruned;
	to->granules_pruned += from->granules_pruned;
	to->granules_scanned += from->granules_scanned;
	to->early_terminated |= from->early_terminated;
}

/*
 * sift._test_topn(k0 int8[], k1 int8[], k2 int8[], keep bool[], descs bool[], nulls_firsts bool[], granule_rows int, granules_per_part int,
 * parts_per_partition int, slab_rows int, lim int8, off int8, with_ties bool, workers int, frontier bool) RETURNS TABLE(rank int, idx int)
 */
Datum
sift_test_topn(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Data	   *d = palloc0(sizeof(Data));
	Datum	   *kd,
			   *dd,
			   *nd;
	bool	   *kn,
			   *dn,
			   *nn;
	int			n,
				workers = PG_GETARG_INT32(13);
	int			nparts;
	SiftTournament *t[MAX_WORKERS];
	SiftMorselQueue *q;
	bool		alive[MAX_WORKERS];
	uint32		one = 1,
				nres;
	const SiftTopNWinRow *res;

	InitMaterializedSRF(fcinfo, 0);
	if (workers < 1 || workers > MAX_WORKERS)
		ereport(ERROR, (errmsg("1 to %d workers", MAX_WORKERS)));
	for (int j = 0; j < 3; j++)
		if (!PG_ARGISNULL(j))
		{
			if (j == 0)
				d->nrows = ArrayGetNItems(ARR_NDIM(PG_GETARG_ARRAYTYPE_P(0)), ARR_DIMS(PG_GETARG_ARRAYTYPE_P(0)));
			load_keys(d, d->nkeys++, PG_GETARG_ARRAYTYPE_P(j));
		}
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(3), BOOLOID, 1, true, 'c', &kd, &kn, &n);
	d->keep = palloc(sizeof(bool) * d->nrows);
	for (int i = 0; i < d->nrows; i++)
		d->keep[i] = DatumGetBool(kd[i]);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(4), BOOLOID, 1, true, 'c', &dd, &dn, &n);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(5), BOOLOID, 1, true, 'c', &nd, &nn, &n);
	d->granule_rows = PG_GETARG_INT32(6);
	d->granules_per_part = PG_GETARG_INT32(7);
	d->parts_per_partition = PG_GETARG_INT32(8);
	d->unit_rows = d->nrows - PG_GETARG_INT32(9);
	d->frontier = PG_GETARG_BOOL(14);
	d->spec.nkeys = (uint8) d->nkeys;
	for (int j = 0; j < d->nkeys; j++)
	{
		d->spec.keys[j].desc = DatumGetBool(dd[j]);
		d->spec.keys[j].nulls_first = DatumGetBool(nd[j]);
	}
	d->spec.limit = (uint64) PG_GETARG_INT64(10);
	d->spec.offset = (uint64) PG_GETARG_INT64(11);
	d->spec.with_ties = PG_GETARG_BOOL(12);

	q = palloc_aligned(sift_morsel_size(1), SIFT_PAR_SHM_ALIGN, MCXT_ALLOC_ZERO);
	sift_morsel_init(q, 1, &one, (uint32) workers);
	for (int w = 0; w < workers; w++)
	{
		t[w] = sift_tourn_create(CurrentMemoryContext, &d->spec, UINT64CONST(1) << 30, workers > 1 ? q : NULL);
		alive[w] = true;
	}
	nparts = (d->unit_rows + d->granule_rows * d->granules_per_part * d->parts_per_partition - 1) / (d->granule_rows * d->granules_per_part * d->parts_per_partition);
	for (int p = 0; p < nparts; p++)
		push_range(t[p % workers], d, STOK_PARTITION, p, 0, 0, p * d->granule_rows * d->granules_per_part * d->parts_per_partition,
				   (p + 1) * d->granule_rows * d->granules_per_part * d->parts_per_partition);
	if (d->unit_rows < d->nrows)
		sift_tourn_push(t[0], STOK_L0_SLAB, 0, 0, 0, NULL, NULL);
	for (bool any = true; any;)
	{
		any = false;
		for (int w = 0; w < workers; w++)
			if (alive[w])
				any |= alive[w] = sift_tourn_step(t[w], &test_ops, d);
	}
	for (int w = 1; w < workers; w++)
		sift_tourn_merge(t[0], t[w]);
	memset(&last_stats, 0, sizeof(last_stats));
	for (int w = 0; w < workers; w++)
		add_stats(&last_stats, sift_tourn_stats(t[w]));
	res = sift_tourn_result(t[0], &nres);
	for (uint32 i = 0; i < nres; i++)
	{
		Datum		row[2] = {Int32GetDatum((int32) i + 1), Int32GetDatum((int32) res[i].tid + 1)};
		bool		nulls[2] = {false, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, row, nulls);
	}
	for (int w = 0; w < workers; w++)
		sift_tourn_free(t[w]);
	return (Datum) 0;
}

/* sift._test_topn_stats() RETURNS TABLE(partitions_pruned int8, parts_pruned int8, granules_pruned int8, granules_scanned int8, early_terminated bool) */
Datum
sift_test_topn_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		v[5];
	bool		nulls[5] = {false, false, false, false, false};

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	v[0] = Int64GetDatum((int64) last_stats.partitions_pruned);
	v[1] = Int64GetDatum((int64) last_stats.parts_pruned);
	v[2] = Int64GetDatum((int64) last_stats.granules_pruned);
	v[3] = Int64GetDatum((int64) last_stats.granules_scanned);
	v[4] = BoolGetDatum(last_stats.early_terminated);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, v, nulls)));
}

Datum
sift_test_f64_order(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(sift_f64_order_i64(PG_GETARG_FLOAT8(0)));
}

#endif							/* SIFT_TESTING */
