/*
 * test/unit/bm25/par_collect.c - the hits the emitter of a parallel ranked search (bm25/parallel.c) collects: a worker that attaches once the emitter took the
 * result claims no units and publishes into no slot the emitter reads. The emitter sized its array by the participants it counted, and a worker that attached
 * between the count and the copy had its hits copied past the end of the array.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"

#include "bm25/parallel.c"

#define K	64

static SiftPar *late_in_collect;
static void publish(SiftPar *par, SiftKey base);

Size
add_size(Size a, Size b)
{
	return a + b;
}

Size
mul_size(Size a, Size b)
{
	return a * b;
}

dsa_pointer
dsa_allocate_extended(dsa_area *a, size_t size, int flags)
{
	return (dsa_pointer) (uintptr_t) calloc(1, size);
}

void
dsa_free(dsa_area *a, dsa_pointer dp)
{
	free((void *) (uintptr_t) dp);
}

/* The emitter's copy reads the published hits here: a worker armed in late_in_collect attaches and publishes at that moment */
void *
dsa_get_address(dsa_area *a, dsa_pointer dp)
{
	SiftPar    *late = late_in_collect;

	if (late)
	{
		late_in_collect = NULL;
		sift_par_attach(late);
		publish(late, 1000);
		sift_par_finish(late);
	}
	return (void *) (uintptr_t) dp;
}

void
ConditionVariableInit(ConditionVariable *cv)
{
}

static void
publish(SiftPar *par, SiftKey base)
{
	SiftParHit	hits[K];

	for (int i = 0; i < K; i++)
		hits[i] = (SiftParHit) {base + i, base + i, (float4) i};
	sift_par_publish(par, hits, K);
}

static void
check_collect(SiftPar *leader, const char *when)
{
	int64		n;
	SiftParHit *all = sift_par_collect(leader, &n);

	SIFT_CHECKF(n == K, "%s: the emitter collected %lld hits, its participants published %d", when, (long long) n, K);
	for (int i = 0; i < Min(n, K); i++)
		SIFT_CHECKF(all[i].key == (SiftKey) (1 + i), "%s: hit %d has key %llu", when, i, (unsigned long long) all[i].key);
	pfree(all);
}

static SiftParShared *
start(SiftPar *leader)
{
	SiftParShared *sh = calloc(1, sift_par_estimate(1));

	sift_par_init(sh, 1, 0);
	*leader = (SiftPar) {sh, NULL, 0};
	sift_par_attach(leader);
	publish(leader, 1);
	SIFT_CHECK(sift_par_finish(leader));
	return sh;
}

static void
late_before_collect(void)
{
	SiftPar		leader;
	SiftParShared *sh = start(&leader);
	SiftPar		late = {sh, NULL, 0};
	int64		lo,
				hi;

	sift_par_attach(&late);
	SIFT_CHECKF(!sift_par_next_units(&late, 1000, &lo, &hi), "a late participant claimed units [%lld, %lld)", (long long) lo, (long long) hi);
	publish(&late, 1000);
	SIFT_CHECK(!sift_par_finish(&late));
	check_collect(&leader, "late before the collect");
	sift_par_reinit(sh, NULL, 1, 0);
	free(sh);
}

static void
late_during_collect(void)
{
	SiftPar		leader;
	SiftParShared *sh = start(&leader);
	SiftPar		late = {sh, NULL, 0};

	late_in_collect = &late;
	check_collect(&leader, "late during the collect");
	sift_par_reinit(sh, NULL, 1, 0);
	free(sh);
}

int
main(void)
{
	late_before_collect();
	late_during_collect();
	return sift_check_summary();
}
