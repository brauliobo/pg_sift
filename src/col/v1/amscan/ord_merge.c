/*
 * col/v1/amscan/ord_merge.c - the merge of the global order of a leaf over the sources of a scan (col/v1/amscan/ord_merge.h).
 */

#include "postgres.h"

#include "col/am.h"
#include "col/v1/gorder/gorder.h"
#include "col/v1/amscan/ord_merge_int.h"
#include "core/cmp.h"
#include "core/limits.h"

#define GORD_HEAD_MIN	32

static void
gord_release(void *arg)
{
	SiftOrdMerge   *g = arg;

	for (int i = 0; i < g->ncur; i++)
		if (g->cur[i]->buf != NULL)
			ordm_buf_put(g->cur[i]->buf);
}

/* the chunk of the entry p is decoded as far as p, doubling when the scan goes on (a few rows of a big table decode a few entries) */
pg_noinline void
ordm_cur_fill(SiftOrdMerge *g, OrdMergeCur *c, uint32 p)
{
	uint32		w = c->gm ? UINT32_MAX : p - c->buf->at < c->buf->rows ? Max(c->buf->an * 2, g->head) : g->head;

	sift_col_gorder_head(g->src, c->src, g->leaf, p, w, c->buf);
}

/* a descending scan reads the entries of a key in the order of their addresses, the keys backwards: the group [gs, ge) ends where the last one starts */
static bool
cur_group(SiftOrdMerge *g, OrdMergeCur *c)
{
	int64		k,
				k2;
	uint64		a;

	if (c->ge <= c->vs)
		return false;
	cur_at(g, c, c->ge - 1, &k, &a);
	if (c->lazy && k < g->far)
	{
		c->vs = c->ge;
		c->lazy = false;
		return false;
	}
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

/* the region of the cursor that ends (nulls: the NULL rows) is cut short: the segment has entries after its last, in the order of the scan, that the block does not */
static bool
cur_cut(const SiftOrdMerge *g, const OrdMergeCur *c, bool nulls)
{
	if (!c->ends)
		return false;
	if (nulls)
		return c->cuts.null_cut;
	return g->desc ? c->cuts.lo_cut && c->vs <= c->cuts.lo : c->cuts.hi_cut && c->ve >= c->cuts.hi;
}

static void
cur_enter(SiftOrdMerge *g, OrdMergeCur *c)
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
cur_str(SiftOrdMerge *g, OrdMergeCur *c, int64 k)
{
	uint32		len;
	const char *str;

	if (c->skey == k)
		return;
	str = sift_col_gorder_str(g->src, c->src, g->leaf, k, &len);
	memcpy(c->str, str, len);
	c->slen = len;
	c->skey = k;
}

static void
cur_next(SiftOrdMerge *g, OrdMergeCur *c)
{
	if (c->gm)
	{
		ordm_group_next(g, c);
		return;
	}
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
			if (unlikely(c->lazy) && !nulls && !g->desc && k > g->far)
			{
				/* the first entry past the upper bound: where the seek that was not made would have ended the range */
				c->end = c->ve = --c->pos;
				c->lazy = false;
				continue;
			}
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
		if (cur_cut(g, c, nulls))
		{
			g->short_ = true;
			c->region = 2;
			continue;
		}
		c->region++;
		if (c->region < 2)
			cur_enter(g, c);
	}
}

static void
cur_start(SiftOrdMerge *g, OrdMergeCur *c)
{
	c->region = 0;
	c->have = false;
	if (c->gm)
	{
		c->gnext = c->gskip;
		c->pos = c->end = c->ns = c->ne = 0;
		c->gnull = true;
		ordm_group_next(g, c);
		return;
	}
	cur_enter(g, c);
	cur_next(g, c);
}

/* The part [vs, ve) of the values of a numeric cursor that the bound keeps; the end of it the scan comes to last is not sought (lazy), cur_next stops there */
static void
cur_window(SiftOrdMerge *g, OrdMergeCur *c, const SiftOrdBound *b)
{
	c->lazy = !c->ends && g->bounded && (g->desc ? b->has_lo : b->has_hi);
	c->vs = g->bounded && b->has_lo && !(c->lazy && g->desc) ? sift_col_gorder_seek(g->src, c->src, g->leaf, c->rvs, c->rve, b->lo, false, g->desc ? NULL : c->buf) : c->rvs;
	c->ve = g->bounded && b->has_hi && !(c->lazy && !g->desc) ? sift_col_gorder_seek(g->src, c->src, g->leaf, c->rvs, c->rve, b->hi, true, g->desc ? c->buf : NULL) : c->rve;
}

