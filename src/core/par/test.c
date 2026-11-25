/*
 * core/par/test.c - test-only SQL function of the pool, the driver and the grouped aggregation of core/par and agg/v6/par, compiled only with SIFT_TESTING and registered by the pg_sift_test
 * extension: `loops` runs of the aggregation of `rows` rows of `groups` groups on `workers` threads of the pool. Every run must give every group and every row once. The statement may be cancelled
 * (statement_timeout, pg_cancel_backend, or the leader's n-th unit when cancel_at is set), and the pool is then idle and its memory freed: the next call runs as the first did.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "funcapi.h"

#include "agg/v6/par/group.h"
#include "core/hash.h"
#include "core/par/pool.h"
#include "miscadmin.h"

PG_FUNCTION_INFO_V1(sift_test_par_group);

typedef struct ParTest
{
	uint32		rows;
	uint64	   *key;
	uint64	   *arg;
	uint32		cancel_at;
	pg_atomic_uint32 leader_units;
	struct
	{
		uint64		groups;
		uint64		rows;
		char		pad[48];
	}			tally[SIFT_PAR_MAX_THREADS];
} ParTest;

static bool
test_fill(void *arg, int worker, uint32 lo, uint32 hi, SiftArena *scratch, SiftParGroupBatch *b, SiftErr *err)
{
	ParTest    *t = arg;

	if (worker == 0 && t->cancel_at > 0 && pg_atomic_add_fetch_u32(&t->leader_units, 1) == t->cancel_at)
	{
		QueryCancelPending = true;
		InterruptPending = true;
	}
	b->n = hi - lo;
	b->key[0] = t->key + lo;
	b->arg[0] = t->arg + lo;
	return true;
}

static void
test_emit(void *arg, int worker, const SiftParGroupRow *row)
{
	ParTest    *t = arg;

	t->tally[worker].groups++;
	t->tally[worker].rows += row->count;
}

static void
test_overflow(void *arg, const SiftParRange *left, uint32 nleft)
{
	elog(ERROR, "sift._test_par_group: the arenas were made too small");	/* check-errors: allow */
}

static void
run_once(ParTest *t, int workers, bool high_card, uint64 *groups, uint64 *rows)
{
	const SiftParGroupSpec sp = {.nk = 1,.nsum = 1};
	const SiftParGroupHooks hooks = {.fill = test_fill,.emit = test_emit,.overflow = test_overflow,.arg = t};
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "par test run", ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	const size_t state = (size_t) t->rows * 64 / workers + (32 << 20);
	SiftParArenas *ar = sift_par_arenas_create(cx, workers, 1 << 20, state, 64 << 20);
	SiftErr		err = {0};
	SiftParGroup *g = sift_par_group_create(ar, &sp, high_card, t->rows, &hooks, &err);

	if (g == NULL)
		sift_err_rethrow(NULL, &err);
	memset(t->tally, 0, sizeof(t->tally));
	PG_TRY();
	{
		if (!sift_par_run(&(SiftParCfg) {.nthreads = workers,.items = t->rows}, &sift_par_group_ops, g, ar, NULL))
			elog(ERROR, "sift._test_par_group: the run overflowed");	/* check-errors: allow */
	}
	PG_FINALLY();
	{
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cx);
	}
	PG_END_TRY();
	*groups = *rows = 0;
	for (int w = 0; w < SIFT_PAR_MAX_THREADS; w++)
	{
		*groups += t->tally[w].groups;
		*rows += t->tally[w].rows;
	}
}

Datum
sift_test_par_group(PG_FUNCTION_ARGS)
{
	const int	workers = Min(PG_GETARG_INT32(0), sift_par_pool_threads());
	const uint32 rows = PG_GETARG_INT32(1);
	const uint32 groups = PG_GETARG_INT32(2);
	const int	loops = PG_GETARG_INT32(3);
	ParTest    *t = palloc0(sizeof(ParTest));
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {false};
	int			done = 0;
	uint64		bad = 0,
				seen_groups = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");	/* check-errors: allow */
	t->rows = rows;
	t->cancel_at = PG_GETARG_INT32(4);
	pg_atomic_init_u32(&t->leader_units, 0);
	t->key = palloc_extended(sizeof(uint64) * rows, MCXT_ALLOC_HUGE);
	t->arg = palloc_extended(sizeof(uint64) * rows, MCXT_ALLOC_HUGE);
	for (uint32 i = 0; i < rows; i++)
	{
		t->key[i] = (sift_fmix64(i) % groups) * UINT64CONST(0x9E3779B97F4A7C15);
		t->arg[i] = 1;
	}
	for (; done < loops; done++)
	{
		uint64		g,
					r;

		run_once(t, workers, groups > SIFT_PAR_GROUP_LOW, &g, &r);
		bad += (r != rows || g > groups || g == 0);
		seen_groups = g;
		CHECK_FOR_INTERRUPTS();
	}
	values[0] = Int32GetDatum(workers);
	values[1] = Int32GetDatum(done);
	values[2] = Int64GetDatum(seen_groups);
	values[3] = Int64GetDatum(bad);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

#endif							/* SIFT_TESTING */
