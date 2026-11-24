/*
 * bench_par_radix.c - the grouped aggregation and COUNT(DISTINCT) of agg/v6/par/ on 1, 2 and 4 threads: the time of the best of three runs of each workload, the speedup, and the check that every
 * thread count gives the same answer (the groups, and the sum of their counts). The shapes are the ones of the ClickBench queries that these operators serve.
 */
#include "postgres.h"

#include <time.h>

#include "core/par/pool.h"
#include "par_data.h"
#include "par_host.h"
#include "pgshim.h"
#include "unit.h"

#define ROWS	4000000
#define RUNS	3

typedef struct Work
{
	const char *name;
	char		kind;			/* 'i' ints, 's' text, 'm' ints and a text, 'd' distinct */
	int			kw;
	int			nsum;
	uint32		ngroups;
	bool		skew;
	bool		key1_u32;
	bool		high;
	uint16		topk;
	bool		desc;
} Work;

static const Work works[] = {
	{"Q15 1 key, count", 'i', 1, 0, 1000000, false, false, true},
	{"Q18 2 keys, 1 sum", 'i', 2, 1, 100000, false, true, true},
	{"Q32 2 keys, count, top 10", 'i', 2, 0, 2000000, false, true, true, 10, true},
	{"Q33 text, count", 's', 0, 0, 500000, false, false, true},
	{"Q34 text, 2 sums", 's', 0, 2, 500000, false, false, true},
	{"Q12 text, top 10", 's', 0, 0, 500000, false, false, true, 10, true},
	{"Q16 int + text, count, top 10", 'm', 1, 0, 500000, false, false, true, 10, true},
	{"Q14 2 ints + text, count", 'm', 2, 0, 500000, false, false, true},
	{"Q8 low groups, count", 'i', 1, 0, 200, false, false, false},
	{"Q8 skewed", 'i', 2, 1, 1000000, true, true, true},
	{"Q4 distinct values", 'd', 0, 0, 0, false, false, true},
	{"Q9 distinct per group", 'd', 0, 0, 6000, false, false, true},
};

static double
now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

typedef struct Tally
{
	uint64		groups;
	uint64		counts;
	char		pad[48];
} Tally;

typedef struct BCtx
{
	Src			src;
	Tally		t[SIFT_PAR_MAX_THREADS];
} BCtx;

static void
b_emit(void *arg, int worker, const SiftParGroupRow *row)
{
	Tally	   *t = &((BCtx *) arg)->t[worker];

	t->groups++;
	t->counts += row->count;
}

static void
b_overflow(void *arg, const SiftParRange *left, uint32 nleft)
{
	Assert(false);
}

static void
b_value(void *arg, uint32 gid, uint64 value)
{
	Assert(false);
}

typedef struct Outcome
{
	double		secs;
	uint64		groups;
	uint64		counts;
} Outcome;

static Outcome
run_once(const Work *w, const Data *d, uint32 workers)
{
	const SiftParGroupSpec sp = {.nk = w->kind == 's' ? 1 : w->kind == 'm' ? w->kw + 1 : w->kw,.strs = w->kind == 's' ? 1 : w->kind == 'm' ? 1 << w->kw : 0,.nsum = w->nsum,
		.key1_u32 = w->key1_u32,.topk = w->topk,.order = 0,.desc = w->desc};
	BCtx		c = {.src = {d, &sp}};
	const size_t state = (size_t) ROWS * 80 / workers + (64 << 20);
	SiftParArenas *ar;
	SiftErr		err = {0};
	Outcome		r = {0};
	double		t0;

	pgshim_reset();
	ar = sift_par_arenas_create(CurrentMemoryContext, workers, 4 << 20, state, 128 << 20);
	t0 = now_s();
	if (w->kind == 'd')
	{
		SiftParDistinctHooks hooks = {.fill = src_distinct_fill,.value = b_value,.overflow = b_overflow,.arg = &c};
		SiftParDistinct *dd = sift_par_distinct_create(ar, w->ngroups, &hooks, &err);
		uint64	   *per = calloc(Max(w->ngroups, 1), sizeof(uint64));

		CHECK(dd != NULL && sift_par_run(&(SiftParCfg) {workers, d->n}, &sift_par_distinct_ops, dd, ar, NULL));
		if (w->ngroups == 0)
			per[0] = sift_par_distinct_count(dd);
		else
			sift_par_distinct_groups(dd, per);
		r.secs = now_s() - t0;
		r.groups = Max(w->ngroups, 1);
		for (uint32 g = 0; g < r.groups; g++)
			r.counts += per[g];
		free(per);
		return r;
	}
	{
		SiftParGroupHooks hooks = {.fill = src_group_fill,.emit = b_emit,.overflow = b_overflow,.arg = &c};
		SiftParGroup *g = sift_par_group_create(ar, &sp, w->high, d->n, &hooks, &err);

		CHECK(g != NULL && sift_par_run(&(SiftParCfg) {workers, d->n}, &sift_par_group_ops, g, ar, NULL));
		r.secs = now_s() - t0;
		for (int t = 0; t < SIFT_PAR_MAX_THREADS; t++)
		{
			r.groups += c.t[t].groups;
			r.counts += c.t[t].counts;
		}
		return r;
	}
}

int
main(void)
{
	static const uint32 threads[] = {1, 2, 4};

	pgshim_keep();
	printf("%-28s %9s %9s %9s %7s %7s  %s\n", "workload", "1T ms", "2T ms", "4T ms", "x2", "x4", "answer");
	for (size_t i = 0; i < sizeof(works) / sizeof(works[0]); i++)
	{
		const Work *w = &works[i];
		Data	   *d = w->kind == 's' ? gen_str(ROWS, w->ngroups, w->skew, w->nsum, true) :
			w->kind == 'm' ? gen_mixed(w->kw + 1, 1 << w->kw, ROWS, w->ngroups, w->skew, w->nsum, false) :
			w->kind == 'd' ? gen_dist(ROWS, ROWS / 4, w->ngroups, true) : gen_int(w->kw, ROWS, w->ngroups, w->skew, w->nsum, w->key1_u32);
		double		best[3];
		Outcome		first = {0};
		bool		same = true;

		for (int k = 0; k < 3; k++)
		{
			best[k] = 1e30;
			for (int r = 0; r < RUNS; r++)
			{
				const Outcome got = run_once(w, d, threads[k]);

				best[k] = Min(best[k], got.secs);
				if (k == 0 && r == 0)
					first = got;
				same &= got.groups == first.groups && got.counts == first.counts;
			}
		}
		printf("%-28s %9.1f %9.1f %9.1f %6.2fx %6.2fx  %s (%lu groups)\n", w->name, best[0] * 1e3, best[1] * 1e3, best[2] * 1e3, best[0] / best[1], best[0] / best[2],
			   same ? "same" : "DIFFERENT", (unsigned long) first.groups);
		CHECK(same);
		data_free(d);
	}
	return 0;
}
