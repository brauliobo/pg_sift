/*
 * col/f2/scan/ord_cursor.c - the cursors of the ordered scan of an index of format major 2 (col/f2/scan/ord_int.h). A cursor reads a part a window of rows of a granule at a time through the sink of
 * f2_scan.c (predicates, zones, deletion vectors and pending deletes decide the rows as for any scan): the first window is the first ORD_WIN_FIRST rows that pass the predicates and each next one doubles up to the point directory's
 * limit, after which a window is the rest of its granule, so a scan that stops after a few rows decodes a few rows of each part instead of its first granule.
 */
#include "postgres.h"

#include "col/f2/col_delta/idx.h"
#include "col/f2/scan/ord_int.h"
#include "core/memtrack.h"

#define ORD_WIN_FIRST	64
#define ORD_WIN_DOUBLED	4		/* windows of ORD_WIN_FIRST << nwin rows (the last is SIFT_POINT_ROWS_MAX), then the rest of the granule */

StaticAssertDecl((ORD_WIN_FIRST << (ORD_WIN_DOUBLED - 1)) <= SIFT_POINT_ROWS_MAX, "the windows that double are rows a granule decodes at their own values");

int
ord_row_cmp(const SiftOrdStream *os, const OrdRow *a, const OrdRow *b)
{
	for (int k = 0; k < os->nkey; k++)
	{
		const bool	na = (a->knull >> k) & 1,
					nb = (b->knull >> k) & 1;
		int			c;

		if (na || nb)
		{
			if (na != nb)
				return na == os->nulls_first[k] ? -1 : 1;
			continue;
		}
		c = a->key[k] < b->key[k] ? -1 : a->key[k] > b->key[k];
		if (c != 0)
			return os->desc[k] ? -c : c;
	}
	return a->tid < b->tid ? -1 : a->tid > b->tid;
}

static int
row_qcmp(const void *a, const void *b, void *arg)
{
	return ord_row_cmp(arg, a, b);
}

/* Whether the row satisfies the row comparisons of the scan: the first pair of values that differ decides, and a NULL among those it reaches makes the comparison NULL, which no row satisfies */
static bool
row_accepted(const SiftOrdStream *os, const OrdRow *o)
{
	for (int j = 0; j < os->s->nrowcmp; j++)
	{
		const SiftRowBound *rb = &os->s->rowcmp[j];
		int			c = 0;

		for (int k = 0; k < rb->n && c == 0; k++)
		{
			if ((o->knull >> k) & 1)
				return false;
			c = o->key[k] < rb->vals[k].i ? -1 : o->key[k] > rb->vals[k].i;
		}
		if (!(rb->op == SIFT_KOP_GT ? c > 0 : rb->op == SIFT_KOP_GE ? c >= 0 : rb->op == SIFT_KOP_LT ? c < 0 : c <= 0))
			return false;
	}
	return true;
}

void
ord_sink(void *arg, const SiftF2Batch *b)
{
	OrdCursor  *c = arg;
	SiftOrdStream *os = c->os;
	const F2Scan *s = os->s;
	const int	nlive = sift_f2_batch_rows(b);
	MemoryContext old;

	if (nlive > c->cap)
	{
		c->rows = c->rows == NULL ? MemoryContextAlloc(os->pcx, (Size) nlive * os->stride) :
			repalloc(c->rows, (Size) nlive * os->stride);
		c->cap = nlive;
	}
	old = MemoryContextSwitchTo(c->datacx);
	for (int w = 0; w < (b->n + 63) / 64; w++)
		for (uint64 bits = b->live[w] | b->maybe[w]; bits != 0; bits &= bits - 1)
		{
			const int	r = w * 64 + __builtin_ctzll(bits);
			OrdRow	   *o = ord_row_at(os, c, c->nrows++);

			o->tid = b->tid[r];
			o->maybe = (((b->maybe[w] >> (r & 63)) & 1) | s->recheck_all) != 0;
			o->knull = 0;
			o->retnull = 0;
			for (int k = 0; k < os->nkey; k++)
			{
				const ColView *cv = &b->views[os->key_leaf[k]];

				o->key[k] = cv->isnull[r] ? 0 : cv->i[r];
				o->knull |= (uint8) cv->isnull[r] << k;
			}
			if (!row_accepted(os, o))
			{
				c->nrows--;
				continue;
			}
			for (int j = 0; j < s->nret; j++)
				if (!sift_f2_view_datum(&os->schema->cols[s->ret_leaf[j]], &b->views[s->ret_leaf[j]], r, &o->ret[j],
									 TupleDescAttr(s->itupdesc, s->ret_att[j] - 1)))
					o->retnull |= 1U << j;
		}
	MemoryContextSwitchTo(old);
}

/* ---- the windows ---- */

/* The rows [w.lo, w.hi) of a granule to read: the first keep of those that pass the predicates (the last, reading backward); keep == 0: all */
typedef struct OrdReq
{
	OrdWin		w;
	int			keep;
} OrdReq;

/* The granule of the part the gi-th granule of the spans of the cursor is */
static uint32
cur_granule(const OrdCursor *c, uint32 gi)
{
	int			sp = 0;

	for (; gi >= c->spans[sp][1] - c->spans[sp][0]; sp++)
		gi -= c->spans[sp][1] - c->spans[sp][0];
	return gi + c->spans[sp][0];
}

static OrdReq
req_all(const OrdCursor *c, uint32 gi)
{
	return (OrdReq) {{gi, 0, c->pin->gdir[cur_granule(c, gi)].nrows}, 0};
}

static OrdReq
req_keep(const OrdCursor *c, OrdReq r)
{
	r.keep = !c->cached && c->nwin < ORD_WIN_DOUBLED ? ORD_WIN_FIRST << c->nwin : 0;
	return r;
}

