/*
 * agg/dist/finish.c - count(DISTINCT): the result and the pairs of a set
 * The count of each group at the end, the walk of the pairs of a set for the partitions,
 * and the end of the sets.
 */
#include "postgres.h"

#include "utils/tuplesort.h"

#include "agg/dist/int.h"

typedef struct DistPairs
{
	const uint32 *cidx;
	uint32	   *pg;
	uint32	   *pv;
	uint32		np;
} DistPairs;

typedef struct DistEach
{
	const uint32 *sg;
	DistFn		fn;
	void	   *arg;
} DistEach;

typedef struct PairSplit
{
	const uint32 *gpart;
	const uint32 *cidx;
	PartPairs  *out;
	int			stride;
} PairSplit;

/* DD_SORT: calls fn(arg, sid, value) for each distinct pair of the sort, in its order */
static void
dist_sorted(Dist *d, void (*fn) (void *arg, int32 sid, int64 val), void *arg)
{
	Datum		x;
	bool		isnull;
	int64		prev = -1;

	tuplesort_rescan(d->sort);
	while (tuplesort_getdatum(d->sort, true, false, &x, &isnull, NULL))
	{
		if (DatumGetInt64(x) != prev)
			fn(arg, (int32) (DatumGetInt64(x) >> 32), (uint32) DatumGetInt64(x));
		prev = DatumGetInt64(x);
	}
}

static void
dist_count_sid(void *arg, int32 sid, int64 val)
{
	((Dist *) arg)->cnt[sid]++;
}

/* DD_SORT: sorts the pairs and sets the count of each group */
static void
dist_finish(AggExec *ex, int ai)
{
	Dist	   *d = ex->dist[ai];
	AggAcc	   *acc = &ex->acc[ai];
	uint32		ngr = ex->hash ? ex->ngroups : ex->gcap;

	if (d == NULL || d->mode != DD_SORT || d->done)
		return;
	tuplesort_performsort(d->sort);
	d->cnt = MemoryContextAllocZero(ex->tcxt, Max(d->nsid, 1) * sizeof(uint64));
	dist_sorted(d, dist_count_sid, d);
	for (uint32 g = 0; g < Min(ngr, d->sidgroups); g++)
		if (d->sid[g])
			acc->n[g] = d->cnt[d->sid[g] - 1];
	d->done = true;
}

void
dist_finish_all(AggExec *ex)
{
	for (int ai = 0; ai < ex->spec->naggs; ai++)
		dist_finish(ex, ai);
}

/* the sorts of the tables about to be forgotten */
void
dist_end_all(AggExec *ex)
{
	if (ex->dist == NULL)
		return;
	for (int ai = 0; ai < ex->spec->naggs; ai++)
		if (ex->dist[ai] && ex->dist[ai]->sort)
		{
			tuplesort_end(ex->dist[ai]->sort);
			ex->dist[ai]->sort = NULL;
		}
}

static void
dist_collect(void *arg, uint32 g, uint32 v)
{
	DistPairs  *dp = arg;

	if (dp->cidx[g] == UINT32_MAX)
		return;
	dp->pg[dp->np] = dp->cidx[g];
	dp->pv[dp->np++] = v;
}

static void
dist_each_sid(void *arg, int32 sid, int64 val)
{
	DistEach   *e = arg;

	e->fn(e->arg, e->sg[sid], (uint32) val);
}

/* fn(arg, group, value id) for each distinct pair of aggregate ai, from the sort or the set */
static void
dist_each(AggExec *ex, int ai, DistFn fn, void *arg)
{
	Dist	   *d = ex->dist[ai];

	dist_finish(ex, ai);
	if (d->mode == DD_SORT)
	{
		uint32		n = ex->hash ? ex->ngroups : ex->gcap;
		uint32	   *sg = palloc0(Max(d->nsid, 1) * sizeof(uint32));
		DistEach	e = {sg, fn, arg};

		for (uint32 g = 0; g < Min(n, d->sidgroups); g++)
			if (d->sid[g])
				sg[d->sid[g] - 1] = g;
		dist_sorted(d, dist_each_sid, &e);
		pfree(sg);
	}
	else
		dist_walk(d, fn, arg);
}

