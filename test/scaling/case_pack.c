/*
 * test/scaling/case_pack.c - col/f2/f2_pack.c: first fit decreasing of the small parts of a partition into the pages that hold them (assign_bins, static: the source is included).
 */
#include "col/f2/f2_pack.c"

#include <stdlib.h>

#include "op_counters.h"

static void
run_pack(uint32_t n)
{
	PackCand   *cands = calloc(n, sizeof(PackCand));
	PackBin    *bins = calloc(n, sizeof(PackBin));
	int			nbins;

	for (uint32_t i = 0; i < n; i++)
		cands[i].len = (uint16) (300 + (uint64_t) i * 2654435761u % 3000);
	OP_BEGIN();
	nbins = assign_bins(cands, (int) n, bins);
	OP_END();
	if (nbins == 0)
		abort();
	free(cands);
	free(bins);
}

const ScalingCase case_pack = {
	.name = "pack_ffd", .under_test = "col/f2/f2_pack.c assign_bins, candidates x8", .n_small = 512, .factor = 8, .run = run_pack,
	GATES({OP_STEPS, 11.5, NULL}, {OP_SORT_CALLS, 11.5, NULL}),
};
