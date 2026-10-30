/*
 * agg/setfold.c - grouping sets summed from the finest one
 */
#include "postgres.h"

#include "agg/agg_int.h"
#include "agg/blob_int.h"
#include "agg/group_int.h"
#include "agg/update_int.h"

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
		if (!fold_kind(&s->agg[i]) || s->agg[i].filter)
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

/* after the scan and the merge of the workers' tables: the groups of the other sets */
void
sets_fold(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	int			nk = s->nkeys;
	uint32		n = ex->hash ? ex->ngroups : ex->gcap,
			   *from,
			   *to;
	uint32		nf = 0;

	if (!ex->fold)
		return;
	from = palloc(Max(n, 1) * sizeof(uint32));
	to = palloc0(Max(n, 1) * sizeof(uint32));
	for (uint32 g = 0; g < n; g++)
		if (ex->seen[g] > 0 && key_id(ex, nk - 1, g) == (uint32) ex->fold)
			from[nf++] = g;
	for (int set = 0; set < s->nsets; set++)
	{
		if (set == ex->fold - 1)
			continue;
		for (uint32 i = 0; i < nf; i++)
		{
			uint32		ids[SIFT_AGG_MAX_KEYS];

			for (int j = 0; j < nk - 1; j++)
				ids[j] = (s->setmask[set] >> j & 1) ? key_id(ex, j, from[i]) : 0;
			ids[nk - 1] = set + 1;
			to[from[i]] = ids_gid(ex, ids);
		}
		if (ex->hash && ex->ngroups > ex->gcap)
			groups_resize(ex, Max(ex->gcap * 2, ex->ngroups), NULL, ex->gcap);
		blob_fold(ex, from, to, nf);
	}
	pfree(from);
	pfree(to);
}
