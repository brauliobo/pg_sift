/*
 * test/scaling/cases/topn.c - agg/topn_v6.c: the Top-N tournament holds the best 64 rows of n offered in the worst order for it (every row beats the worst held), O(n log k).
 */
#include "postgres.h"

#include <stdlib.h>

#include "agg/topn_v6.h"
#include "op_counters.h"

#define K 64

static void
run_topn(uint32_t n)
{
	SiftTournSpec spec = {.nkeys = 1, .limit = K};
	SiftTournament *t = sift_tourn_create(CurrentMemoryContext, &spec, 0, NULL);
	SiftTopNWinRow *rows = calloc(n, sizeof(SiftTopNWinRow));
	uint32		got;

	for (uint32_t i = 0; i < n; i++)
		rows[i] = (SiftTopNWinRow) {.key = {(int64) (n - i)}, .tid = i};
	OP_BEGIN();
	for (uint32_t i = 0; i < n; i++)
		sift_tourn_offer(t, &rows[i]);
	if (sift_tourn_result(t, &got) == NULL || got != K)
		abort();
	OP_END();
	sift_tourn_free(t);
	free(rows);
}

const ScalingCase case_topn = {
	.name = "topn", .under_test = "agg/topn_v6.c tournament, descending offers", .n_small = 8192, .factor = 8, .run = run_topn,
	GATES({OP_STEPS, 9.5, NULL}, {OP_CMPS, 9.5, NULL}, {OP_ALLOC_BYTES, 1.25, NULL}),
};
