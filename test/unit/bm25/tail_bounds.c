/*
 * test/unit/bm25/tail_bounds.c - the bounds of the deferred wide leaves (bm25/tail.c): whether a K-th score settles the terms the leaves left out is decided from
 * what every other leaf gives a document at most, a sum each segment has once; summing it again for every leaf made a plan of L leaves cost L * L per segment.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"

#include "bm25/tail.c"

#define NLEAVES	2000
#define NSEGS	3

volatile sig_atomic_t InterruptPending = 0;
static uint64 weighed;

float4
sift_kern_ub_weight(float4 w, double avgdl_now, double avgdl_ref, int nterms)
{
	weighed++;
	return w;
}

/* A plan of nleaves deferred scoring leaves of weight 1, each with a bound of 1 left out and over its terms in every segment */
static SiftPlan *
plan_of(int nleaves)
{
	SiftPlan   *p = calloc(1, sizeof(SiftPlan));
	SiftSegHeaderData *hdr = calloc(1, sizeof(SiftSegHeaderData));
	static uint8 mb[NSEGS] = {1, 1, 1};

	p->nsegs = NSEGS;
	p->seg = calloc(NSEGS, sizeof(SiftSegment *));
	for (int s = 0; s < NSEGS; s++)
		(p->seg[s] = calloc(1, sizeof(SiftSegment)))->hdr = hdr;
	p->nleaves = p->ndeferred = nleaves;
	p->leaves = calloc(nleaves, sizeof(EvalLeaf));
	for (int i = 0; i < nleaves; i++)
		p->leaves[i] = (EvalLeaf) {.scoring = true, .deferred = true, .wide_w = 1, .tail_mb = mb, .all_mb = mb};
	return p;
}

int
main(void)
{
	SiftPlan   *p = plan_of(NLEAVES);

	SIFT_CHECKF(sift_plan_tail_settled(p, NLEAVES * 1.01f), "a K-th score above every bound settles the leaves");
	SIFT_CHECKF(weighed <= 4 * NLEAVES * NSEGS, "%llu weights for %d leaves in %d segments", (unsigned long long) weighed, NLEAVES, NSEGS);
	SIFT_CHECKF(!sift_plan_tail_settled(p, NLEAVES * 0.99f), "a K-th score under the bound of a leaf and the others does not settle it");
	p->leaves[0] = (EvalLeaf) {.nterms = calloc(NSEGS, sizeof(int))};
	p->ndeferred--;
	SIFT_CHECKF(sift_plan_tail_settled(p, NLEAVES - 0.5f), "a leaf that does not score adds nothing to the bound of the others");
	SIFT_CHECKF(!sift_plan_tail_settled(p, NLEAVES - 1.5f), "the bound of a leaf is its own and that of the other scoring leaves");
	return sift_check_summary();
}
