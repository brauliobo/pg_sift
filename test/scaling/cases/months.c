/*
 * test/scaling/cases/months.c - col/scope_v6/cube.c: the table of month cubes of a scope, one cube per month in ascending order, then every month found again: O(M log M).
 */
#include "postgres.h"

#include <stdlib.h>

#include "col/scope_v6/meta.h"
#include "op_counters.h"

static void
run_months(uint32_t m)
{
	SiftScopeShape shape = {.nmeas = 1, .ndims = 1};
	SiftScopeMonthTable t;

	sift_smonth_init(&t);
	OP_BEGIN();
	for (uint32_t i = 0; i < m; i++)
		sift_smonth_get(&t, (int32) i, shape);
	for (uint32_t i = 0; i < m; i++)
		if (sift_smonth_find(&t, (int32) i) == NULL)
			abort();
	OP_END();
	sift_smonth_free(&t);
}

const ScalingCase case_months = {
	.name = "month_table", .under_test = "col/scope_v6/cube.c month table, months x8", .n_small = 128, .factor = 8, .run = run_months,
	GATES({OP_STEPS, 12.0, NULL}, {OP_CMPS, 12.0, NULL}, {OP_MOVE_BYTES, 9.0, NULL}, {OP_ALLOC_BYTES, 9.5, NULL}),
};
