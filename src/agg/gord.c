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
	int			slot;			/* its place in the cursors of the scan (the leaf of the tournament of a text key) */
	uint32		entries;
	uint32		values;
	uint32		vs;				/* the values in the range of the scan: [vs, ve) */
	uint32		ve;
	uint32		ns;				/* the NULL rows of the group of the scan: [ns, ne) */
	uint32		ne;
	int			region;			/* 0, 1: the regions in the order of the scan; 2: done */
	uint32		pos;			/* the next entry of the region (a group of a descending scan: pos in [gs, ge)) */
	uint32		end;
	uint32		gs;
	uint32		ge;
	bool		have;			/* cls, k and tid are the cursor's entry */
	int			cls;			/* 0: NULL rows first, 1: values, 2: NULL rows last */
	int64		k;
	uint64		tid;
	uint32		slen;			/* a text key: the string of k (str, slen bytes) */
	int64		skey;			/* ... and the key str is the string of (-1: none yet) */
	SiftCGOrdBuf *buf;			/* from the pool: the chunk of the entry in buf->key / buf->addr (a seek may have left it): the first buf->cm entries, the scan asks for more when it goes past them */
	char		str[SIFT_CGORD_STR_MAX];	/* last: a cursor clears the fields before it */
} GCur;

/*
 * The 16 KB of keys and addresses of a cursor are the largest buffers of a lookup, and a block of that size is a malloc of its own that goes back to the kernel with
 * the top of the heap when the statement ends (24 page faults for 6 segments, a fifth of the time of a lookup of 0.5 million instructions). The backend keeps the blocks
 * it has used, and a scan gets them back through the reset callback of its context.
 */
#define GBUF_KEEP	64
static MemoryContext gbuf_cx;
static SiftCGOrdBuf *gbuf_spare[GBUF_KEEP];
static int	ngbuf_spare;

static void
gbuf_put(SiftCGOrdBuf *b)
{
	if (ngbuf_spare < GBUF_KEEP)
		gbuf_spare[ngbuf_spare++] = b;
	else
		pfree(b);
}

static SiftCGOrdBuf *
gbuf_get(void)
{
	SiftCGOrdBuf *b;

	if (ngbuf_spare > 0)
		b = gbuf_spare[--ngbuf_spare];
	else
	{
		if (gbuf_cx == NULL)
			gbuf_cx = AllocSetContextCreate(TopMemoryContext, "sift cache cursors", ALLOCSET_DEFAULT_SIZES);
		b = MemoryContextAlloc(gbuf_cx, sizeof(SiftCGOrdBuf));
	}
	b->at = UINT32_MAX;
	b->cm = b->rows = 0;
	return b;
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
	int		   *tree;			/* a text key: the tournament of the cursors, tree[0] the winner and tree[1..] the loser of each match (log2 of the sources compares a step, where the scan of them compares each) */
	bool		last_set;
	int			last_cls;
	int64		last_key;
	uint64		last_tid;
	uint32		collid;			/* the keys are ranks of strings (a text key under this collation): entries compare by their strings, across sources */
	uint32		last_len;
	char		last_str[SIFT_CGORD_STR_MAX];
	MemoryContextCallback release;
};

static void
gord_release(void *arg)
{
	SiftGOrd   *g = arg;

	for (int i = 0; i < g->ncur; i++)
		gbuf_put(g->cur[i]->buf);
}

/* the chunk of the entry p is decoded as far as p, doubling when the scan goes on (a few rows of a big table decode a few entries) */
static pg_noinline void
cur_fill(SiftGOrd *g, GCur *c, uint32 p)
{
	uint32		w = p - c->buf->at < c->buf->rows ? c->buf->cm * 2 : g->head;

	sift_colsrc_gorder_head(g->src, c->src, g->leaf, p, w, c->buf);
}

