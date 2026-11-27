/*
 * test/unit/agg/topn/tourn.c - the rows WITH TIES of the Top-N tournament (agg/topn/tourn.c): the ties held in memory stay within the bytes the tournament is given, at any work_mem
 * within one allocation, and a result past what one allocation holds fails with 54000 before it is allocated.
 */
#include "postgres.h"

#include "agg/topn/tourn.c"

#include "test/common/check.h"
#include "test/common/pgshim.h"

volatile sig_atomic_t InterruptPending = false;

static const SiftTournSpec spec = {.nkeys = 1,.limit = 1,.with_ties = true};

static bool
offer(SiftTournament *t, uint64 tid)
{
	const SiftTopNWinRow row = {.key = {7},.tid = tid};

	return sift_tourn_offer(t, &row);
}

static void
test_ties_within_bytes(void)
{
	SiftTournament *t = sift_tourn_create(CurrentMemoryContext, &spec, 10 * sizeof(SiftTopNWinRow), NULL);
	uint32		n;

	for (uint64 tid = 1; tid <= 10; tid++)
		SIFT_CHECK(offer(t, tid));
	SIFT_CHECKF(t->tie_cap <= t->max_tie_rows, "%u ties held for at most %llu", t->tie_cap, (unsigned long long) t->max_tie_rows);
	(void) sift_tourn_result(t, &n);
	SIFT_CHECKF(n == 10, "the row and its 9 ties: %u", n);
	sift_tourn_free(t);
}

static void
test_ties_one_allocation(void)
{
	SiftTournament *t = sift_tourn_create(CurrentMemoryContext, &spec, UINT64CONST(8) << 30, NULL);

	SIFT_CHECKF(t->max_tie_rows * sizeof(SiftTopNWinRow) <= MaxAllocSize, "%llu ties fit one allocation", (unsigned long long) t->max_tie_rows);
	sift_tourn_free(t);
}

static void
result_fn(void *arg)
{
	uint32		n;

	(void) sift_tourn_result(arg, &n);
}

static void
test_result_bound(void)
{
	SiftTournament *t = sift_tourn_create(CurrentMemoryContext, &spec, 0, NULL);

	SIFT_CHECK(offer(t, 1));
	t->tie_spilled = MaxAllocSize / sizeof(SiftTopNWinRow);
	SIFT_CHECK(!pgshim_try(result_fn, t));
	SIFT_CHECKF(pgshim_sqlstate == ERRCODE_PROGRAM_LIMIT_EXCEEDED && strstr(pgshim_errmsg, "WITH TIES") != NULL, "%d: %s", pgshim_sqlstate, pgshim_errmsg);
	pgshim_reset();
}

int
main(void)
{
	pgshim_keep();
	if (unit_wants("tourn_ties_within_bytes"))
		test_ties_within_bytes();
	if (unit_wants("tourn_ties_one_allocation"))
		test_ties_one_allocation();
	if (unit_wants("tourn_result_bound"))
		test_result_bound();
	return sift_check_summary();
}
