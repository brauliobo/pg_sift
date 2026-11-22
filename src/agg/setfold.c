/*
 * agg/setfold.c - grouping sets summed from the finest one
 */
#include "postgres.h"

#include "agg/agg_int.h"
#include "agg/blob_int.h"
#include "agg/group_int.h"
#include "agg/update_int.h"
#include "core/popcount.h"

/*
 * A query whose grouping sets all lie inside one of them (ROLLUP, CUBE, and the sets of a report: (a, b), (a), (b), ()) needs the rows of the finest set only: the groups of
 * the others are sums of its groups. The scan updates that set (ex->fold = its number + 1) and sets_fold adds each of its groups into the group of every other set, with the merge
 * of two processes' tables, so the scan hashes a row once and updates the aggregates once instead of once per set. Aggregates whose states add exactly qualify: counts, integer
 * and numeric sums and averages, min, max (float sums would add in another order than the rows came in). Not count(DISTINCT x) of a unique column (SAA_COUNT_ROWS): over the
 * elements of a list a row has elements in several groups of the finest set and counts once in each, once in the coarser set.
 */
static bool
fold_kind(const SiftAggAgg *a)
{
	switch (a->kind)
	{
		case SAA_COUNT_STAR:
		case SAA_COUNT:
		case SAA_SUM_INT:
		case SAA_AVG_INT:
		case SAA_MIN:
		case SAA_MAX:
		case SAA_GROUPING:
			return true;
		case SAA_SUM_NUM:
		case SAA_AVG_NUM:
			return !a->distinct;
		default:
			return false;
	}
}

/* before the scan, in the leader and in every worker: the set the others lie inside, if the query allows folding */
void
sets_fold_begin(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	int			finest = -1;

	ex->fold = 0;
	if (s->nsets < 2 || s->narms > 0 || s->spine || s->setaggs || s->setpred || s->nhidden > 0)
		return;
	for (int i = 0; i < s->naggs; i++)
		/* count(DISTINCT id) of a unique id over the elements of an array (s->group): a row with elements in two groups is in both, and once in a coarser group, which is no sum of them */
		if (!fold_kind(&s->agg[i]) || s->agg[i].filter || (s->agg[i].kind == SAA_COUNT_ROWS && s->group >= 0))
			return;
	for (int set = 0; set < s->nsets; set++)
	{
		bool		all = true;

		if (set_has_post(s, set))
			return;
		for (int c = 0; c < s->nsets; c++)
			all &= (s->setmask[c] & ~s->setmask[set]) == 0;
		if (all && finest < 0)
			finest = set;
	}
	ex->fold = finest + 1;
}

static uint32
groups_now(const AggExec *ex)
{
	return ex->hash ? ex->ngroups : ex->gcap;
}

static int
bits_of(uint32 mask)
{
	return (int) sift_pc64(mask);
}

/* the smallest set among those whose groups are made (done) that contains the set c: ROLLUP and CUBE sets are folded from the next finer one, whose groups are fewer than the finest set's */
static int
fold_parent(const SiftAggSpec *s, int c, const bool *done)
{
	int			best = -1;

	for (int p = 0; p < s->nsets; p++)
		if (done[p] && (s->setmask[c] & ~s->setmask[p]) == 0 && (best < 0 || bits_of(s->setmask[p]) < bits_of(s->setmask[best])))
			best = p;
	return best;
}

/* the sets from the finest to the coarsest, by the keys they group by */
static int
fold_order_cmp(const void *a, const void *b, void *arg)
{
	const SiftAggSpec *s = arg;
	const int	x = bits_of(s->setmask[*(const int *) a]),
				y = bits_of(s->setmask[*(const int *) b]);

	return y != x ? y - x : *(const int *) a - *(const int *) b;
}

/* after the scan and the merge of the workers' tables: the groups of the other sets */
void
sets_fold(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	int			nk = s->nkeys;
	uint32		n = groups_now(ex),
			   *from,
			   *to;
	uint32		nf = 0,
				tocap = Max(n, 1);
	int		   *order;
	uint32	  **grp,
			   *ngrp;
	bool	   *done;

	if (!ex->fold)
		return;
	order = palloc(s->nsets * sizeof(int));
	grp = palloc0(s->nsets * sizeof(uint32 *));
	ngrp = palloc0(s->nsets * sizeof(uint32));
	done = palloc0(s->nsets * sizeof(bool));
	from = palloc(Max(n, 1) * sizeof(uint32));
	to = palloc0(tocap * sizeof(uint32));
	for (uint32 g = 0; g < n; g++)
		if (ex->seen[g] > 0 && key_id(ex, nk - 1, g) == (uint32) ex->fold)
			from[nf++] = g;
	grp[ex->fold - 1] = from;
	ngrp[ex->fold - 1] = nf;
	done[ex->fold - 1] = true;
	for (int set = 0; set < s->nsets; set++)
		order[set] = set;
	qsort_arg(order, s->nsets, sizeof(int), fold_order_cmp, s);
	for (int o = 0; o < s->nsets; o++)
	{
		const int	set = order[o];
		const int	parent = set == ex->fold - 1 ? -1 : fold_parent(s, set, done);
		const uint32 *src;
		uint32		nsrc,
					live;
		uint64	   *marked;

		if (parent < 0)
			continue;
		src = grp[parent];
		nsrc = ngrp[parent];
		if (groups_now(ex) > tocap)
		{
			const uint32 ncap = groups_now(ex);

			to = repalloc(to, ncap * sizeof(uint32));
			memset(to + tocap, 0, (ncap - tocap) * sizeof(uint32));
			tocap = ncap;
		}
		for (uint32 i = 0; i < nsrc; i++)
		{
			uint32		ids[SIFT_AGG_MAX_KEYS];

			for (int j = 0; j < nk - 1; j++)
				ids[j] = (s->setmask[set] >> j & 1) ? key_id(ex, j, src[i]) : 0;
			ids[nk - 1] = set + 1;
			to[src[i]] = ids_gid(ex, ids);
		}
		if (ex->hash && ex->ngroups > ex->gcap)
			groups_resize(ex, Max(ex->gcap * 2, ex->ngroups), NULL, ex->gcap);
		blob_fold(ex, src, to, nsrc);
		live = groups_now(ex);
		marked = palloc0(((live + 63) >> 6) * sizeof(uint64));
		grp[set] = palloc(Max(nsrc, 1) * sizeof(uint32));
		for (uint32 i = 0; i < nsrc; i++)
		{
			const uint32 g = to[src[i]];

			if (!(marked[g >> 6] >> (g & 63) & 1))
			{
				marked[g >> 6] |= UINT64CONST(1) << (g & 63);
				grp[set][ngrp[set]++] = g;
			}
		}
		pfree(marked);
		done[set] = true;
	}
	for (int set = 0; set < s->nsets; set++)
		if (grp[set])
			pfree(grp[set]);
	pfree(grp);
	pfree(ngrp);
	pfree(done);
	pfree(order);
	pfree(to);
}
