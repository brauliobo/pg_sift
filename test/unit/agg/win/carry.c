/*
 * test/unit/agg/win/carry.c - the carry table of agg/win/carry.c grows while its budget and one palloc of its values hold it, at any work_mem.
 */
#include "postgres.h"

#include "agg/win/carry.c"

#include "test/common/check.h"
#include "test/common/pgshim.h"

static uint64
max_slots_at(int nvars, uint64 max_bytes)
{
	SiftWinCarrySpec spec = {.nvars = nvars,.max_bytes = max_bytes};
	SiftWinCarry *c = sift_win_carry_create(CurrentMemoryContext, &spec);
	const uint64 slots = c->max_slots;

	sift_win_carry_free(c);
	return slots;
}

static void
test_table_fits_budget(void)
{
	const uint64 slots = max_slots_at(2, (uint64) 16 << 20);

	SIFT_CHECK(slots * slot_bytes(2) <= (uint64) 16 << 20 && 2 * slots * slot_bytes(2) > (uint64) 16 << 20);
}

static void
test_table_fits_one_palloc(void)
{
	const int	nvars = SIFT_WIN_CARRY_MAX_VARS;

	SIFT_CHECK(max_slots_at(nvars, (uint64) 4 << 30) * nvars * sizeof(Datum) <= MaxAllocSize);
}

int
main(void)
{
	pgshim_keep();
	if (unit_wants("carry_budget"))
		test_table_fits_budget();
	if (unit_wants("carry_max_alloc"))
		test_table_fits_one_palloc();
	puts("carry: ok");
	return 0;
}
