/*
 * agg/v6/v6_par_topk.c - the heap of the best groups of a participant of the parallel SiftAggV6 (agg/v6/v6_par_topk.h).
 */
#include "postgres.h"

#include "agg/v6/v6_par_topk.h"
#include "agg/v6/v6_topk_run.h"

struct SiftV6ParTopK
{
	SiftV6TopKRun *run;
	MemoryContext tuple_cx;
	SiftV6ParTopKSource source;
	void	   *arg;
	bool		drained;
};

SiftV6ParTopK *
sift_v6_par_topk_new(MemoryContext cx, const SiftV6Spec *spec, TupleDesc desc, MemoryContext tuple_cx, SiftV6ParTopKSource source, void *arg)
{
	SiftV6ParTopK *t = MemoryContextAllocZero(cx, sizeof(SiftV6ParTopK));

	*t = (SiftV6ParTopK) {.run = sift_v6_topk_run_new(cx, spec, desc),.tuple_cx = tuple_cx,.source = source,.arg = arg};
	return t;
}

static bool
next_group(void *arg, Datum *values, bool *isnull)
{
	const SiftV6ParTopK *t = arg;

	MemoryContextReset(t->tuple_cx);
	return t->source(t->arg, values, isnull);
}

bool
sift_v6_par_topk_next(SiftV6ParTopK *t, Datum *values, bool *isnull)
{
	if (!t->drained)
	{
		t->drained = true;
		sift_v6_topk_run_stream(t->run, next_group, t, NULL, NULL, NULL);
		MemoryContextReset(t->tuple_cx);
	}
	return sift_v6_topk_run_next(t->run, values, isnull);
}
