/*
 * col/f2/f2_proj_scan.c - the rows of a part, one at a time, granule by granule: the live rows of its deletion vector, or with dead_only the dead ones (the rows an AGG projection part still counts
 * after the base part was given a deletion vector). A SORT projection part is read through the same cursor, lazily, so that a merge with a limit stops decoding where it stops reading; sift_proj_cursor_seek leaves the cursor the granules its key marks say a statement can reach,
 * and sift_proj_cursor_progressive makes it decode a granule a window of rows at a time (the first PROJ_WIN_FIRST, then twice as many up to SIFT_POINT_ROWS_MAX, then the rest of the granule).
 */
#include "core/host.h"

#include "col/f2/delvec.h"
#include "col/f2/f2_proj.h"
#include "col/f2/f2_proj_span.h"
#include "col/f2/key_plan.h"
#include "col/f2/part_format.h"
#include "core/memtrack.h"
#include "core/page_io.h"

#define SIFT_PROJ_SEC_KEY_MARKS	0x020D
#define PROJ_WIN_FIRST			64
#define PROJ_WIN_DOUBLED		4

StaticAssertDecl((PROJ_WIN_FIRST << (PROJ_WIN_DOUBLED - 1)) <= SIFT_POINT_ROWS_MAX, "the windows that double are rows a granule decodes at their own values");

StaticAssertDecl(SIFT_PROJ_SPANS_MAX == SIFT_KEY_PLAN_MAX_RANGES, "a key plan fits the spans of a cursor");

struct SiftProjCursor
{
	Relation	rel;
	SiftPart   *part;
	SiftGranuleDesc *dir;
	SiftDv	   *dv;
	const SiftPartSpec *spec;
	bool		dead_only;
	SiftProjSpans spans;		/* the granules to read */
	uint32		nread;			/* granules read */
	SiftDvCursor dgc;			/* the deletion vector's granule of the rows in hand */
	SiftRowBuf *one;			/* the rows of the window in hand */
	int			r;
	int			r0;				/* the row of the granule that row 0 of one is */
	bool		keyed;			/* only the rows whose first sort column is in [key_lo, key_hi] are read */
	bool		progressive;	/* a granule is read a window of rows at a time */
	const bool *want;
	int64		key_lo;
	int64		key_hi;
	SiftGranuleView view;		/* the granule in hand */
	uint32		g;
	uint16		gn;				/* its rows */
	uint16		lo;				/* the window in hand is rows [lo, hi) of it */
	uint16		hi;
	int			nwin;			/* the windows read so far: the next one is larger */
};

SiftProjCursor *
sift_proj_cursor_open(Relation rel, const SiftPartDesc *d, uint32 part_id, const SiftPartSpec *spec, bool dead_only)
{
	SiftProjCursor *c = palloc0(sizeof(SiftProjCursor));
	uint16	   *grows;

	c->rel = rel;
	c->part = sift_part_open_desc(rel, d, part_id);
	c->dir = sift_part_granule_dir(rel, c->part);
	grows = palloc(Max(c->part->hdr.ngranules, 1) * sizeof(uint16));
	for (uint32 g = 0; g < c->part->hdr.ngranules; g++)
		grows[g] = c->dir[g].nrows;
	c->dv = d->dv_blk != InvalidBlockNumber ? sift_dv_read(rel, d, part_id, grows) : NULL;
	c->spec = spec;
	c->dead_only = dead_only;
	sift_proj_spans_whole(&c->spans, c->part->hdr.ngranules);
	return c;
}

bool
sift_proj_cursor_seek(SiftProjCursor *c, const SiftKeySpec *ks, const SiftKeyPlan *plan)
{
	SiftKeyMarks km;
	uint32		spans[SIFT_PROJ_SPANS_MAX][2];
	uint64		len;
	void	   *blob;
	const char *err;

	if (sift_part_section(c->part, SIFT_PROJ_SEC_KEY_MARKS) == NULL)
		return false;
	blob = sift_part_read_section(c->rel, c->part, SIFT_PROJ_SEC_KEY_MARKS, &len);
	err = sift_key_marks_open(blob, len, ks, c->part->hdr.ngranules, &km);
	if (err != NULL)
		SIFT_SEG_CORRUPT(c->rel, c->part->hdr.part_gen, "%s.", err);
	sift_proj_spans_set(&c->spans, (const uint32 (*)[2]) spans, sift_key_marks_spans(&km, plan, spans), c->part->hdr.ngranules);
	pfree(blob);
	return true;
}

void
sift_proj_cursor_stats(const SiftProjCursor *c, uint32 *skipped, uint32 *read)
{
	*skipped = c->spans.skipped;
	*read = c->nread;
}

void
sift_proj_cursor_window(SiftProjCursor *c, int64 lo, int64 hi)
{
	c->keyed = true;
	c->key_lo = lo;
	c->key_hi = hi;
}

