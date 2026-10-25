/*
 * agg/gord.c - the merge of the global order of a leaf over the sources of a scan (agg/gord.h).
 */

#include "postgres.h"

#include "col/gorder.h"
#include "agg/gord.h"
#include "core/cmp.h"

/* A cursor over one source: the entries in the order of the scan, one chunk decoded at a time */
typedef struct GCur
{
	int			src;
	uint32		entries;
	uint32		values;
	uint32		vs;				/* the values in the range of the scan: [vs, ve) */
	uint32		ve;
	uint32		ns;				/* the NULL rows of the group of the scan: [ns, ne) */
	uint32		ne;
	uint32		chunk;			/* the chunk in key/addr, UINT32_MAX: none */
	uint32		cm;
	int			region;			/* 0, 1: the regions in the order of the scan; 2: done */
	uint32		pos;			/* the next entry of the region (a group of a descending scan: pos in [gs, ge)) */
	uint32		end;
	uint32		gs;
	uint32		ge;
	bool		have;			/* cls, k and tid are the cursor's entry */
	int			cls;			/* 0: NULL rows first, 1: values, 2: NULL rows last */
	int64		k;
	uint64		tid;
	int64		key[SIFT_CGORD_CAP];
	uint64		addr[SIFT_CGORD_CAP];
} GCur;

struct SiftGOrd
{
	SiftSrcSource *src;
	int			leaf;
	bool		desc;
	bool		nulls_first;
	int			grouped;		/* the leaf the arrays are grouped by (the scan reads the group of its condition), -1 */
	bool		bounded;		/* a range of the key: the rows with no value are not in it */
	int			ncur;
	GCur	  **cur;
	bool		started;
	bool		last_set;
	int			last_cls;
	int64		last_key;
	uint64		last_tid;
};

/* the entry p of the cursor's source: its chunk is decoded when it is not the one at hand */
static void
cur_at(SiftGOrd *g, GCur *c, uint32 p, int64 *key, uint64 *addr)
{
	uint32		ch = p / SIFT_CGORD_ROWS;

	if (c->chunk != ch)
	{
		c->cm = sift_colsrc_gorder_chunk(g->src, c->src, g->leaf, ch, c->key, c->addr);
		c->chunk = ch;
	}
	*key = c->key[p % SIFT_CGORD_ROWS];
	*addr = c->addr[p % SIFT_CGORD_ROWS];
}

/* a descending scan reads the entries of a key in the order of their addresses, the keys backwards: the group [gs, ge) ends where the last one starts */
static bool
cur_group(SiftGOrd *g, GCur *c)
{
	int64		k,
				k2;
	uint64		a;

	if (c->ge <= c->vs)
		return false;
	cur_at(g, c, c->ge - 1, &k, &a);
	c->gs = c->ge - 1;
	while (c->gs > c->vs)
	{
		cur_at(g, c, c->gs - 1, &k2, &a);
		if (k2 != k)
			break;
		c->gs--;
	}
	c->pos = c->gs;
	return true;
}

static void
cur_enter(SiftGOrd *g, GCur *c)
{
	bool		nulls = (c->region == 0) == g->nulls_first;		/* the first region holds the NULL rows when they come first */

	if (nulls)
	{
		c->pos = g->bounded ? 0 : c->ns;
		c->end = g->bounded ? 0 : c->ne;
	}
	else if (g->desc)
	{
		c->ge = c->ve;
		c->end = cur_group(g, c) ? c->ge : 0;
		c->pos = c->end == 0 ? 0 : c->gs;
	}
	else
	{
		c->pos = c->vs;
		c->end = c->ve;
	}
}

static void
cur_next(SiftGOrd *g, GCur *c)
{
	for (;;)
	{
		bool		nulls;
		int64		k;
		uint64		a;

		if (c->region >= 2)
		{
			c->have = false;
			return;
		}
		nulls = (c->region == 0) == g->nulls_first;
		if (c->pos < c->end)
		{
			cur_at(g, c, c->pos++, &k, &a);
			c->have = true;
			c->cls = nulls ? (g->nulls_first ? 0 : 2) : 1;
			c->k = nulls ? 0 : k;
			c->tid = a;
			return;
		}
		if (!nulls && g->desc && c->end > 0)
		{
			/* the group is read: the next one ends where this began */
			c->ge = c->gs;
			if (cur_group(g, c))
			{
				c->end = c->ge;
				continue;
			}
		}
		c->region++;
		if (c->region < 2)
			cur_enter(g, c);
	}
}

