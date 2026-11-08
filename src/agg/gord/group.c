/*
 * agg/gord/group.c - the cursors of the global order that read the second order of another leaf group after group (ORDER BY group, key), and the skip of an OFFSET over them.
 */
#include "postgres.h"
#include "core/rowmem.h"

#include "agg/gord/int.h"

/* The next group of a source read group after group (descending: the last first) with entries, and those entries to read; false at the end */
bool
gord_group_load(SiftGOrd *g, GCur *c)
{
	while (c->gnext < c->ngrp)
	{
		uint32		vs,
					ve;

		sift_colsrc_gorder_group_at(g->src, c->src, g->leaf, g->desc ? c->ngrp - 1 - c->gnext : c->gnext, &c->gval, &vs, &ve, &c->ns, &c->ne);
		c->gnext++;
		c->pos = vs;
		c->end = ve;
		c->gnull = false;
		if (ve > vs || c->ne > c->ns)
			return true;
	}
	return false;
}

/*
 * The next entry of a source read group after group: the entries of the group (its value entries, then the NULL-key ones, in no order that matters: the entries of one key are put
 * in order by whoever reads them), each with the group for its key. The second order lists the groups in ascending order; a descending scan takes them from the last.
 */
void
gord_group_next(SiftGOrd *g, GCur *c)
{
	for (;;)
	{
		int64		k;
		uint64		a;

		if (c->pos < c->end)
		{
			cur_at(g, c, c->pos++, &k, &a);
			c->have = true;
			c->cls = 1;
			c->k = c->gval;
			c->tid = a;
			return;
		}
		if (!c->gnull && c->ne > c->ns)
		{
			c->gnull = true;
			c->pos = c->ns;
			c->end = c->ne;
			continue;
		}
		if (!gord_group_load(g, c))
		{
			c->have = false;
			c->region = 2;
			return;
		}
	}
}

/* The entries of the groups of the source that come before the groups with v and after it (ascending: the groups below v; descending: those above) in the scan, and the groups they are in */
static uint32
gm_before(SiftGOrd *g, GCur *c, int64 v, uint32 *groups)
{
	uint32		lo = 0,
				hi = c->ngrp,
				idx,
				vs,
				ve,
				ns,
				ne,
				prefix;
	int64		grp;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		sift_colsrc_gorder_group_at(g->src, c->src, g->leaf, mid, &grp, &vs, &ve, &ns, &ne);
		if (g->desc ? grp <= v : grp < v)
			lo = mid + 1;
		else
			hi = mid;
	}
	idx = lo;
	if (idx >= c->ngrp)
		prefix = c->entries;
	else
	{
		sift_colsrc_gorder_group_at(g->src, c->src, g->leaf, idx, &grp, &vs, &ve, &ns, &ne);
		prefix = vs + (ns - c->values);
	}
	*groups = g->desc ? c->ngrp - idx : idx;
	return g->desc ? c->entries - prefix : prefix;
}

/*
 * The skip of an OFFSET over sources read group after group: the groups before a group value v are all among the first k when at most k entries come before v (the sum over the sources),
 * and their entries are the first of the scan whatever the order within a group. v is the largest group value of the first source for which that holds. An entry is counted when it stands
 * for a row (sift_colsrc_gorder_state), listed for the heap to confirm otherwise, as in sift_gord_skip; a group with an entry that cannot say is left to the walk, with the groups after it.
 */
uint32
gord_group_skip(SiftGOrd *g, uint32 k)
{
	uint32		lo = 0,
				hi,
				total = 0;
	uint32	   *take;
	int64		v;
	uint64	   *addr = NULL;
	uint32		room = 0;

	for (int i = 0; i < g->ncur; i++)
		if (!g->cur[i]->gm || g->cur[i]->gskip != 0)
			return 0;
	if (g->npend != 0)
		return 0;
	if (g->cur[0]->ngrp == 0)
		return 0;
	hi = g->cur[0]->ngrp - 1;
	take = palloc(g->ncur * sizeof(uint32));
	/* the largest place gi (in the order of the scan) of the first source whose group has at most k entries before it, in all sources */
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo + 1) / 2,
					n = 0,
					tmp;
		uint32		vs,
					ve,
					ns,
					ne;
		int64		grp;

		sift_colsrc_gorder_group_at(g->src, g->cur[0]->src, g->leaf, g->desc ? g->cur[0]->ngrp - 1 - mid : mid, &grp, &vs, &ve, &ns, &ne);
		for (int i = 0; i < g->ncur; i++)
			n += gm_before(g, g->cur[i], grp, &tmp);
		if (n <= k)
			lo = mid;
		else
			hi = mid - 1;
	}
	if (lo == 0)
	{
		pfree(take);
		return 0;
	}
	{
		uint32		vs,
					ve,
					ns,
					ne;

		sift_colsrc_gorder_group_at(g->src, g->cur[0]->src, g->leaf, g->desc ? g->cur[0]->ngrp - 1 - lo : lo, &v, &vs, &ve, &ns, &ne);
	}
	for (int i = 0; i < g->ncur; i++)
		(void) gm_before(g, g->cur[i], v, &take[i]);
	for (int i = 0; i < g->ncur; i++)
	{
		GCur	   *c = g->cur[i];

		for (uint32 n = 0; n < take[i]; n++)
		{
			uint32		vs,
						ve,
						ns,
						ne,
						m;
			int64		grp;
			int		   *st;
			bool		stuck = false;

			sift_colsrc_gorder_group_at(g->src, c->src, g->leaf, g->desc ? c->ngrp - 1 - n : n, &grp, &vs, &ve, &ns, &ne);
			m = (ve - vs) + (ne - ns);
			if (m > room)
			{
				room = Max(m, room * 2);
				addr = addr == NULL ? sift_rows_alloc(room * sizeof(uint64)) : repalloc_huge(addr, room * sizeof(uint64));
			}
			st = sift_rows_alloc(Max(m, 1) * sizeof(int));
			for (uint32 e = 0; e < m && !stuck; e++)
			{
				uint32		p = e < ve - vs ? vs + e : ns + (e - (ve - vs));
				int64		key;

				cur_at(g, c, p, &key, &addr[e]);
				st[e] = sift_colsrc_gorder_state(g->src, c->src, addr[e]);
				stuck = st[e] == SIFT_GORD_UNKNOWN || (st[e] == SIFT_GORD_CHECK && g->npend + m > GORD_PEND_MAX);
			}
			if (stuck)
			{
				pfree(st);
				break;
			}
			for (uint32 e = 0; e < m; e++)
			{
				if (st[e] == SIFT_GORD_EXACT)
					total++;
				else if (st[e] == SIFT_GORD_CHECK)
				{
					if (g->npend == g->cpend)
					{
						g->cpend = Max(g->cpend * 2, 1024);
						g->pend = g->pend == NULL ? MemoryContextAlloc(g->cxt, g->cpend * sizeof(SiftGPend)) : repalloc(g->pend, g->cpend * sizeof(SiftGPend));
					}
					g->pend[g->npend].tid = addr[e];
					g->pend[g->npend].key = grp;
					g->pend[g->npend].cur = i;
					g->pend[g->npend++].check = true;
				}
			}
			pfree(st);
			c->gskip = n + 1;
		}
	}
	if (addr != NULL)
		pfree(addr);
	pfree(take);
	return total;
}
