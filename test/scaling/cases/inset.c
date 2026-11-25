/*
 * test/scaling/cases/inset.c - agg/inset/filter.c: `column = ANY (array)` tested per row. The rows are fixed (65536); the array grows 8x: a binary search per row, O(log V).
 */
#include "postgres.h"

#include <stdlib.h>

#include "agg/inset/inset.h"
#include "op_counters.h"

#define ROWS	65536
#define BATCH	1024

static void
run_inset(uint32_t v)
{
	int64	   *vals = malloc(sizeof(int64) * v);
	int64	   *col = malloc(sizeof(int64) * ROWS);
	uint64	   *keep = malloc(sizeof(uint64) * (BATCH / 64));
	SiftV6InSet set = {.n = (int) v, .vals = vals};
	uint64_t	left = 0;

	for (uint32_t i = 0; i < v; i++)
		vals[i] = (int64) i * 2;
	for (uint32_t i = 0; i < ROWS; i++)
		col[i] = (int64) ((uint64_t) i * 2654435761u % (2 * (uint64_t) v));
	OP_BEGIN();
	for (uint32_t b = 0; b < ROWS; b += BATCH)
	{
		SiftVec		vec = {.data = col + b, .len = BATCH, .all_valid = true, .dense = true, .type = SVEC_I64};

		for (uint32_t w = 0; w < BATCH / 64; w++)
			keep[w] = ~UINT64CONST(0);
		left += sift_v6_inset_filter(&set, &vec, BATCH, keep, BATCH);
	}
	OP_END();
	free(vals);
	free(col);
	free(keep);
	if (left == 0 || left == ROWS)
		abort();
}

const ScalingCase case_inset = {
	.name = "inset", .under_test = "agg/v6_inset_filter.c, IN list x8, rows fixed", .n_small = 512, .factor = 8, .run = run_inset,
	GATES({OP_STEPS, 1.5, NULL}, {OP_CMPS, 1.5, NULL}, {OP_ALLOC_BYTES, 1.25, NULL}),
};
