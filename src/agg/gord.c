/*
 * agg/gord.c - the merge of the global order of a leaf over the sources of a scan (agg/gord.h).
 */

#include "postgres.h"

#include "col/gorder.h"
#include "agg/gord.h"
#include "core/cmp.h"

#define GORD_HEAD_MIN	32

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
	uint32		cm;				/* the entries of the chunk decoded: the first cm, the scan asks for more when it goes past them */
	int			region;			/* 0, 1: the regions in the order of the scan; 2: done */
	uint32		pos;			/* the next entry of the region (a group of a descending scan: pos in [gs, ge)) */
	uint32		end;
	uint32		gs;
	uint32		ge;
	bool		have;			/* cls, k and tid are the cursor's entry */
	int			cls;			/* 0: NULL rows first, 1: values, 2: NULL rows last */
	int64		k;
	uint64		tid;
	int64	   *key;			/* a GBuf of the pool */
	uint64	   *addr;
} GCur;

typedef struct GBuf
{
	int64		key[SIFT_CGORD_CAP];
	uint64		addr[SIFT_CGORD_CAP];
} GBuf;

/*
 * The 16 KB of keys and addresses of a cursor are the largest buffers of a lookup, and a block of that size is a malloc of its own that goes back to the kernel with
 * the top of the heap when the statement ends (24 page faults for 6 segments, a fifth of the time of a lookup of 0.5 million instructions). The backend keeps the blocks
 * it has used, and a scan gets them back through the reset callback of its context.
 */
#define GBUF_KEEP	64
static MemoryContext gbuf_cx;
static GBuf *gbuf_spare[GBUF_KEEP];
static int	ngbuf_spare;

static GBuf *
gbuf_get(void)
{
	if (ngbuf_spare > 0)
		return gbuf_spare[--ngbuf_spare];
	if (gbuf_cx == NULL)
		gbuf_cx = AllocSetContextCreate(TopMemoryContext, "sift cache cursors", ALLOCSET_DEFAULT_SIZES);
	return MemoryContextAlloc(gbuf_cx, sizeof(GBuf));
}

struct SiftGOrd
{
	SiftSrcSource *src;
	int			leaf;
	bool		desc;
	bool		nulls_first;
	int			grouped;		/* the leaf the arrays are grouped by (the scan reads the group of its condition), -1 */
	bool		bounded;		/* a range of the key: the rows with no value are not in it */
	uint32		head;			/* the entries of a chunk decoded when the scan enters it (the rows asked for, shared by the sources, at least GORD_HEAD_MIN) */
	int			ncur;
	GCur	  **cur;
	bool		started;
	GCur	   *prev;			/* the source of the last entry given, and the best of the others at that time (NULL: the sources are scanned) */
	GCur	   *runner;
	bool		last_set;
	int			last_cls;
	int64		last_key;
	uint64		last_tid;
	MemoryContextCallback release;
};

static void
gord_release(void *arg)
{
	SiftGOrd   *g = arg;

	for (int i = 0; i < g->ncur; i++)
	{
		if (ngbuf_spare < GBUF_KEEP)
			gbuf_spare[ngbuf_spare++] = (GBuf *) g->cur[i]->key;
		else
			pfree(g->cur[i]->key);
	}
}

/* the chunk of the entry p is decoded as far as p, doubling when the scan goes on (a few rows of a big table decode a few entries) */
static pg_noinline void
cur_fill(SiftGOrd *g, GCur *c, uint32 p)
{
	uint32		ch = p / SIFT_CGORD_ROWS,
				w = Max(c->chunk == ch ? c->cm * 2 : g->head, p % SIFT_CGORD_ROWS + 1);

	c->cm = sift_colsrc_gorder_head(g->src, c->src, g->leaf, ch, Min(w, (uint32) SIFT_CGORD_ROWS), c->key, c->addr);
	c->chunk = ch;
}

/* the entry p of the cursor's source */
static inline void
cur_at(SiftGOrd *g, GCur *c, uint32 p, int64 *key, uint64 *addr)
{
	if (c->chunk != p / SIFT_CGORD_ROWS || p % SIFT_CGORD_ROWS >= c->cm)
		cur_fill(g, c, p);
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
	g->release.func = gord_release;
	g->release.arg = g;
	MemoryContextRegisterResetCallback(cxt, &g->release);
	for (int i = 0; i < n; i++)
	{
		GCur	   *c = MemoryContextAlloc(cxt, sizeof(GCur));
		GBuf	   *gb;
		uint32		rvs,
					rve;

		memset(c, 0, sizeof(GCur));
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
		c->key = (int64 *) (gb = gbuf_get())->key;
		c->addr = gb->addr;
		g->cur[g->ncur++] = c;
	}
	g->head = Max(GORD_HEAD_MIN, Min((uint32) SIFT_CGORD_ROWS, g->ncur > 0 ? 5 * (uint32) Max(rows, 0) / (4 * g->ncur) : 0));
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
		g->prev = NULL;
	}
	for (;;)
	{
		GCur	   *best = g->prev;

		/*
		 * The source that gave the last entry stays the best while its next one comes before the runner-up of the others (they have not moved): one comparison for a row
		 * where the scan of every source costs one for each
		 */
		if (best == NULL || !best->have || (g->runner != NULL && cand_cmp(g, best, g->runner) >= 0))
		{
			best = g->runner = NULL;
			for (int i = 0; i < g->ncur; i++)
			{
				GCur	   *c = g->cur[i];

				if (!c->have)
					continue;
				if (best == NULL || cand_cmp(g, c, best) < 0)
				{
					g->runner = best;
					best = c;
				}
				else if (g->runner == NULL || cand_cmp(g, c, g->runner) < 0)
					g->runner = c;
			}
		}
		if (best == NULL)
			return false;
		*key = best->k;
		*isnull = best->cls != 1;
		ItemPointerSet(tid, (BlockNumber) (best->tid >> 16), (OffsetNumber) (best->tid & 0xFFFF));
		/* a source holds an entry once: only the entry of another source can repeat the last one */
		if (best != g->prev && g->last_set && g->last_cls == best->cls && g->last_key == best->k && g->last_tid == best->tid)
		{
			g->prev = best;
			cur_next(g, best);
			continue;
		}
		g->prev = best;
		g->last_set = true;
		g->last_cls = best->cls;
		g->last_key = best->k;
		g->last_tid = best->tid;
		cur_next(g, best);
		return true;
	}
}