/* An IN list: the cursors are put on the entries of the value that is the j-th of the scan (the values ascend, so the entries of one are all before those of the next) */
static void
gord_in_window(SiftOrdMerge *g, uint32 j)
{
	int64		v = g->in[g->desc ? g->nin - 1 - j : j];
	SiftOrdBound	b = {true, true, v, v};

	g->iin = j;
	g->far = v;
	for (int i = 0; i < g->ncur; i++)
		cur_window(g, g->cur[i], &b);
}

/* The entries of the value before the next one of an IN list are all read: the cursors move to it (false: there is none) */
static bool
gord_in_advance(SiftOrdMerge *g)
{
	if (g->in == NULL || g->iin + 1 >= g->nin)
		return false;
	gord_in_window(g, g->iin + 1);
	for (int i = 0; i < g->ncur; i++)
		cur_start(g, g->cur[i]);
	g->prev = g->runner = NULL;
	return true;
}

SiftOrdMerge *
sift_ordm_open(SiftColSource *src, int leaf, int rows, bool desc, bool nulls_first, uint32 collid, const SiftOrdBound *b, MemoryContext cxt)
{
	return sift_ordm_open_from(NULL, src, leaf, rows, desc, nulls_first, collid, b, cxt);
}

SiftOrdMerge *
sift_ordm_open_from(SiftOrdMerge *prev, SiftColSource *src, int leaf, int rows, bool desc, bool nulls_first, uint32 collid, const SiftOrdBound *b, MemoryContext cxt)
{
	int			grouped,
				n;
	SiftCGOrdWant want = {rows, desc, nulls_first, b != NULL && collid == 0 && b->has_lo, b != NULL && collid == 0 && b->has_hi, b != NULL ? b->lo : 0, b != NULL ? b->hi : 0, b == NULL};
	SiftOrdMerge   *g;

	/* a string bound is of ranks that differ from segment to segment: the ends of a text order are not read (they are not written) */
	n = sift_col_gorder_open(src, leaf, rows, b != NULL ? b->npin : 0, b != NULL ? b->pinleaf : NULL, b != NULL ? b->pinval : NULL, &grouped, collid, &want);
	if (n < 0)
		return NULL;
	g = MemoryContextAllocZero(cxt, sizeof(SiftOrdMerge));
	g->src = src;
	g->cxt = cxt;
	g->leaf = leaf;
	g->grouped = grouped;
	g->desc = desc;
	g->nulls_first = nulls_first;
	g->collid = collid;
	g->bounded = b != NULL && (b->has_lo || b->has_hi);
	g->far = b == NULL ? 0 : desc ? b->lo : b->hi;
	if (b != NULL && collid == 0 && b->nin > 1)
	{
		g->nin = b->nin;
		g->in = MemoryContextAlloc(cxt, b->nin * sizeof(int64));
		memcpy(g->in, b->in, b->nin * sizeof(int64));
	}
	g->cur =MemoryContextAlloc(cxt, Max(n, 1) * sizeof(OrdMergeCur *));
	g->release.func = gord_release;
	g->release.arg = g;
	MemoryContextRegisterResetCallback(cxt, &g->release);
	for (int i = 0; i < n; i++)
	{
		OrdMergeCur	   *c = MemoryContextAlloc(cxt, sizeof(OrdMergeCur));

		memset(c, 0, offsetof(OrdMergeCur, str));
		c->src = i;
		c->skey = -1;
		if (!sift_col_gorder_info(src, i, leaf, &c->entries, &c->values) || c->entries == 0)
			continue;
		c->buf = ordm_buf_get();
		c->buf->desc = desc;
		if (b == NULL && collid == 0 && sift_col_gorder_groups(src, i, leaf, &c->ngrp))
		{
			c->gm = g->gm = true;
			c->slot = g->ncur;
			g->cur[g->ncur++] = c;
			continue;
		}
		c->ends = sift_col_gorder_cuts(src, i, leaf, &c->cuts);
		g->ends |= c->ends;
		sift_col_gorder_range(src, i, leaf, &c->rvs, &c->rve, &c->ns, &c->ne);
		if (g->bounded && collid != 0)
		{
			/* the ends are ranks of this source's strings, found by the order of the collation */
			uint32		rlo = b->has_lo ? sift_col_gorder_strrank(src, i, leaf, b->slo, b->slolen, b->slo_excl) : 0,
						rhi = b->has_hi ? sift_col_gorder_strrank(src, i, leaf, b->shi, b->shilen, !b->shi_excl) : PG_UINT32_MAX;

			c->vs = b->has_lo ? sift_col_gorder_seek(src, i, leaf, c->rvs, c->rve, rlo, false, desc ? NULL : c->buf) : c->rvs;
			c->ve = b->has_hi ? sift_col_gorder_seek(src, i, leaf, c->rvs, c->rve, rhi, false, desc ? c->buf : NULL) : c->rve;
		}
		else if (g->in == NULL)
			cur_window(g, c, b);
		/* the cursors of an IN list stay: a source with no entry of the first value has some of another */
		if (g->in == NULL && ((g->bounded && collid == 0 && b->has_lo && b->has_hi && b->lo > b->hi) || (c->vs >= c->ve && (g->bounded || c->ns >= c->ne))))
		{
			/* nothing in the part of the ends this scan reads; the segment's own order goes on past it when the part is cut at the side the scan comes to */
			g->short_ |= !(g->bounded && collid == 0 && b->has_lo && b->has_hi && b->lo > b->hi) && (cur_cut(g, c, false) || (!g->bounded && cur_cut(g, c, true)));
			ordm_buf_put(c->buf);
			continue;
		}
		/* the chunk the last merge decoded of this segment (the next group is often in it): the buffer is passed on, and a write buffer's, which each open builds again, is dropped */
		for (int j = 0; prev != NULL && j < prev->ncur; j++)
			if (prev->cur[j]->src == i && prev->cur[j]->buf != NULL)
			{
				ordm_buf_put(c->buf);
				c->buf = prev->cur[j]->buf;
				prev->cur[j]->buf = NULL;
				c->buf->desc = desc;
				if (!c->buf->stored)
				{
					c->buf->at = UINT32_MAX;
					c->buf->cm = c->buf->rows = c->buf->kfrom = c->buf->kn = c->buf->afrom = c->buf->an = 0;
					c->buf->compact = false;
				}
				break;
			}
		c->slot = g->ncur;
		g->cur[g->ncur++] = c;
	}
	if (g->in != NULL)
		gord_in_window(g, 0);
	g->head = Max(GORD_HEAD_MIN, Min((uint32) SIFT_CGORD_ROWS, g->ncur > 0 ? 5 * (uint32) Max(rows, 0) / (4 * g->ncur) : 0));
	return g;
}

