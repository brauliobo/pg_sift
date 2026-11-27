/*
 * test/unit/bm25/rank_budget.c - the hits of a ranked search (bm25/rank/search.c) fit work_mem and one allocation: past 1 GB, which a work_mem of 2 GB allows, the
 * ties, the deferred ties, the sorted hits and the result the emitter of a parallel search merges fail the statement naming the limit ("invalid memory alloc
 * request size" before), and a search without a limit sorts its hits outside memory before its heap of hits reaches 1 GB. The merged result is held to work_mem
 * as one search's hits are (it took every participant's hits whatever their number).
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf

#include "test/common/check.h"
#include "test/common/pgshim.h"

static void *test_repalloc(void *p, Size n);

#define repalloc	test_repalloc
#include "bm25/rank/search.c"
#undef repalloc

#define MERGED		8192
#define SPILL_HITS	((int64) 1 << 24)

int			work_mem;

static int64 spilled;

/* A grown array is not copied: the tests read none of its hits, and a copy of 1 GB would touch it all */
static void *
test_repalloc(void *p, Size n)
{
	if (!AllocSizeIsValid(n))
		elog(ERROR, "invalid memory alloc request size %zu", n);
	pfree(p);
	return palloc(n);
}

SiftHitSpill *
sift_hitspill_begin(MemoryContext mcxt, int work_mem_kb)
{
	return (SiftHitSpill *) 1;
}

void
sift_hitspill_put(SiftHitSpill *s, SiftKey key, float4 score)
{
	spilled++;
}

void
sift_par_publish(SiftPar *par, const SiftParHit *hits, int64 n)
{
}

bool
sift_par_finish(SiftPar *par)
{
	return true;
}

SiftParHit *
sift_par_collect(SiftPar *par, int64 *n)
{
	SiftParHit *all = palloc_extended(MERGED * sizeof(SiftParHit), MCXT_ALLOC_HUGE);

	for (int64 i = 0; i < MERGED; i++)
		all[i] = (SiftParHit) {i + 1, i + 1, (float4) i};
	*n = MERGED;
	return all;
}

int64
sift_par_units_claimed(const SiftPar *par, int64 nunit)
{
	return 0;
}

typedef struct Ctx
{
	SiftRanker	r;
	SiftPlan	plan;
	SiftTopK	tk;
} Ctx;

/* A full array arr of entries of size elem, one more being past 1 GB once doubled (its pages are never touched) */
static void
fill(void **arr, int64 *n, int64 *cap, Size elem)
{
	*cap = *n = (int64) (MaxAllocSize / elem) / 2 + 1;
	*arr = palloc_extended(*cap * elem, MCXT_ALLOC_HUGE);
}

static void
grow_ties(void *arg)
{
	Ctx		   *c = arg;

	fill((void **) &c->tk.tie, &c->tk.ntie, &c->tk.tiecap, sizeof(SiftHit));
	tie_add(&c->r, &c->tk, 1, 1);
}

static void
grow_lazy(void *arg)
{
	Ctx		   *c = arg;

	fill((void **) &c->tk.lazy, &c->tk.nlazy, &c->tk.lazycap, sizeof(SiftLazyHit));
	tie_defer(&c->r, &c->tk, 1, 1, 0);
}

static void
append_ties(void *arg)
{
	Ctx		   *c = arg;

	c->tk.cap = c->tk.n = MaxAllocSize / sizeof(SiftHit);
	c->tk.hit = palloc_extended(c->tk.cap * sizeof(SiftHit), MCXT_ALLOC_HUGE);
	c->tk.tie = palloc(sizeof(SiftHit));
	c->tk.tie[0] = (SiftHit) {1, 1};
	c->tk.ntie = c->tk.tiecap = 1;
	rank_append_ties(&c->tk);
}

/* A search without a limit whose heap of hits would double to 512 MB: its hits go to the sort outside memory */
static void
spill(void *arg)
{
	Ctx		   *c = arg;

	c->r.spill_ok = true;
	c->tk.k = PG_INT64_MAX;
	c->tk.cap = c->tk.n = SPILL_HITS;
	c->tk.hit = palloc_extended(c->tk.cap * sizeof(SiftHit), MCXT_ALLOC_HUGE);
	SIFT_CHECKF(rank_spill_flush(&c->r, &c->tk) && spilled == SPILL_HITS && c->tk.n == 0, "a heap of %lld hits was not sorted outside memory",
				(long long) SPILL_HITS);
}

static void
merge(void *arg)
{
	Ctx		   *c = arg;

	c->r.hit = palloc(sizeof(SiftHit));
	c->r.hitcap = 1;
	rank_merge_shared(&c->r);
}

typedef struct Case
{
	const char *what;
	void		(*fn) (void *);
	int			work_mem_kb;
	int64		K;
	bool		ties;
	bool		fails;
} Case;

static const Case cases[] = {
	{"ties past 1 GB", grow_ties, MAX_KILOBYTES, PG_INT64_MAX, true, true},
	{"deferred ties past 1 GB", grow_lazy, MAX_KILOBYTES, PG_INT64_MAX, true, true},
	{"sorted hits with their ties past 1 GB", append_ties, MAX_KILOBYTES, PG_INT64_MAX, true, true},
	{"hits without a limit past 512 MB", spill, MAX_KILOBYTES, PG_INT64_MAX, false, false},
	{"a merged result within work_mem", merge, 32, 16, false, false},
	{"a merged result past work_mem", merge, 32, MERGED, false, true},
	{"a merged result with ties past work_mem", merge, 32, MERGED / 2, true, true},
};

int
main(void)
{
	for (int i = 0; i < lengthof(cases); i++)
	{
		const Case *t = &cases[i];
		Ctx			c = {0};
		bool		ok;

		c.plan.mcxt = CurrentMemoryContext;
		c.r.plan = &c.plan;
		c.r.par = (SiftPar *) 1;
		c.r.K = t->K;
		c.r.ties = t->ties;
		work_mem = t->work_mem_kb;
		spilled = 0;
		ok = pgshim_try(t->fn, &c);
		SIFT_CHECKF(ok != t->fails, "%s: %s", t->what, ok ? "did not fail" : pgshim_errmsg);
		SIFT_CHECKF(ok || pgshim_sqlstate == ERRCODE_PROGRAM_LIMIT_EXCEEDED, "%s: failed without naming the limit: %s", t->what, pgshim_errmsg);
		pgshim_reset();
	}
	return sift_check_summary();
}
