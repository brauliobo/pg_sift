/*
 * test/scaling/case_card.c - agg/v6/cost/card_est.c: the planner's estimate of a windowed scan from the summary of the manifest. The partitions grow 8x with the window a quarter of them:
 * the estimate walks the partitions the window leaves once, O(P).
 */
#include "postgres.h"

#include <stdlib.h>

#include "agg/v6/cost/card_est.h"
#include "op_counters.h"

#define ROWS_PER_PART 1000

static void
run_card(uint32_t nparts)
{
	SiftCardPartition *parts = calloc(nparts, sizeof(SiftCardPartition));
	SiftColCardStats col = {.col = 0, .flags = SIFT_CSTAT_HAS_RANGE | SIFT_CSTAT_HAS_NDV, .total_rows = (uint64_t) nparts * ROWS_PER_PART,
							.ndv_est = (double) nparts * ROWS_PER_PART, .min_i64 = 0, .max_i64 = (int64_t) nparts * ROWS_PER_PART - 1};
	SiftCardStatsView view = {.total_live_rows = (uint64_t) nparts * ROWS_PER_PART, .total_granules = (uint64_t) nparts * 10, .ncols = 1, .nparts = nparts, .cols = &col, .parts = parts};
	int64_t		lo = (int64_t) nparts * ROWS_PER_PART / 4,
				hi = (int64_t) nparts * ROWS_PER_PART / 2;
	SiftCardPred pred = {.kind = SCP_RANGE, .col = 0, .has_lo = true, .has_hi = true, .lo = lo, .hi = hi};
	SiftCardQuery q = {.preds = &pred, .npreds = 1, .has_window = true, .win_lo = lo, .win_hi = hi, .lead_col = 0};
	SiftCardEst est;

	for (uint32_t i = 0; i < nparts; i++)
		parts[i] = (SiftCardPartition) {.rows = ROWS_PER_PART, .lo = (int64_t) i * ROWS_PER_PART, .hi = (int64_t) i * ROWS_PER_PART + ROWS_PER_PART - 1};
	OP_BEGIN();
	sift_card_estimate(&view, &q, &est);
	OP_END();
	if (est.est_rows <= 0)
		abort();
	free(parts);
}

const ScalingCase case_card = {
	.name = "card_est", .under_test = "agg/v6/v6_card_est.c, partitions x8, window a quarter", .n_small = 512, .factor = 8, .run = run_card,
	GATES({OP_STEPS, 9.5, NULL}, {OP_ALLOC_BYTES, 1.25, NULL}),
};