bool
sift_ordm_compact(const SiftOrdMerge *g)
{
	for (int i = 0; i < g->ncur; i++)
		if (sift_col_gorder_compact(g->src, g->cur[i]->src, g->leaf))
			return true;
	return false;
}

int
sift_ordm_state(const SiftOrdMerge *g, const ItemPointerData *tid)
{
	OrdMergeCur	   *c = g->prev;

	if (c == NULL)
		return SIFT_GORD_UNKNOWN;
	if (c->clean == 0)
		c->clean = sift_col_gorder_clean(g->src, c->src) ? 1 : 2;
	return c->clean == 1 ? SIFT_GORD_EXACT : sift_col_gorder_exact(g->src, c->src, sift_key_from_tid((ItemPointer) tid));
}

bool
sift_ordm_repeated(const SiftOrdMerge *g)
{
	return g->repeat;
}

/* The cut is taken again while this many rows of the OFFSET are left to take, at most this many times */
#define GORD_SKIP_LEFT		32
#define GORD_SKIP_PASSES	3

/* An OFFSET of a text key shorter than this is walked: the cut below costs about as many string fetches as the walk of 2,000 entries */
#define GORD_TEXT_SKIP_MIN	2048

bool
sift_ordm_ends(const SiftOrdMerge *g)
{
	return g->ends;
}

/* < 0: a comes before b in the scan */
static pg_attribute_always_inline int
cand_cmp(const SiftOrdMerge *g, const OrdMergeCur *a, const OrdMergeCur *b, bool text)
{
	if (a->cls != b->cls)
		return a->cls < b->cls ? -1 : 1;
	if (text && a->cls == 1)
	{
		int			c = sift_cgord_str_order(a->str, a->slen, b->str, b->slen, g->collid);

		if (c != 0)
			return (c < 0) != g->desc ? -1 : 1;
	}
	else if (a->k != b->k)
		return (a->k < b->k) != g->desc ? -1 : 1;
	return SIFT_CMP3(a->tid, b->tid);
}