static void
cur_start(SiftGOrd *g, GCur *c)
{
	c->chunk = UINT32_MAX;
	c->region = 0;
	c->have = false;
	cur_enter(g, c);
	cur_next(g, c);
}

SiftGOrd *
sift_gord_open(SiftSrcSource *src, int leaf, int rows, bool desc, bool nulls_first, const SiftGBound *b, MemoryContext cxt)
{
	int			grouped,
				n = sift_colsrc_gorder_open(src, leaf, rows, b != NULL ? b->npin : 0, b != NULL ? b->pinleaf : NULL, b != NULL ? b->pinval : NULL, &grouped);
	SiftGOrd   *g;

	if (n < 0)
		return NULL;
	g = MemoryContextAllocZero(cxt, sizeof(SiftGOrd));
	g->src = src;
	g->leaf = leaf;
	g->grouped = grouped;
	g->desc = desc;
	g->nulls_first = nulls_first;
	g->bounded = b != NULL && (b->has_lo || b->has_hi);
	g->cur = MemoryContextAlloc(cxt, Max(n, 1) * sizeof(GCur *));
	for (int i = 0; i < n; i++)
	{
		GCur	   *c = MemoryContextAllocZero(cxt, sizeof(GCur));
		uint32		rvs,
					rve;

		c->src = i;
		if (!sift_colsrc_gorder_info(src, i, leaf, &c->entries, &c->values) || c->entries == 0)
			continue;
		sift_colsrc_gorder_range(src, i, leaf, &rvs, &rve, &c->ns, &c->ne);
		c->vs = g->bounded && b->has_lo ? sift_colsrc_gorder_seek(src, i, leaf, rvs, rve, b->lo, false) : rvs;
		c->ve = g->bounded && b->has_hi ? sift_colsrc_gorder_seek(src, i, leaf, rvs, rve, b->hi, true) : rve;
		if (g->bounded && b->has_lo && b->has_hi && b->lo > b->hi)
			continue;
		if (c->vs >= c->ve && (g->bounded || c->ns >= c->ne))
			continue;
		g->cur[g->ncur++] = c;
	}
	return g;
}

void
sift_gord_rewind(SiftGOrd *g)
{
	g->started = false;
	g->last_set = false;
}

/* < 0: a comes before b in the scan */
static int
cand_cmp(const SiftGOrd *g, const GCur *a, const GCur *b)
{
	if (a->cls != b->cls)
		return a->cls < b->cls ? -1 : 1;
	if (a->k != b->k)
		return (a->k < b->k) != g->desc ? -1 : 1;
	return SIFT_CMP3(a->tid, b->tid);
}

int
sift_gord_group(const SiftGOrd *g)
{
	return g->grouped;
}

bool
sift_gord_next(SiftGOrd *g, int64 *key, bool *isnull, ItemPointerData *tid)
{
	if (!g->started)
	{
		for (int i = 0; i < g->ncur; i++)
			cur_start(g, g->cur[i]);
		g->started = true;
	}
	for (;;)
	{
		GCur	   *best = NULL;

		for (int i = 0; i < g->ncur; i++)
			if (g->cur[i]->have && (best == NULL || cand_cmp(g, g->cur[i], best) < 0))
				best = g->cur[i];
		if (best == NULL)
			return false;
		*key = best->k;
		*isnull = best->cls != 1;
		ItemPointerSet(tid, (BlockNumber) (best->tid >> 16), (OffsetNumber) (best->tid & 0xFFFF));
		if (g->last_set && g->last_cls == best->cls && g->last_key == best->k && g->last_tid == best->tid)
		{
			cur_next(g, best);
			continue;
		}
		g->last_set = true;
		g->last_cls = best->cls;
		g->last_key = best->k;
		g->last_tid = best->tid;
		cur_next(g, best);
		return true;
	}
}
