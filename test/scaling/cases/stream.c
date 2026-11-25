/*
 * test/scaling/cases/stream.c - agg/stream_exec.c: the sorted GROUP BY and Top-N take one source per part of the partition, added one at a time (sift_stream_add_source): the cursors of
 * P sources are P allocations' worth of bytes, O(P), not the P(P+1)/2 of growing by one cursor each time.
 */
#include "core/host.h"

#include <stdlib.h>

#include "agg/stream_exec.h"
#include "op_counters.h"

static void
run_stream(uint32_t nparts)
{
	SiftStreamExec se;

	sift_stream_init(&se, 1, false, false, 0, 10, NULL, NULL, NULL);
	OP_BEGIN();
	for (uint32_t i = 0; i < nparts; i++)
		sift_stream_add_source(&se, NULL, NULL);
	OP_END();
	if (se.n_parts != nparts)
		abort();
	sift_stream_free(&se);
}

const ScalingCase case_stream = {
	.name = "stream_sources", .under_test = "agg/stream_exec.c sources added, parts x8", .n_small = 512, .factor = 8, .run = run_stream,
	GATES({OP_STEPS, 9.5, NULL}, {OP_ALLOC_BYTES, 9.5, "complexity.allow: repalloc-additive src/agg/stream_exec.c"}),
};
