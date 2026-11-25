/*
 * agg/par/topk.c - the heap of the best groups of a participant of the parallel SiftAggV6 (agg/par/topk.h).
 */
#include "postgres.h"

#include "agg/par/topk.h"
#include "agg/topn/topk_run.h"

struct SiftV6ParTopK
{
	SiftV6TopKRun *run;
	bool		drained;
};

SiftV6ParTopK *
sift_v6_par_topk_new(MemoryContext cx, const SiftV6Spec *spec, TupleDesc desc)
{
	SiftV6ParTopK *t = MemoryContextAllocZero(cx, sizeof(SiftV6ParTopK));

	t->run = sift_v6_topk_run_new(cx, spec, desc);
	return t;
}

SiftV6TopKRun *
sift_v6_par_topk_heap(const SiftV6ParTopK *t)
{
	return t->run;
}

bool
sift_v6_par_topk_next(SiftV6ParTopK *t, SiftV6ParRun *r, Datum *values, bool *isnull)
{
	if (!t->drained)
	{
		const bool	emitted = sift_v6_par_next(r, values, isnull);

		t->drained = true;
		SIFT_INVARIANT(!emitted, "a run with a heap offers its groups there and emits none");
		sift_v6_topk_run_finish(t->run);
	}
	return sift_v6_topk_run_next(t->run, values, isnull);
}
