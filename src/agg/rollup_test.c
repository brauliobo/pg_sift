/*
 * agg/rollup_test.c - test-only SQL function of the grouping sets executor (agg/group/rollup.h), compiled only with SIFT_TESTING and registered by the
 * pg_sift_test extension. sift._test_rollup feeds three key columns and two value columns (arrays, NULLs allowed) in batches to the sets given as bit
 * masks of the keys, by the leaf sets or by every set, and returns the groups of every set in the columns of the SQL test's GROUPING SETS query, so that
 * the two can be compared with EXCEPT ALL.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "access/htup_details.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/numeric.h"

#include "agg/group/rollup.h"
#include "core/memtrack.h"

PG_FUNCTION_INFO_V1(sift_test_rollup);
PG_FUNCTION_INFO_V1(sift_test_rollup_distinct);

typedef struct Out
{
	ReturnSetInfo *rsi;
} Out;

static Datum
i128_numeric(SiftI128 v)
{
	char		digits[48];
	int			n = 0;
	const bool	neg = v.hi < 0;
	unsigned __int128 x = ((unsigned __int128) (uint64) v.hi << 64) | v.lo;
	char		text[52],
			   *o = text;

	if (neg)
		x = -x;
	do
	{
		digits[n++] = (char) ('0' + (int) (x % 10));
		x /= 10;
	} while (x);
	if (neg)
		*o++ = '-';
	while (n)
		*o++ = digits[--n];
	*o = 0;
	return DirectFunctionCall3(numeric_in, CStringGetDatum(text), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

#define NCOLS	15

static void
emit_group(void *arg, const SiftRollupGroup *g)
{
	Out		   *o = arg;
	Datum		row[NCOLS];
	bool		nulls[NCOLS] = {false};
	int			grouping = 0;

	for (int k = 0; k < 3; k++)
		grouping |= (int) ((g->omitted >> k) & 1) << (2 - k);
	row[0] = Int32GetDatum(g->set);
	row[1] = Int32GetDatum(grouping);
	for (int k = 0; k < 3; k++)
	{
		row[2 + k] = Int64GetDatum(g->key[k]);
		nulls[2 + k] = (g->key_nulls >> k) & 1;
	}
	row[5] = Int64GetDatum(g->rows);
	for (int v = 0; v < 2; v++)
	{
		const int	c = 6 + v * 4;

		row[c] = Int64GetDatum(g->val[v].nvals);
		row[c + 1] = i128_numeric(g->val[v].sum);
		row[c + 2] = Int64GetDatum(g->val[v].min);
		row[c + 3] = Int64GetDatum(g->val[v].max);
		nulls[c + 1] = nulls[c + 2] = nulls[c + 3] = g->val[v].nvals == 0;
	}
	row[14] = Int64GetDatum((int64) g->distinct);
	tuplestore_putvalues(o->rsi->setResult, o->rsi->setDesc, row, nulls);
}

static void
fill(SiftVec *v, int64 *data, uint64 *valid, const Datum *d, const bool *nl, int from, int n)
{
	memset(v, 0, sizeof(*v));
	memset(valid, 0, SIFT_VEC_WORDS * 8);
	v->data = data;
	v->valid = valid;
	v->len = (uint16) n;
	v->dense = true;
	v->type = SVEC_I64;
	for (int i = 0; i < n; i++)
	{
		data[i] = nl[from + i] ? 0 : DatumGetInt64(d[from + i]);
		valid[i >> 6] |= (uint64) !nl[from + i] << (i & 63);
	}
}

/*
 * sift._test_rollup(k1 int8[], k2 int8[], k3 int8[], v1 int8[], v2 int8[], masks int8[], prehash bool, batch int4, with_distinct bool, act bool[])
 * RETURNS TABLE(set_idx int4, gset int4, k1 int8, k2 int8, k3 int8, n int8, c1 int8, s1 numeric, mn1 int8, mx1 int8, c2 int8, s2 numeric, mn2 int8,
 * mx2 int8, dcnt int8)
 */