void
sift_proj_cursor_progressive(SiftProjCursor *c)
{
	SIFT_INVARIANT(c->spec->parent == NULL, "the rows of a projection part hold no arrays");
	c->progressive = true;
}

void
sift_proj_cursor_columns(SiftProjCursor *c, const bool *want)
{
	c->want = want;
}

/* The rows [r0, r1) of the granule from the first to the last whose first sort column is in the window, or cannot say (an exception tag) */
static void
window_rows(const SiftProjCursor *c, const SiftGranuleView *v, int *r0, int *r1)
{
	const int	col = c->spec->sort_cols[0];
	const int	len = sift_granule_chunk_len(v, col);
	int64	   *iv = palloc(Max(len, 1) * sizeof(int64));
	bool	   *nv = palloc(Max(len, 1) * sizeof(bool));
	uint8	   *tv = palloc(Max(len, 1));

	sift_granule_ints(v, col, iv, nv);
	sift_granule_tags(v, col, tv);
	*r0 = *r1 = 0;
	for (int r = 0; r < len; r++)
		if (tv[r] != 0 || (!nv[r] && iv[r] >= c->key_lo && iv[r] <= c->key_hi))
		{
			if (*r1 == 0)
				*r0 = r;
			*r1 = r + 1;
		}
	pfree(iv);
	pfree(nv);
	pfree(tv);
}

static void
open_granule(SiftProjCursor *c, uint32 g)
{
	SiftGranuleDesc gd;
	char	   *rec = sift_part_read_granule_cols(c->rel, c->part, g, c->want, &gd);

	c->view = sift_granule_open_part(c->rel, c->part, rec, gd.data_len, &gd);
	c->g = g;
	c->gn = gd.nrows;
	c->hi = 0;
	sift_dv_cursor_init(&c->dgc, c->dv ? sift_dv_granule(c->dv, g) : NULL);
	c->nread++;
}

/* The rows of the granule in hand that the key window leaves, or all of them */
static void
load_whole(SiftProjCursor *c)
{
	int			r0 = 0,
				r1 = c->gn;

	if (c->keyed)
		window_rows(c, &c->view, &r0, &r1);
/* check-memory: allow bounded to one granule: SIFT_GRANULE_ROWS rows */
	c->one = sift_rowbuf_new(c->spec);
	sift_rowbuf_add_granule_rows(c->one, &c->view, c->spec, r0, r1, c->want);
	c->r = 0;
	c->r0 = r0;
	c->lo = 0;
	c->hi = c->gn;
}

typedef struct WinCol
{
	int64	   *i;
	double	   *f;
	SiftStrCell *s;
	bool	   *isnull;
	uint8	   *tag;
} WinCol;

/* Column col of the rows named, decoded at their own values */
static WinCol
win_column(const SiftGranuleView *v, int col, SiftCellKind kind, const uint16 *rows, int m)
{
	WinCol		w = {.isnull = palloc(m),.tag = palloc(v->hdr->nrows)};

	sift_granule_tags(v, col, w.tag);
	if (kind == SIFT_CELL_INT)
		sift_granule_point_ints(v, col, rows, m, w.i = palloc(m * sizeof(int64)), w.isnull);
	else if (kind == SIFT_CELL_F8)
		sift_granule_point_f8(v, col, rows, m, w.f = palloc(m * sizeof(double)), w.isnull);
	else
		sift_granule_point_strs(v, col, rows, m, w.s = palloc(m * sizeof(SiftStrCell)));
	return w;
}

static SiftCell
win_cell(const SiftGranuleView *v, const WinCol *w, int col, SiftCellKind kind, const uint16 *rows, int j)
{
	SiftCell	cell = {.tag = w->tag[rows[j]]};

	cell.isnull = kind == SIFT_CELL_STR ? w->s[j].isnull : w->isnull[j];
	if (kind == SIFT_CELL_STR)
	{
		cell.s = w->s[j].p;
		cell.slen = w->s[j].len;
	}
	else
	{
		cell.i = w->i ? w->i[j] : 0;
		cell.f = w->f ? w->f[j] : 0;
	}
	if (cell.tag != 0)
		cell.xp = sift_granule_exc_payload(v, col, rows[j], &cell.xplen);
	return cell;
}