static OrdReq
win_first(const OrdCursor *c, bool fwd)
{
	return c->pin == NULL ? (OrdReq) {{0}, 0} : req_keep(c, req_all(c, fwd ? 0 : c->ngran - 1));
}

/* The rows after the window in hand in the direction; false past the last row of the cursor (the slab is one window) */
static bool
win_next(const OrdCursor *c, bool fwd, OrdReq *r)
{
	const OrdWin *p = &c->win;
	OrdReq		here;

	if (c->pin == NULL)
		return false;
	here = req_all(c, p->gi);
	if (fwd ? p->hi < here.w.hi : p->lo > 0)
		*r = fwd ? (OrdReq) {{p->gi, p->hi, here.w.hi}, 0} : (OrdReq) {{p->gi, 0, p->lo}, 0};
	else if (fwd ? p->gi + 1 < c->ngran : p->gi > 0)
		*r = req_all(c, fwd ? p->gi + 1 : p->gi - 1);
	else
		return false;
	*r = req_keep(c, *r);
	return true;
}

/* Everything the cursor read in the direction before the window in hand: the rest of its granule, else the whole granule before */
static OrdReq
win_before(const OrdCursor *c, bool fwd)
{
	const OrdWin *p = &c->win;
	const OrdReq here = req_all(c, p->gi);

	if (fwd ? p->lo > 0 : p->hi < here.w.hi)
		return fwd ? (OrdReq) {{p->gi, 0, p->lo}, 0} : (OrdReq) {{p->gi, p->hi, here.w.hi}, 0};
	return req_all(c, fwd ? p->gi - 1 : p->gi + 1);
}

/* The live rows of the request in the order of the key; the window is what the scan took of its range */
static void
cur_load(SiftOrdStream *os, OrdCursor *c, OrdReq q, bool fwd)
{
	F2Scan	   *s = os->s;
	MemoryContext old = MemoryContextSwitchTo(s->cx);
	uint32		g;

	MemoryContextReset(c->datacx);
	c->nrows = 0;
	c->win = q.w;
	c->loaded = true;
	s->sink_arg = c;
	s->part = c->d;
	if (c->pin == NULL)
	{
		scan_slab(os->rel, s, os->f, os->schema, c->e, c->px, os->snap);
		qsort_arg(c->rows, c->nrows, os->stride, row_qcmp, os);
		MemoryContextSwitchTo(old);
		return;
	}
	if (c->cached)
	{
		sift_rescan_granule(os->rel, s, os->rc, os->schema, c->pin, cur_granule(c, q.w.gi), c->px, os->snap);
		MemoryContextSwitchTo(old);
		return;
	}
	{
		int			nexc = c->pcx.pp.nexc;
		const SiftVariantExcSummary *exc = sift_cdi_slot_exceptions(s, &c->cd_map, os->rel, os->schema, c->e, c->pin->part, os->snap, c->pcx.pp.exc, &nexc);

		if (exc != c->pcx.pp.exc)
			pfree((void *) exc);
	}
	g = cur_granule(c, q.w.gi);
	s->cands = c->cands;
	s->rowwin = (SiftF2RowWin) {q.w.lo, q.w.hi, q.keep, fwd, fwd ? q.w.hi : q.w.lo};
	scan_span(os->rel, s, os->schema, c->pin, &c->pcx.pp, g, g + 1, c->px, os->snap);
	if (fwd)
		c->win.hi = s->rowwin.end;
	else
		c->win.lo = s->rowwin.end;
	s->rowwin = (SiftF2RowWin) {0};
	s->cands = NULL;
	MemoryContextSwitchTo(old);
}

void
ord_cur_init(SiftOrdStream *os, OrdCursor *c, const SiftPartitionEntry *e, const SiftPartDesc *d, const SiftPendDelIndex *px)
{
	memset(c, 0, sizeof(*c));
	c->os = os;
	c->e = e;
	c->d = d;
	c->px = px;
	c->datacx = sift_mem_allocset_create(os->pcx, "sift f2 ordered row");
	c->cd_map.cx = os->pcx;
	c->ngran = 1;
}

static void
cur_enter(SiftOrdStream *os, OrdCursor *c, OrdReq q, bool fwd)
{
	cur_load(os, c, q, fwd);
	c->nwin++;
	c->pos = fwd ? 0 : c->nrows;
}

int
ord_cur_head(SiftOrdStream *os, OrdCursor *c, bool fwd)
{
	OrdReq		q;

	if (!c->loaded)
	{
		if (c->ngran == 0)
			return -1;
		cur_enter(os, c, win_first(c, fwd), fwd);
	}
	for (;;)
	{
		if (fwd ? c->pos < c->nrows : c->pos > 0)
			return fwd ? c->pos : c->pos - 1;
		if (!win_next(c, fwd, &q))
			return -1;
		cur_enter(os, c, q, fwd);
	}
}

void
ord_cur_unstep(SiftOrdStream *os, OrdCursor *c, bool fwd)
{
	while (fwd ? c->pos == 0 : c->pos == c->nrows)
	{
		cur_load(os, c, win_before(c, fwd), fwd);
		c->pos = fwd ? c->nrows : 0;
	}
	c->pos += fwd ? -1 : 1;
}

void
ord_cur_restore(SiftOrdStream *os, OrdCursor *c, const OrdPos *p)
{
	const bool	same = c->loaded && c->win.gi == p->win.gi && c->win.lo == p->win.lo && c->win.hi == p->win.hi;

	if (p->loaded && !same)
		cur_load(os, c, (OrdReq) {p->win, 0}, true);
	c->loaded = p->loaded;
	c->pos = p->pos;
}
