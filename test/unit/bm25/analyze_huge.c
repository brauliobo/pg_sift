/*
 * test/unit/bm25/analyze_huge.c - the buffers of the analyzer (bm25/analyze/scan.h) past 1 GB: they are made with the huge allocation, and growing one past
 * 1 GB must not fail (it grew with repalloc, which refuses that size: "invalid memory alloc request size" for a doc whose terms or positions passed 1 GB).
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

#include "bm25/analyze/analyze.h"
#include "core/cpu.h"

SiftCpu		sift_cpu;

static SiftAnalyzer *an;

static void
grow_past_1gb(void *arg)
{
	sift_analyzer_spare_terms(an, MaxAllocSize / sizeof(SiftTermFreq) / 2 + 1);
	sift_analyzer_spare_terms(an, MaxAllocSize / sizeof(SiftTermFreq) + 1);
}

int
main(void)
{
	SiftAnalyzerConfig cfg = {0};

	an = sift_analyzer_create(&cfg, CurrentMemoryContext);
	SIFT_CHECKF(pgshim_try(grow_past_1gb, NULL), "growing past 1 GB failed: %s", pgshim_errmsg);
	return sift_check_summary();
}