Datum
sift_test_rollup(PG_FUNCTION_ARGS)
{
	Out			out = {(ReturnSetInfo *) fcinfo->resultinfo};
	Datum	   *col[5],
			   *md,
			   *ad = NULL;
	bool	   *nul[5],
			   *mn,
			   *an = NULL;
	int			cnt[5],
				nm,
				na = 0;
	SiftRollupSpec spec;
	SiftRollupTree *t;
	SiftRollupBatch b;
	SiftVec		vec[5];
	int64	   *data[5];
	uint64		valid[5][SIFT_VEC_WORDS],
				mask[SIFT_VEC_WORDS];
	const int	batch = PG_ARGISNULL(7) ? SIFT_VEC_LEN : Min(PG_GETARG_INT32(7), SIFT_VEC_LEN);

	sift_aggkern6_init();
	InitMaterializedSRF(fcinfo, 0);
	for (int c = 0; c < 5; c++)
		deconstruct_array(PG_GETARG_ARRAYTYPE_P(c), INT8OID, 8, true, 'd', &col[c], &nul[c], &cnt[c]);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(5), INT8OID, 8, true, 'd', &md, &mn, &nm);
	if (!PG_ARGISNULL(9))
		deconstruct_array(PG_GETARG_ARRAYTYPE_P(9), BOOLOID, 1, true, 'c', &ad, &an, &na);
	memset(&spec, 0, sizeof(spec));
	spec.n_keys = 3;
	spec.n_vals = 2;
	spec.n_sets = (uint16) nm;
	for (int s = 0; s < nm; s++)
		spec.set_masks[s] = (uint64) DatumGetInt64(md[s]);
	spec.force_prehash = PG_GETARG_BOOL(6);
	spec.count_distinct = !PG_ARGISNULL(8) && PG_GETARG_BOOL(8);
	t = sift_rollup_create(CurrentMemoryContext, &spec, (uint64) work_mem * 1024);
	for (int c = 0; c < 5; c++)
		data[c] = palloc_aligned(SIFT_VEC_LEN * 8 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	for (int from = 0; from < cnt[0]; from += batch)
	{
		const int	n = Min(batch, cnt[0] - from);

		memset(&b, 0, sizeof(b));
		b.n = (uint16) n;
		for (int c = 0; c < 5; c++)
			fill(&vec[c], data[c], valid[c], col[c], nul[c], from, n);
		for (int k = 0; k < 3; k++)
			b.keys[k] = &vec[k];
		for (int v = 0; v < 2; v++)
			b.vals[v] = &vec[3 + v];
		if (ad)
		{
			memset(mask, 0, sizeof(mask));
			for (int i = 0; i < n; i++)
				mask[i >> 6] |= (uint64) (!an[from + i] && DatumGetBool(ad[from + i])) << (i & 63);
			b.mask = mask;
		}
		sift_rollup_feed(t, &b);
	}
	sift_rollup_finish(t);
	sift_rollup_emit(t, emit_group, &out);
	sift_rollup_free(t);
	return (Datum) 0;
}

static void
discard_group(void *arg, const SiftRollupGroup *g)
{
}

static void
feed_distinct(SiftRollupTree *t, int64 n)
{
	int64	   *keys = palloc_aligned(SIFT_VEC_LEN * 8 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO),
			   *vals = palloc_aligned(SIFT_VEC_LEN * 8 + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	SiftVec		kv = {.type = SVEC_I64, .dense = true, .all_valid = true, .data = keys},
				vv = {.type = SVEC_I64, .dense = true, .all_valid = true, .data = vals};
	SiftRollupBatch b = {.keys = {&kv}, .vals = {&vv}};

	for (int64 from = 0; from < n; from += SIFT_VEC_LEN)
	{
		b.n = kv.len = vv.len = (uint16) Min(SIFT_VEC_LEN, n - from);
		for (int i = 0; i < b.n; i++)
			vals[i] = from + i + 1;
		sift_rollup_feed(t, &b);
	}
	pfree(keys);
	pfree(vals);
}

/*
 * sift._test_rollup_distinct(masks int8[], n int8, max_bytes int8, OUT errored bool, OUT retained int8): count(DISTINCT) of n distinct values in the sets
 * masks of one key column, in a context of its own under a budget of max_bytes. errored: the budget was exceeded; retained: the bytes the context still
 * holds in use after the tree is freed, which are none but the headers of its blocks.
 */
Datum
sift_test_rollup_distinct(PG_FUNCTION_ARGS)
{
	ArrayType  *masks = PG_GETARG_ARRAYTYPE_P(0);
	MemoryContext parent = CurrentMemoryContext,
				cxt = sift_mem_allocset_create(parent, "rollup distinct");
	MemoryContextCounters counters = {0};
	SiftRollupSpec spec = {.n_keys = 1, .n_vals = 1, .count_distinct = true};
	SiftRollupTree *t;
	Datum	   *md,
				row[2];
	bool	   *mn,
				nulls[2] = {false, false},
				errored = false;
	TupleDesc	desc;
	int			nm;

	sift_aggkern6_init();
	deconstruct_array(masks, INT8OID, 8, true, 'd', &md, &mn, &nm);
	spec.n_sets = (uint16) nm;
	for (int s = 0; s < nm; s++)
		spec.set_masks[s] = (uint64) DatumGetInt64(md[s]);
	t = sift_rollup_create(cxt, &spec, (uint64) PG_GETARG_INT64(2));
	PG_TRY();
	{
		feed_distinct(t, PG_GETARG_INT64(1));
		sift_rollup_finish(t);
		sift_rollup_emit(t, discard_group, NULL);
	}
	PG_CATCH();
	{
		ErrorData  *e;

		MemoryContextSwitchTo(parent);
		e = CopyErrorData();
		if (e->sqlerrcode != ERRCODE_PROGRAM_LIMIT_EXCEEDED)
			PG_RE_THROW();
		FlushErrorState();
		errored = true;
	}
	PG_END_TRY();
	sift_rollup_free(t);
	MemoryContextMemConsumed(cxt, &counters);
	MemoryContextDelete(cxt);
	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	row[0] = BoolGetDatum(errored);
	row[1] = Int64GetDatum((int64) (counters.totalspace - counters.freespace));
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), row, nulls)));
}

#endif							/* SIFT_TESTING */
