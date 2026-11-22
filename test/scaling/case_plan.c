/*
 * test/scaling/case_plan.c - col/f2/compact_plan.c: what a compaction takes of the parts of a partition, from their descriptors. The parts spread over four levels and both tiers.
 */
#include "postgres.h"

#include <stdlib.h>

#include "col/f2/compact.h"
#include "op_counters.h"

static void
run_plan(uint32_t nparts)
{
	SiftPartDesc *parts = calloc(nparts, sizeof(SiftPartDesc));
	SiftCompactGroup *groups;
	int			ngroups;

	for (uint32_t i = 0; i < nparts; i++)
	{
		parts[i].part_gen = i + 1;
		parts[i].level = i % 4;
		parts[i].nbytes = 4096 + (uint64_t) i * 2654435761u % (200 * 1024);
		parts[i].nrows = 1000;
		parts[i].ndead = i % 7 == 0 ? 400 : 0;
		parts[i].flags = i % 5 == 0 ? SIFT_PART_FLAG_TIME_COLD : 0;
	}
	OP_BEGIN();
	groups = sift_compact_plan(parts, (int) nparts, UINT64_C(64) << 20, PG_INT64_MIN, &ngroups);
	OP_END();
	if (ngroups == 0)
		abort();
	for (int i = 0; i < ngroups; i++)
		pfree(groups[i].gens);
	pfree(groups);
	free(parts);
}

const ScalingCase case_plan = {
	.name = "compact_plan", .under_test = "col/f2/compact_plan.c, parts x8", .n_small = 1024, .factor = 8, .run = run_plan,
	GATES({OP_STEPS, 11.5, NULL}, {OP_CMPS, 11.5, NULL}, {OP_SORT_CALLS, 12.5, NULL}, {OP_ALLOC_BYTES, 9.0, NULL}),
};