/* the pairs of aggregate ai in the groups of cidx (blob numbering): the arrays pg and pv, np of them */
void
dist_pairs(AggExec *ex, int ai, const uint32 *cidx, uint32 **pg_out, uint32 **pv_out, uint32 *np_out)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap;
	uint64		total = 0;
	DistPairs	dp = {cidx, NULL, NULL, 0};

	dist_finish(ex, ai);
	for (uint32 g = 0; g < n; g++)
		if (cidx[g] != UINT32_MAX)
			total += ex->acc[ai].n[g];
	dp.pg = palloc(Max(total, 1) * sizeof(uint32));
	dp.pv = palloc(Max(total, 1) * sizeof(uint32));
	dist_each(ex, ai, dist_collect, &dp);
	*pg_out = dp.pg;
	*pv_out = dp.pv;
	*np_out = dp.np;
}

/* The (group of cidx, value id) pairs of the set of a sum or average over distinct values (ex->dset[ai]), as dist_pairs gives those of a count */
void
dset_pairs(AggExec *ex, int ai, const uint32 *cidx, uint32 **pg_out, uint32 **pv_out, uint32 *np_out)
{
	const DSet *s = &ex->dset[ai];
	uint32		np = 0;

	*pg_out = palloc(Max(s->n, 1) * sizeof(uint32));
	*pv_out = palloc(Max(s->n, 1) * sizeof(uint32));
	for (uint64 i = 0; i < s->cap; i++)
		if (s->slot[i] && cidx[(s->slot[i] >> 32) & 0x7fffffff] != UINT32_MAX)
		{
			(*pg_out)[np] = cidx[(s->slot[i] >> 32) & 0x7fffffff];
			(*pv_out)[np++] = (uint32) s->slot[i];
		}
	*np_out = np;
}

static void
pairs_put(PartPairs *pp, uint32 g, uint32 v)
{
	if (pp->np == pp->cap)
	{
		pp->cap = pp->cap ? pp->cap * 2 : 64;
		pp->pg = pp->pg ? repalloc(pp->pg, pp->cap * sizeof(uint32)) : palloc(pp->cap * sizeof(uint32));
		pp->pv = pp->pv ? repalloc(pp->pv, pp->cap * sizeof(uint32)) : palloc(pp->cap * sizeof(uint32));
	}
	pp->pg[pp->np] = g;
	pp->pv[pp->np++] = v;
}

static void
split_collect(void *arg, uint32 g, uint32 v)
{
	PairSplit  *sp = arg;

	if (sp->gpart[g] != UINT32_MAX)
		pairs_put(&sp->out[(Size) sp->gpart[g] * sp->stride], sp->cidx[g], v);
}

/*
 * The pairs of the set of aggregate ai of every partition of a spilled table, from one walk of the set: gpart[g] is the partition of group g
 * (UINT32_MAX: not written) and cidx[g] its number in the blob of that partition; partition p's pairs go to out[p * stride]. A walk for each
 * partition costs the whole set each time.
 */
void
dist_pairs_split(AggExec *ex, int ai, const uint32 *gpart, const uint32 *cidx, PartPairs *out, int stride)
{
	PairSplit	sp = {gpart, cidx, out, stride};

	dist_each(ex, ai, split_collect, &sp);
}

/* the same for the set of a sum or average over distinct numbers */
void
dset_pairs_split(AggExec *ex, int ai, const uint32 *gpart, const uint32 *cidx, PartPairs *out, int stride)
{
	const DSet *s = &ex->dset[ai];
	PairSplit	sp = {gpart, cidx, out, stride};

	for (uint64 i = 0; i < s->cap; i++)
		if (s->slot[i])
			split_collect(&sp, (s->slot[i] >> 32) & 0x7fffffff, (uint32) s->slot[i]);
}