/* The rows [lo, hi) of the granule in hand, at most SIFT_POINT_ROWS_MAX of them, decoded at their own values and not through the rows before them; the columns nobody reads are null */
static void
add_window(SiftProjCursor *c, SiftRowBuf *rb, int lo, int hi)
{
	MemoryContext tmp = sift_mem_allocset_create(CurrentMemoryContext, "sift proj window");
	MemoryContext old = MemoryContextSwitchTo(tmp);
	const int	m = hi - lo;
	uint16		rows[SIFT_POINT_ROWS_MAX];
	uint64	   *tids = palloc(hi * sizeof(uint64));
	WinCol	   *col = palloc0(c->spec->ncols * sizeof(WinCol));
	SiftCell   *cells = palloc0(c->spec->ncols * sizeof(SiftCell));
	uint8		key[SIFT_BOUND_BYTES];

	sift_granule_tids_head(&c->view, hi, tids);
	for (int j = 0; j < m; j++)
		rows[j] = lo + j;
	for (int k = 0; k < c->spec->ncols; k++)
		if (c->want == NULL || c->want[k])
			col[k] = win_column(&c->view, k, c->spec->kinds[k], rows, m);
	for (int j = 0; j < m; j++)
	{
		for (int k = 0; k < c->spec->ncols; k++)
			cells[k] = c->want == NULL || c->want[k] ? win_cell(&c->view, &col[k], k, c->spec->kinds[k], rows, j) : (SiftCell) {.isnull = true};
		sift_row_key(c->spec, cells, tids[lo + j], key);
		MemoryContextSwitchTo(old);
		sift_rowbuf_add(rb, tids[lo + j], FrozenTransactionId, 0, 0, key, cells, NULL);
		MemoryContextSwitchTo(tmp);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
}

/* The rows [lo, hi) of the granule in hand: a window the point decode reads, else the whole granule, whose first lo rows the cursor has been through */
static void
load_window(SiftProjCursor *c, int lo, int hi)
{
	c->lo = lo;
	c->hi = hi;
	c->nwin++;
	if (hi - lo <= SIFT_POINT_ROWS_MAX && hi - lo < c->gn)
	{
/* check-memory: allow bounded to one granule: SIFT_GRANULE_ROWS rows */
		c->one = sift_rowbuf_new(c->spec);
		add_window(c, c->one, lo, hi);
		c->r = 0;
		c->r0 = lo;
		return;
	}
/* check-memory: allow bounded to one granule: SIFT_GRANULE_ROWS rows */
	c->one = sift_rowbuf_new(c->spec);
	sift_rowbuf_add_granule_rows(c->one, &c->view, c->spec, 0, c->gn, c->want);
	c->r = lo;
	c->r0 = 0;
}

static int
win_width(const SiftProjCursor *c)
{
	return c->nwin < PROJ_WIN_DOUBLED ? PROJ_WIN_FIRST << c->nwin : c->gn;
}

/* The next rows to read: the next window of the granule in hand, else the first of the next granule; false past the last */
static bool
load_next(SiftProjCursor *c)
{
	const bool	windows = c->progressive && !c->keyed;
	uint32		g;

	if (windows && c->hi < c->gn)
		load_window(c, c->hi, Min(c->gn, c->hi + win_width(c)));
	else if (sift_proj_spans_next(&c->spans, &g))
	{
		open_granule(c, g);
		if (windows)
			load_window(c, 0, Min(c->gn, win_width(c)));
		else
			load_whole(c);
	}
	else
		return false;
	return true;
}

bool
sift_proj_cursor_next(SiftProjCursor *c, const SiftRowBuf **rows, int *r)
{
	for (;;)
	{
		while (c->one != NULL && c->r < c->one->n)
		{
			int			at = c->r++;
			bool		dead = false;

			(void) sift_dv_cursor_find(&c->dgc, (uint16) (c->r0 + at), &dead);
			if (dead == c->dead_only)
			{
				*rows = c->one;
				*r = at;
				return true;
			}
		}
		if (c->one != NULL)
		{
			sift_rowbuf_free(c->one);
			c->one = NULL;
		}
		if (!load_next(c))
			return false;
	}
}

void
sift_proj_cursor_close(SiftProjCursor *c)
{
	if (c->one != NULL)
		sift_rowbuf_free(c->one);
	pfree(c);
}

void
sift_proj_scan_granule(Relation rel, const SiftPart *part, uint32 g, const SiftPartSpec *spec, const uint64 *mask, SiftProjRowFn fn, void *arg)
{
	SiftGranuleDesc gd;
	char	   *rec = sift_part_read_granule(rel, part, g, &gd);
	SiftGranuleView v = sift_granule_open_part(rel, part, rec, gd.data_len, &gd);
/* check-memory: allow bounded to one granule: SIFT_GRANULE_ROWS rows */
	SiftRowBuf *rows = sift_rowbuf_new(spec);

	sift_rowbuf_add_granule(rows, &v, NULL, spec, NULL);
	for (int r = 0; r < rows->n; r++)
		if ((mask[r / 64] >> (r % 64)) & 1)
			fn(arg, rows, r);
	sift_rowbuf_free(rows);
}

void
sift_proj_scan_part(Relation rel, const SiftPartDesc *d, uint32 part_id, const SiftPartSpec *spec, bool dead_only, SiftProjRowFn fn, void *arg)
{
	SiftProjCursor *c = sift_proj_cursor_open(rel, d, part_id, spec, dead_only);
	const SiftRowBuf *rows;
	int			r;

	while (sift_proj_cursor_next(c, &rows, &r))
		fn(arg, rows, r);
	sift_proj_cursor_close(c);
}