/* the entry p of the cursor's source */
static inline void
cur_at(SiftGOrd *g, GCur *c, uint32 p, int64 *key, uint64 *addr)
{
	if (p - c->buf->at >= c->buf->cm)
		cur_fill(g, c, p);
	*key = c->buf->key[p - c->buf->at];
	*addr = c->buf->addr[p - c->buf->at];
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

/* a text key: the string of the entry's rank, fetched when the rank changes */
static pg_noinline void
cur_str(SiftGOrd *g, GCur *c, int64 k)
{
	uint32		len;
	const char *str;

	if (c->skey == k)
		return;
	str = sift_colsrc_gorder_str(g->src, c->src, g->leaf, k, &len);
	memcpy(c->str, str, len);
	c->slen = len;
	c->skey = k;
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
			if (unlikely(g->collid != 0) && !nulls)
				cur_str(g, c, k);
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
	c->region = 0;
	c->have = false;
	cur_enter(g, c);
	cur_next(g, c);
}

SiftGOrd *
sift_gord_open(SiftSrcSource *src, int leaf, int rows, bool desc, bool nulls_first, uint32 collid, const SiftGBound *b, MemoryContext cxt)
{
	int			grouped,
				n = sift_colsrc_gorder_open(src, leaf, rows, b != NULL ? b->npin : 0, b != NULL ? b->pinleaf : NULL, b != NULL ? b->pinval : NULL, &grouped, collid);
	SiftGOrd   *g;

	if (n < 0)
		return NULL;
	g = MemoryContextAllocZero(cxt, sizeof(SiftGOrd));
	g->src = src;
	g->leaf = leaf;
	g->grouped = grouped;
	g->desc = desc;
	g->nulls_first = nulls_first;
	g->collid = collid;
	g->bounded = b != NULL && (b->has_lo || b->has_hi);
	g->cur = MemoryContextAlloc(cxt, Max(n, 1) * sizeof(GCur *));
	g->release.func = gord_release;
	g->release.arg = g;
	MemoryContextRegisterResetCallback(cxt, &g->release);
	for (int i = 0; i < n; i++)
	{
		GCur	   *c = MemoryContextAlloc(cxt, sizeof(GCur));
		uint32		rvs,
					rve;

		memset(c, 0, offsetof(GCur, str));
		c->src = i;
		c->skey = -1;
		if (!sift_colsrc_gorder_info(src, i, leaf, &c->entries, &c->values) || c->entries == 0)
			continue;
		c->buf = gbuf_get();
		sift_colsrc_gorder_range(src, i, leaf, &rvs, &rve, &c->ns, &c->ne);
		c->vs = g->bounded && b->has_lo ? sift_colsrc_gorder_seek(src, i, leaf, rvs, rve, b->lo, false, desc ? NULL : c->buf) : rvs;
		c->ve = g->bounded && b->has_hi ? sift_colsrc_gorder_seek(src, i, leaf, rvs, rve, b->hi, true, desc ? c->buf : NULL) : rve;
		if ((g->bounded && b->has_lo && b->has_hi && b->lo > b->hi) || (c->vs >= c->ve && (g->bounded || c->ns >= c->ne)))
		{
			gbuf_put(c->buf);
			continue;
		}
		c->slot = g->ncur;
		g->cur[g->ncur++] = c;
	}
	g->head = Max(GORD_HEAD_MIN, Min((uint32) SIFT_CGORD_ROWS, g->ncur > 0 ? 5 * (uint32) Max(rows, 0) / (4 * g->ncur) : 0));
	return g;
}

bool
sift_gord_exact(const SiftGOrd *g, const ItemPointerData *tid)
{
	return g->prev != NULL && sift_colsrc_gorder_exact(g->src, g->prev->src, ItemPointerGetBlockNumber(tid));
}

void
sift_gord_rewind(SiftGOrd *g)
{
	g->started = false;
	g->last_set = false;
}

/* the order of the strings of a text key: the collation of the query, then the bytes (what the b-tree of the column does) */
static pg_noinline int
str_cmp(const SiftGOrd *g, const char *a, uint32 la, const char *b, uint32 lb)
{
	int			c = sift_cgord_strcmp != NULL ? sift_cgord_strcmp(a, la, b, lb, g->collid) : 0;

	if (c == 0)
		c = memcmp(a, b, Min(la, lb));
	return c != 0 ? c : SIFT_CMP3(la, lb);
}

/* < 0: a comes before b in the scan */
static pg_attribute_always_inline int
cand_cmp(const SiftGOrd *g, const GCur *a, const GCur *b, bool text)
{
	if (a->cls != b->cls)
		return a->cls < b->cls ? -1 : 1;
	if (text && a->cls == 1)
	{
		int			c = str_cmp(g, a->str, a->slen, b->str, b->slen);

		if (c != 0)
			return (c < 0) != g->desc ? -1 : 1;
	}
	else if (a->k != b->k)
		return (a->k < b->k) != g->desc ? -1 : 1;
	return SIFT_CMP3(a->tid, b->tid);
}

/* a cursor with no entry left loses every match */
static inline bool
text_before(const SiftGOrd *g, const GCur *a, const GCur *b)
{
	return a->have && (!b->have || cand_cmp(g, a, b, true) < 0);
}

/* the match of leaf slot j up to the root: the winner of the others replaces it where it loses */
static void
text_replay(SiftGOrd *g, int j)
{
	int			k = g->ncur,
				w = j;

	for (int node = (k + j) / 2; node >= 1; node /= 2)
		if (text_before(g, g->cur[g->tree[node]], g->cur[w]))
		{
			int			l = g->tree[node];

			g->tree[node] = w;
			w = l;
		}
	g->tree[0] = w;
}

static void
text_tournament(SiftGOrd *g)
{
	int			k = g->ncur;
	int		   *win = palloc(2 * k * sizeof(int));

	if (g->tree == NULL)
		g->tree = MemoryContextAlloc(GetMemoryChunkContext(g), Max(k, 1) * sizeof(int));
	for (int j = 0; j < k; j++)
		win[k + j] = j;
	for (int i = k - 1; i >= 1; i--)
	{
		int			a = win[2 * i],
					b = win[2 * i + 1];

		win[i] = text_before(g, g->cur[a], g->cur[b]) ? a : b;
		g->tree[i] = win[i] == a ? b : a;
	}
	g->tree[0] = k > 1 ? win[1] : 0;
	pfree(win);
}

/* the merge of a text key: the sources are the leaves of a tournament, so a step compares strings log2(sources) times, where the scan of the sources does it once for each */
static bool
gord_next_text(SiftGOrd *g, int64 *key, bool *isnull, ItemPointerData *tid)
{
	if (!g->started)
	{
		g->prev = NULL;
		for (int i = 0; i < g->ncur; i++)
			cur_start(g, g->cur[i]);
		text_tournament(g);
		g->started = true;
	}
	for (;;)
	{
		GCur	   *best;
		bool		dup;

		if (g->ncur == 0 || !(best = g->cur[g->tree[0]])->have)
			return false;
		*key = best->k;
		*isnull = best->cls != 1;
		ItemPointerSet(tid, (BlockNumber) (best->tid >> 16), (OffsetNumber) (best->tid & 0xFFFF));
		/* the entry of another source can repeat the last one */
		dup = g->last_set && g->last_cls == best->cls && g->last_tid == best->tid &&
			(best->cls == 1 ? g->last_len == best->slen && memcmp(g->last_str, best->str, best->slen) == 0 : g->last_key == best->k);
		g->prev = best;
		if (!dup)
		{
			g->last_set = true;
			g->last_cls = best->cls;
			g->last_key = best->k;
			g->last_tid = best->tid;
			if (best->cls == 1)
			{
				memcpy(g->last_str, best->str, best->slen);
				g->last_len = best->slen;
			}
		}
		cur_next(g, best);
		text_replay(g, best->slot);
		if (!dup)
			return true;
	}
}

const char *
sift_gord_str(const SiftGOrd *g, uint32 *len)
{
	*len = g->last_len;
	return g->last_str;
}

int
sift_gord_group(const SiftGOrd *g)
{
	return g->grouped;
}

bool
sift_gord_next(SiftGOrd *g, int64 *key, bool *isnull, ItemPointerData *tid)
{
	if (g->collid != 0)
		return gord_next_text(g, key, isnull, tid);
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
		if (best == NULL || !best->have || (g->runner != NULL && cand_cmp(g, best, g->runner, false) >= 0))
		{
			best = g->runner = NULL;
			for (int i = 0; i < g->ncur; i++)
			{
				GCur	   *c = g->cur[i];

				if (!c->have)
					continue;
				if (best == NULL || cand_cmp(g, c, best, false) < 0)
				{
					g->runner = best;
					best = c;
				}
				else if (g->runner == NULL || cand_cmp(g, c, g->runner, false) < 0)
					g->runner = c;
			}
		}
		if (best == NULL)
			return false;
		*key = best->k;
		*isnull = best->cls != 1;
		ItemPointerSet(tid, (BlockNumber) (best->tid >> 16), (OffsetNumber) (best->tid & 0xFFFF));
		/* a source holds an entry once: only the entry of another source can repeat the last one */
		if (best != g->prev && g->last_set && g->last_cls == best->cls && g->last_tid == best->tid && g->last_key == best->k)
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
