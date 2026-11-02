/*
 * agg/gord.c - the merge of the global order of a leaf over the sources of a scan (agg/gord.h).
 */

#include "postgres.h"

#include "col/am.h"
#include "col/gorder.h"
#include "agg/gord/int.h"
#include "core/cmp.h"

#define GORD_HEAD_MIN	32

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
	b->cm = b->rows = b->kfrom = b->kn = 0;
	b->compact = false;
	return b;
}

static void
gord_release(void *arg)
{
	SiftGOrd   *g = arg;

	for (int i = 0; i < g->ncur; i++)
		gbuf_put(g->cur[i]->buf);
}

/* the chunk of the entry p is decoded as far as p, doubling when the scan goes on (a few rows of a big table decode a few entries) */
pg_noinline void
gord_cur_fill(SiftGOrd *g, GCur *c, uint32 p)
{
	uint32		w = c->gm ? UINT32_MAX : p - c->buf->at < c->buf->rows ? c->buf->cm * 2 : g->head;

	sift_colsrc_gorder_head(g->src, c->src, g->leaf, p, w, c->buf);
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

/* the region of the cursor that ends (nulls: the NULL rows) is cut short: the segment has entries after its last, in the order of the scan, that the block does not */
static bool
cur_cut(const SiftGOrd *g, const GCur *c, bool nulls)
{
	if (!c->ends)
		return false;
	if (nulls)
		return c->cuts.null_cut;
	return g->desc ? c->cuts.lo_cut && c->vs <= c->cuts.lo : c->cuts.hi_cut && c->ve >= c->cuts.hi;
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
	if (c->gm)
	{
		gord_group_next(g, c);
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
cur_start(SiftGOrd *g, GCur *c)
{
	c->region = 0;
	c->have = false;
	if (c->gm)
	{
		c->gnext = c->gskip;
		c->pos = c->end = c->ns = c->ne = 0;
		c->gnull = true;
		gord_group_next(g, c);
		return;
	}
	cur_enter(g, c);
	cur_next(g, c);
}

SiftGOrd *
sift_gord_open(SiftSrcSource *src, int leaf, int rows, bool desc, bool nulls_first, uint32 collid, const SiftGBound *b, MemoryContext cxt)
{
	int			grouped,
				n;
	SiftCGOrdWant want = {rows, desc, nulls_first, b != NULL && collid == 0 && b->has_lo, b != NULL && collid == 0 && b->has_hi, b != NULL ? b->lo : 0, b != NULL ? b->hi : 0};
	SiftGOrd   *g;

	/* a string bound is of ranks that differ from segment to segment: the ends of a text order are not read (they are not written) */
	n = sift_colsrc_gorder_open(src, leaf, rows, b != NULL ? b->npin : 0, b != NULL ? b->pinleaf : NULL, b != NULL ? b->pinval : NULL, &grouped, collid, &want);
	if (n < 0)
		return NULL;
	g = MemoryContextAllocZero(cxt, sizeof(SiftGOrd));
	g->src = src;
	g->cxt = cxt;
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
		if (b == NULL && collid == 0 && sift_colsrc_gorder_groups(src, i, leaf, &c->ngrp))
		{
			c->gm = g->gm = true;
			c->slot = g->ncur;
			g->cur[g->ncur++] = c;
			continue;
		}
		c->ends = sift_colsrc_gorder_cuts(src, i, leaf, &c->cuts);
		g->ends |= c->ends;
		sift_colsrc_gorder_range(src, i, leaf, &rvs, &rve, &c->ns, &c->ne);
		if (g->bounded && collid != 0)
		{
			/* the ends are ranks of this source's strings, found by the order of the collation */
			uint32		rlo = b->has_lo ? sift_colsrc_gorder_strrank(src, i, leaf, b->slo, b->slolen, b->slo_excl) : 0,
						rhi = b->has_hi ? sift_colsrc_gorder_strrank(src, i, leaf, b->shi, b->shilen, !b->shi_excl) : PG_UINT32_MAX;

			c->vs = b->has_lo ? sift_colsrc_gorder_seek(src, i, leaf, rvs, rve, rlo, false, desc ? NULL : c->buf) : rvs;
			c->ve = b->has_hi ? sift_colsrc_gorder_seek(src, i, leaf, rvs, rve, rhi, false, desc ? c->buf : NULL) : rve;
		}
		else
		{
			c->vs = g->bounded && b->has_lo ? sift_colsrc_gorder_seek(src, i, leaf, rvs, rve, b->lo, false, desc ? NULL : c->buf) : rvs;
			c->ve = g->bounded && b->has_hi ? sift_colsrc_gorder_seek(src, i, leaf, rvs, rve, b->hi, true, desc ? c->buf : NULL) : rve;
		}
		if ((g->bounded && collid == 0 && b->has_lo && b->has_hi && b->lo > b->hi) || (c->vs >= c->ve && (g->bounded || c->ns >= c->ne)))
		{
			/* nothing in the part of the ends this scan reads; the segment's own order goes on past it when the part is cut at the side the scan comes to */
			g->short_ |= !(g->bounded && collid == 0 && b->has_lo && b->has_hi && b->lo > b->hi) && (cur_cut(g, c, false) || (!g->bounded && cur_cut(g, c, true)));
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
sift_gord_compact(const SiftGOrd *g)
{
	for (int i = 0; i < g->ncur; i++)
		if (sift_colsrc_gorder_compact(g->src, g->cur[i]->src, g->leaf))
			return true;
	return false;
}

int
sift_gord_state(const SiftGOrd *g, const ItemPointerData *tid)
{
	return g->prev == NULL ? SIFT_GORD_UNKNOWN : sift_colsrc_gorder_state(g->src, g->prev->src, sift_key_from_tid((ItemPointer) tid));
}

/* The source of the entry sift_gord_next gave last: with its address, what sift_gord_values reads */
int
sift_gord_src(const SiftGOrd *g)
{
	return g->prev->src;
}

void
sift_gord_values(const SiftGOrd *g, int leaf, const int *seg, const SiftKey *tid, uint32 n, int64 *val, uint8 *st)
{
	sift_colsrc_gorder_values(g->src, leaf, seg, tid, n, val, st);
}

bool
sift_gord_repeated(const SiftGOrd *g)
{
	return g->repeat;
}

/* The sum over the cursors of how many of their entries possibly come before v in the scan: the entries a scan that skips the n first cannot reach all of if this is at most n */
static bool
skip_count(SiftGOrd *g, int64 v, uint32 k, uint32 *clo)
{
	uint64		total = 0;

	for (int i = 0; i < g->ncur; i++)
	{
		GCur	   *c = g->cur[i];
		uint32		lo,
					hi;

		/* a source that cannot say (a write buffer) may have all its entries before v, and none of them is taken off */
		if (!sift_colsrc_gorder_before(g->src, c->src, g->leaf, c->vs, c->ve, v, g->desc, &lo, &hi, c->buf))
			lo = 0, hi = c->ve - c->vs;
		total += hi;
		if (clo != NULL)
			clo[i] = lo;
	}
	return total <= k;
}

/* The cut is taken again while this many rows of the OFFSET are left to take, at most this many times */
#define GORD_SKIP_LEFT		32
#define GORD_SKIP_PASSES	3

/* An OFFSET of a text key shorter than this is walked: the cut below costs about as many string fetches as the walk of 2,000 entries */
#define GORD_TEXT_SKIP_MIN	2048

/* How many entries of the cursor come before the string s in the scan: the strings below it (ascending), above it (descending) */
static uint32
cur_before_str(const SiftGOrd *g, const GCur *c, const char *s, uint32 len)
{
	uint32		rank = sift_colsrc_gorder_strrank(g->src, c->src, g->leaf, s, len, g->desc);
	uint32		pos = sift_colsrc_gorder_seek(g->src, c->src, g->leaf, c->vs, c->ve, rank, false, NULL);

	return g->desc ? c->ve - pos : pos - c->vs;
}

/* The string of the m-th entry of the cursor in the order of the scan, in str */
static uint32
cur_nth_str(SiftGOrd *g, GCur *c, uint32 m, char *str)
{
	int64		key;
	uint64		addr;
	uint32		len;
	const char *s;

	cur_at(g, c, g->desc ? c->ve - 1 - m : c->vs + m, &key, &addr);
	s = sift_colsrc_gorder_str(g->src, c->src, g->leaf, key, &len);
	memcpy(str, s, len);
	return len;
}

/* The entries of the sources that come before the string s in the scan, at most k of them (false: more) */
static bool
text_cut(SiftGOrd *g, const char *s, uint32 len, uint32 k, uint32 *clo)
{
	uint64		total = 0;

	for (int i = 0; i < g->ncur; i++)
	{
		clo[i] = cur_before_str(g, g->cur[i], s, len);
		total += clo[i];
	}
	return total <= k;
}

/*
 * The cut of a text key: a string v that at most k entries of the sources come before (clo: how many of each), as late in the scan as a few rounds find. A round takes the string at the
 * fraction f of what is left to take of each source's remaining entries (the sources hold alike strings), tries the latest of those first, and moves the cut there when at most k come
 * before it; the rounds take 90% of what is left, then half, then a fifth, and the walk that follows reads the rest. Every cut is counted over all the sources (text_cut), so a skew in
 * them costs a shorter cut, never a wrong one. At most `rounds` rounds, from the cut clo already holds when `resume` (the entries a dead row left over are cut by one more round). false when nothing comes before the first string.
 */
static bool
text_skip(SiftGOrd *g, uint32 k, uint32 *clo, int rounds, bool resume)
{
	static const double frac[] = {0.9, 0.5, 0.2};
	char	   *cand = palloc(g->ncur * SIFT_CGORD_STR_MAX);
	uint32	   *tmp = palloc(g->ncur * sizeof(uint32)),
			   *clen = palloc(g->ncur * sizeof(uint32));
	int		   *ord = palloc(g->ncur * sizeof(int));
	uint32		total = 0;

	if (!resume)
		memset(clo, 0, g->ncur * sizeof(uint32));
	for (int i = 0; i < g->ncur; i++)
		total += clo[i];
	for (int round = 0, f = 0; round < rounds && f < lengthof(frac) && k - total > 32; round++)
	{
		uint64		rem = 0;
		int			nc = 0;
		bool		moved = false;

		for (int i = 0; i < g->ncur; i++)
			rem += (g->cur[i]->ve - g->cur[i]->vs) - clo[i];
		for (int i = 0; rem > 0 && i < g->ncur; i++)
		{
			GCur	   *c = g->cur[i];
			uint64		left = (c->ve - c->vs) - clo[i],
						q = clo[i] + (uint64) (frac[f] * (k - total) * ((double) left / rem));

			if (q >= (uint64) (c->ve - c->vs))
				continue;
			clen[nc] = cur_nth_str(g, c, (uint32) q, cand + (size_t) nc * SIFT_CGORD_STR_MAX);
			ord[nc] = nc;
			nc++;
		}
		/* latest in the scan first: insertion sort, the candidates are as many as the sources */
		for (int i = 1; i < nc; i++)
			for (int j = i; j > 0; j--)
			{
				int			x = ord[j - 1],
							y = ord[j],
							cmp = sift_cgord_str_order(cand + (size_t) x * SIFT_CGORD_STR_MAX, clen[x], cand + (size_t) y * SIFT_CGORD_STR_MAX, clen[y], g->collid);

				if (g->desc ? cmp <= 0 : cmp >= 0)
					break;
				ord[j - 1] = y, ord[j] = x;
			}
		for (int i = 0; i < nc && !moved; i++)
			if (text_cut(g, cand + (size_t) ord[i] * SIFT_CGORD_STR_MAX, clen[ord[i]], k, tmp))
			{
				uint32		t2 = 0;

				for (int j = 0; j < g->ncur; j++)
					t2 += tmp[j];
				if (t2 > total)
				{
					memcpy(clo, tmp, g->ncur * sizeof(uint32));
					total = t2;
					moved = true;
				}
			}
		if (!moved)
			f++;
	}
	pfree(cand);
	pfree(tmp);
	pfree(clen);
	pfree(ord);
	return total > 0;
}

/* The cut of a numeric key: the largest v (ascending) before which at most k entries may come, the smallest above which (descending); clo is how many of each source's entries that is */
static bool
num_skip(SiftGOrd *g, uint32 k, uint32 *clo)
{
	uint64		lo = 0,
				hi = UINT64_MAX;

	if (!skip_count(g, g->desc ? PG_INT64_MAX : PG_INT64_MIN, k, NULL))
		return false;
	while (lo < hi)
	{
		uint64		mid = lo + (hi - lo) / 2 + (g->desc ? 0 : (hi - lo) & 1);

		if (skip_count(g, (int64) (mid + (uint64) PG_INT64_MIN), k, NULL))
		{
			if (g->desc)
				hi = mid;
			else
				lo = mid;
		}
		else
		{
			if (g->desc)
				lo = mid + 1;
			else
				hi = mid - 1;
		}
	}
	(void) skip_count(g, (int64) (lo + (uint64) PG_INT64_MIN), k, clo);
	return true;
}

/*
 * Takes the first entries off the front of every cursor, as many as can be said to be among the first k of the scan without reading their keys: those whose bucket (a compact
 * block keeps the bucket of a key, not the key; a whole one its key, a bucket of one) is before v, the largest v before which at most k entries may come. They are all among the
 * first k (fewer than k entries have a key before v). An entry stands for a row if the page of its address is all-visible and the unit's row live (sift_colsrc_gorder_exact): those are
 * counted (returned); the others are kept for the caller (sift_gord_pending) to confirm in the heap, as the scan would have, and to count the ones that stand for a row. The scan then goes on
 * with the rest and throws away the other k less the rows counted; the result is the same as the throwing away of the first k. A source that cannot say (a write buffer) counts all its
 * entries as possibly first and gives none up. A text key is cut at a string instead of a bucket (text_skip), and an entry the heap must confirm is confirmed by its string. Before the first sift_gord_next; 0 (nothing skipped) for the ends of an order, or when the scan starts with NULL rows.
 */
uint32
sift_gord_skip(SiftGOrd *g, uint32 k)
{
	uint32	   *clo,
			   *done;
	uint32		total = 0;
	bool		stuck = false;

#ifdef SIFT_TESTING
	if (sift_test_col_gorder_noskip)
		return 0;
#endif
	if (g->started || g->ends || k == 0 || g->ncur == 0 || (g->collid != 0 && k < GORD_TEXT_SKIP_MIN))
		return 0;
	if (g->gm)
		return g->collid == 0 && g->ncur > 0 ? gord_group_skip(g, k) : 0;
	for (int i = 0; i < g->ncur; i++)
		if (g->cur[i]->sk != 0 || (g->nulls_first && g->cur[i]->ne > g->cur[i]->ns && !g->bounded))
			return 0;
	clo = palloc(g->ncur * sizeof(uint32));
	done = palloc0(g->ncur * sizeof(uint32));
	/*
	 * The entries of a cut are at most its budget, so the rows among them are at most k. Entries that are no row to count (a dead row, one the heap must confirm) leave the rest of k to the
	 * walk that follows, which reads a string for each entry (a block of the dictionary to decompress): the cut of a text key is taken further for the entries it may add (as many as the rows still to take:
	 * they hold no more rows than that). An integer key walks them for less than the extra cut costs (7.0 M instructions become 8.2 M).
	 */
	for (int pass = 0; pass < (g->collid != 0 ? GORD_SKIP_PASSES : 1) && !stuck; pass++)
	{
		int64		left = (int64) k - total - g->npend;
		uint32		taken = 0;

		if (pass > 0 && left < GORD_SKIP_LEFT)
			break;
		for (int i = 0; i < g->ncur; i++)
			taken += done[i];
		memcpy(clo, done, g->ncur * sizeof(uint32));
		if (!(g->collid != 0 ? text_skip(g, taken + (uint32) left, clo, pass == 0 ? 8 : 1, pass > 0) : num_skip(g, taken + (uint32) left, clo)))
			break;
		for (int i = 0; i < g->ncur; i++)
			if (clo[i] < done[i])
				stuck = true;
		if (stuck)
			break;
		for (int i = 0; i < g->ncur; i++)
		{
			GCur	   *c = g->cur[i];
			uint32		n = done[i];

			/* the addresses of the entries come with their chunk; one that is not exact goes to the caller to be confirmed in the heap, with its key where the block has it */
			for (; n < clo[i]; n++)
			{
				uint32		p = g->desc ? c->ve - 1 - n : c->vs + n;
				uint64		a;
				int			st;

				if (p - c->buf->at >= c->buf->cm)
					sift_colsrc_gorder_head(g->src, c->src, g->leaf, p, UINT32_MAX, c->buf);
				a = c->buf->addr[p - c->buf->at];
				st = sift_colsrc_gorder_state(g->src, c->src, a);
				if (st == SIFT_GORD_EXACT)
					total++;
				else if (st == SIFT_GORD_UNKNOWN || (st == SIFT_GORD_CHECK && g->npend >= GORD_PEND_MAX))
					break;
				else if (st == SIFT_GORD_CHECK)
				{
					if (g->npend == g->cpend)
					{
						g->cpend = Max(g->cpend * 2, 1024);
						g->pend = g->pend == NULL ? MemoryContextAlloc(g->cxt, g->cpend * sizeof(SiftGPend)) : repalloc(g->pend, g->cpend * sizeof(SiftGPend));
					}
					g->pend[g->npend].tid = a;
					g->pend[g->npend].key = c->buf->compact ? 0 : c->buf->key[p - c->buf->at];
					g->pend[g->npend].cur = i;
					g->pend[g->npend++].check = !c->buf->compact;
				}
			}
			stuck |= n < clo[i];
			done[i] = n;
		}
	}
	for (int i = 0; i < g->ncur; i++)
	{
		GCur	   *c = g->cur[i];

		if (g->desc)
			c->ve -= done[i];
		else
			c->vs += done[i];
		c->sk = done[i];
#ifdef SIFT_TESTING
		sift_col_gdead_stat[2] += done[i];
#endif
	}
	pfree(clo);
	pfree(done);
	return total;
}

const char *
sift_gord_pend_str(SiftGOrd *g, const SiftGPend *p, uint32 *len)
{
	return sift_colsrc_gorder_str(g->src, g->cur[p->cur]->src, g->leaf, p->key, len);
}

const SiftGPend *
sift_gord_pending(const SiftGOrd *g, uint32 *n)
{
	*n = g->npend;
	return g->pend;
}

void
sift_gord_pending_done(SiftGOrd *g)
{
	if (g->pend != NULL)
		pfree(g->pend);
	g->pend = NULL;
	g->npend = g->cpend = 0;
}

void
sift_gord_rewind(SiftGOrd *g)
{
	sift_gord_pending_done(g);
	for (int i = 0; i < g->ncur; i++)
	{
		GCur	   *c = g->cur[i];

		if (g->desc)
			c->ve += c->sk;
		else
			c->vs -= c->sk;
		c->sk = 0;
		c->gskip = 0;
	}
	g->started = false;
	g->last_set = false;
	g->short_ = false;
}

bool
sift_gord_ends(const SiftGOrd *g)
{
	return g->ends;
}

bool
sift_gord_short(const SiftGOrd *g)
{
	return g->short_;
}

/* < 0: a comes before b in the scan */
static pg_attribute_always_inline int
cand_cmp(const SiftGOrd *g, const GCur *a, const GCur *b, bool text)
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
	g->repeat = false;
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

		if (g->short_)
			return false;

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