/* a cursor with no entry left loses every match */
static inline bool
text_before(const SiftOrdMerge *g, const OrdMergeCur *a, const OrdMergeCur *b)
{
	return a->have && (!b->have || cand_cmp(g, a, b, g->collid != 0) < 0);
}

/* the match of leaf slot j up to the root: the winner of the others replaces it where it loses */
static void
text_replay(SiftOrdMerge *g, int j)
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
text_tournament(SiftOrdMerge *g)
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

/* the merge of many sources (a text key, or an integer key of more than GORD_TREE_MIN sources that all run to the end of the scan): the sources are the leaves of a tournament, so a step compares log2(sources) times, where the scan of the sources does it once for each */
static pg_noinline bool
gord_next_tree(SiftOrdMerge *g, int64 *key, bool *isnull, ItemPointerData *tid)
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
		OrdMergeCur	   *best;
		bool		dup;

		if (g->ncur == 0 || !(best = g->cur[g->tree[0]])->have)
			return false;
		*key = best->k;
		*isnull = best->cls != 1;
		ItemPointerSet(tid, (BlockNumber) (best->tid >> 16), (OffsetNumber) (best->tid & 0xFFFF));
		/* the entry of another source can repeat the last one */
		dup = g->last_set && g->last_cls == best->cls && g->last_tid == best->tid &&
			(best->cls == 1 ? g->last_len == best->slen && memcmp(g->last_str, best->str, best->slen) == 0 : g->last_key == best->k);
		g->repeat |= dup;
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

int
sift_ordm_group(const SiftOrdMerge *g)
{
	return g->grouped;
}

/* The source of the next entry (NULL: none is left) */
static inline OrdMergeCur *
gord_pick(SiftOrdMerge *g)
{
	OrdMergeCur	   *best;

	if (g->ncur == 2)
	{
		/* the usual table (a parallel build makes a segment for each 150,000 rows): one comparison picks the next entry */
		OrdMergeCur	   *a = g->cur[0],
				   *b = g->cur[1];

		best = !b->have || (a->have && cand_cmp(g, a, b, false) < 0) ? a : b;
		return best->have ? best : NULL;
	}
	best = g->prev;
	/*
	 * The source that gave the last entry stays the best while its next one comes before the runner-up of the others (they have not moved): one comparison for a row
	 * where the scan of every source costs one for each
	 */
	if (best == NULL || !best->have || (g->runner != NULL && cand_cmp(g, best, g->runner, false) >= 0))
	{
		best = g->runner = NULL;
		for (int i = 0; i < g->ncur; i++)
		{
			OrdMergeCur	   *c = g->cur[i];

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
	return best;
}

bool
sift_ordm_next(SiftOrdMerge *g, int64 *key, bool *isnull, ItemPointerData *tid)
{
	g->repeat = false;
	if (g->collid != 0 || (g->ncur > GORD_TREE_MIN && g->in == NULL && !g->ends && !g->gm && !g->short_))
		return gord_next_tree(g, key, isnull, tid);
	if (!g->started)
	{
		for (int i = 0; i < g->ncur; i++)
			cur_start(g, g->cur[i]);
		g->started = true;
		g->prev = NULL;
	}
	/* one source (a table of one segment): nothing to choose from or to repeat */
	if (g->ncur == 1)
	{
		OrdMergeCur	   *best = g->cur[0];

		while (!g->short_ && !best->have && gord_in_advance(g))
			;
		if (g->short_ || !best->have)
			return false;
		*key = best->k;
		*isnull = best->cls != 1;
		ItemPointerSet(tid, (BlockNumber) (best->tid >> 16), (OffsetNumber) (best->tid & 0xFFFF));
		g->prev = best;
		cur_next(g, best);
		return true;
	}
	for (;;)
	{
		OrdMergeCur	   *best;

		best = NULL;
		while (!g->short_ && (best = gord_pick(g)) == NULL && gord_in_advance(g))
			;
		if (g->short_ || best == NULL)
			return false;
		*key = best->k;
		*isnull = best->cls != 1;
		ItemPointerSet(tid, (BlockNumber) (best->tid >> 16), (OffsetNumber) (best->tid & 0xFFFF));
		/* a source holds an entry once: only the entry of another source can repeat the last one */
		if (best != g->prev && g->last_set && g->last_cls == best->cls && g->last_tid == best->tid && g->last_key == best->k)
		{
			g->repeat = true;
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
