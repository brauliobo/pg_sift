/*
 * col/f2/f2_proj_scan.c - the rows of a part, one at a time, granule by granule: the live rows of its deletion vector, or with dead_only the dead ones (the rows an AGG projection part still counts
 * after the base part was given a deletion vector). A SORT projection part is read through the same cursor, lazily, so that a merge with a limit stops decoding where it stops reading; sift_proj_cursor_seek leaves the cursor the granules its key marks say a statement can reach.
 */
#include "core/host.h"

#include "col/f2/delvec.h"
#include "col/f2/f2_proj.h"
#include "col/f2/f2_proj_span.h"
#include "col/f2/key_plan.h"
#include "col/f2/part_format.h"
#include "core/page_io.h"

#define SIFT_PROJ_SEC_KEY_MARKS	0x020D

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
	SiftRowBuf *one;
	int			r;
};

SiftProjCursor *
sift_proj_cursor_open(Relation rel, const SiftPartDesc *d, uint32 part_id, const SiftPartSpec *spec, bool dead_only)
{
	SiftProjCursor *c = palloc0(sizeof(SiftProjCursor));
	uint16	   *grows;

	c->rel = rel;
	c->part = sift_part_open(rel, d->header_blk, d->part_gen, part_id);
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

static void
load_granule(SiftProjCursor *c, uint32 g)
{
	SiftGranuleDesc gd;
	char	   *rec = sift_part_read_granule(c->rel, c->part, g, &gd);
	SiftGranuleView v = sift_granule_open_part(c->rel, c->part, rec, gd.data_len, &gd);

	c->one = sift_rowbuf_new(c->spec);
	sift_dv_cursor_init(&c->dgc, c->dv ? sift_dv_granule(c->dv, g) : NULL);
	sift_rowbuf_add_granule(c->one, &v, NULL, c->spec, NULL);
	c->r = 0;
	c->nread++;
}

bool
sift_proj_cursor_next(SiftProjCursor *c, const SiftRowBuf **rows, int *r)
{
	uint32		g;

	for (;;)
	{
		while (c->one != NULL && c->r < c->one->n)
		{
			int			at = c->r++;
			bool		dead = false;

			(void) sift_dv_cursor_find(&c->dgc, at, &dead);
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
		if (!sift_proj_spans_next(&c->spans, &g))
			return false;
		load_granule(c, g);
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
