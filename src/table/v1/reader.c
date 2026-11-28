/*
 * table/v1/reader.c - the unit reader of a sift_table (col/v1/reader.h): the stored units of every column layout the table's segments were written for, the rows of its
 * write buffers as in-memory batches one superblock at a time, and the rows the snapshot sees, which are the table's (a superblock without tombstones or write buffer rows is
 * entirely visible, the others are filtered by sift_tbl_filter_page like every other reader of the table does).
 */
#include "postgres.h"

#include "pgstat.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/col.h"
#include "col/v1/seg/seg.h"
#include "col/v1/reader.h"
#include "core/memtrack.h"
#include "table/v1/table.h"
#include "table/v1/reader.h"

/*
 * The table's own mask, not the heap's map: an SB is plain when it holds no write buffer row and no tombstone among those synced when the source
 * opened (tbl_open), before it read any row. A VACUUM of the table clears the liveness of rows behind tombstones, and a scan that holds
 * those tombstones sends their SBs to the filter whatever liveness it reads later, so no read of the mask follows a liveness it should have
 * preceded and the source needs no directory guard (sift_vis_guard).
 */
static void
tbl_sb_mask(void *arg, uint32 sb, uint64 *pagemask)
{
	SiftV1Tbl  *t = arg;
	bool		plain = (sb < t->wbuf_lo || sb > t->wbuf_hi) &&
		!sift_tomb_any(&t->st->tomb, (uint64) sb * (SIFT_SB_PAGES << 16), (uint64) (sb + 1) * (SIFT_SB_PAGES << 16) - 1) &&
		sift_tbl_vers_sb(t->rel, t->snapshot, t->vers, sb) == SIFT_TBL_VER_ALL;

	memset(pagemask, plain ? 0xFF : 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
}

static int
tbl_filter_page(void *arg, BlockNumber blk, uint64 *offmask)
{
	SiftV1Tbl  *t = arg;

	return sift_tbl_filter_page_vers(t->rel, &t->ss, blk, offmask, false);
}

/* The leaf of schema that holds leaf `want` of the layout: of the table's column (a store of another kind is a column the table changed the type of) or of a declared path (its def has to be the same) */
static int
part_col(const TblPart *p, const SiftColDef *want)
{
	for (int j = 0; j < p->schema->ncols; j++)
	{
		const SiftColDef *have = &p->schema->cols[j];

		if (have->attno != want->attno || strcmp(have->path ? have->path : "", want->path) != 0)
			continue;
		if (sift_v1_tbl_leaf_is_path(want))
		{
			if (have->kind == want->kind && have->store == want->store && have->type == want->type)
				return j;
			continue;
		}
		/* a uuid written before it was a dictionary is a VAR leaf of images: the segment is read through the datum, the next rewrite makes it a dictionary */
		SIFT_INVARIANT(have->store == want->store || (have->store == SIFT_COLS_VAR && want->store == SIFT_COLS_STR), "sift_table: a segment holds column %d in another storage than the table's", want->attno);
		return j;
	}
	return -1;
}

static void
part_open(SiftV1Tbl *t, TblPart *p, const SiftColSchema *schema, const SiftColSchema *cur, const SiftColSchema *const *layouts, const SiftColVisibility *vis,
		  uint32 sbs)
{
	p->schema = schema;
	p->src = sift_col_open_table(t->rel, schema, t->dir, layouts, vis, sbs, t->cxt);
	p->col = palloc(t->lay->nleaves * sizeof(int));
	p->leaf_of_col = palloc(schema->ncols * sizeof(int));
	memset(p->leaf_of_col, -1, schema->ncols * sizeof(int));
	for (int i = 0; i < t->lay->nleaves; i++)
	{
		p->col[i] = part_col(p, &cur->cols[i]);
		if (p->col[i] >= 0)
			p->leaf_of_col[p->col[i]] = i;
	}
}

/*
 * What a worker opens its source on: the directory and the write buffers the leader's source read (the items of the scan are superblocks of that
 * directory, and its tombstones are those of the log it names), so that no participant reads another table than the leader does
 */
typedef struct TblView
{
	int			nwbuf;
	Size		dir_len;
	char		data[FLEXIBLE_ARRAY_MEMBER];	/* the directory (dir_len bytes), then the write buffers */
} TblView;

static Size
view_wbuf_at(Size dir_len)
{
	return MAXALIGN(offsetof(TblView, data) + dir_len);
}

Size
sift_v1_tbl_view_size(void *impl)
{
	SiftV1Tbl  *t = impl;

	return view_wbuf_at(sift_dir_bytes(t->dir)) + t->nwbuf * sizeof(SiftTblSeg);
}

void
sift_v1_tbl_view_export(void *impl, void *buf)
{
	SiftV1Tbl  *t = impl;
	TblView    *v = buf;

	v->nwbuf = t->nwbuf;
	v->dir_len = sift_dir_bytes(t->dir);
	memcpy(v->data, t->dir, v->dir_len);
	memcpy((char *) buf + view_wbuf_at(v->dir_len), t->wbuf, t->nwbuf * sizeof(SiftTblSeg));
}

/* The directory, write buffers and tombstones the scan reads: the leader's (view), else the table's now. Its versions follow the directory */
static void
tbl_load_state(SiftV1Tbl *t, SiftTblRel *st, const TblView *view)
{
	if (view != NULL)
	{
		t->dir = palloc(view->dir_len);
		memcpy(t->dir, view->data, view->dir_len);
		t->nwbuf = view->nwbuf;
		t->wbuf = sift_tbl_wbuf_copy((const SiftTblSeg *) ((const char *) view + view_wbuf_at(view->dir_len)), t->nwbuf, CurrentMemoryContext);
		sift_tomb_sync_at(t->rel, &st->tomb, t->dir);
	}
	else
	{
		sift_tomb_sync(t->rel, &st->tomb);
		t->dir = sift_dir_copy(st->dir);
		t->nwbuf = st->nwbuf;
		t->wbuf = sift_tbl_wbuf_copy(st->wbuf, t->nwbuf, CurrentMemoryContext);
		for (int i = 0; i < t->nwbuf; i++)
			sift_tbl_seg_load(t->rel, &t->wbuf[i]);
	}
	t->vers = sift_tbl_vers_build(t->rel, st, t->dir, t->cxt);
	t->wbuf_lo = PG_UINT32_MAX;
	for (int i = 0; i < t->nwbuf; i++)
	{
		if (t->wbuf[i].npages == 0)
			continue;
		t->wbuf_lo = Min(t->wbuf_lo, t->wbuf[i].vbase / SIFT_SB_PAGES);
		t->wbuf_hi = Max(t->wbuf_hi, (t->wbuf[i].vbase + t->wbuf[i].npages - 1) / SIFT_SB_PAGES);
	}
	t->ss = (SiftTblScanState) {.st = st,.wbuf = t->wbuf,.nwbuf = t->nwbuf,.vers = t->vers,.snapshot = t->snapshot};
}

void
sift_v1_tbl_init(SiftV1Tbl *t, Relation heap, Snapshot snapshot, MemoryContext mcxt, const void *view)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftTblRel *st = sift_tbl_rel(heap);
	const SiftColSchema *cur;
	const SiftColSchema **layouts;
	SiftColVisibility vis = {tbl_sb_mask, tbl_filter_page, NULL, t};
	uint32		sbs;

	t->rel = heap;
	t->st = st;
	t->cxt = mcxt;
	t->snapshot = RegisterSnapshot(snapshot);
	t->last = -1;
	sift_srcsel_init(&t->sel, mcxt, RelationGetRelid(heap));
	pgstat_count_heap_scan(heap);
	sift_tbl_auto_defrag_tick(heap, st);
	sift_tbl_refresh(heap, st);
	sift_tbl_scan_note(heap, st);
	tbl_load_state(t, st, view);
	t->lay = sift_v1_tbl_layout(heap);
	t->dict = palloc0(sizeof(SiftSrcDict) * t->lay->nleaves);
	t->adict = palloc0(sizeof(SiftSrcDict) * t->lay->nleaves);
	t->absent = sift_v1_tbl_absent(heap, t->lay);
	t->buf = palloc0(sizeof(SiftSrcLeafBuf) * t->lay->nleaves);
	t->raw = palloc0(sizeof(bool) * t->lay->nleaves);
	t->wcxt = palloc0(sizeof(MemoryContext) * t->lay->nleaves);
	sbs = SIFT_TBL_NEXT_VBLOCK(&t->dir->meta) / SIFT_SB_PAGES + 1;
	cur = sift_tbl_layout_schema(heap, st);
	t->part = palloc0((t->dir->nsegs + 1) * sizeof(TblPart));
	layouts = palloc0(Max(t->dir->nsegs, 1) * sizeof(SiftColSchema *));
	for (int i = 0; i < t->dir->nsegs; i++)
		if (t->dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
			layouts[i] = sift_tbl_seg_schema(heap, st, &t->dir->segs[i]);
	part_open(t, &t->part[t->nparts++], cur, cur, layouts, &vis, sbs);
	for (int i = 0; i < t->dir->nsegs; i++)
	{
		int			k;

		if (layouts[i] == NULL)
			continue;
		for (k = 0; k < t->nparts && t->part[k].schema->hash != layouts[i]->hash; k++)
			;
		if (k == t->nparts)
			part_open(t, &t->part[t->nparts++], layouts[i], cur, layouts, &vis, sbs);
	}
	MemoryContextSwitchTo(old);
	SIFT_COL_OPEN_GATE();
}

void
sift_v1_tbl_close(void *impl)
{
	SiftV1Tbl  *t = impl;

	for (int k = 0; k < t->nparts; k++)
		sift_col_close(t->part[k].src);
	UnregisterSnapshot(t->snapshot);
}

static void
tbl_pruned(void *impl, SiftV6GranCounts *out)
{
	SiftV1Tbl  *t = impl;

	for (int k = 0; k < t->nparts; k++)
		sift_v1_units_pruned(t->part[k].src, out);
	out->pruned_zone += t->sel.units_skipped;
}

const SiftSrcLayout *
sift_v1_tbl_source_layout(const void *impl)
{
	return ((const SiftV1Tbl *) impl)->lay;
}

uint32
sift_v1_tbl_nsb(void *impl)
{
	return sift_col_heap_sbs(((SiftV1Tbl *) impl)->part[0].src);
}

static bool
wbuf_fed(const SiftV1Tbl *t, uint32 sb)
{
	for (int i = 0; i < t->nwb; i++)
		if (t->wb[i].sb == sb)
			return true;
	return false;
}

/* The end of the window of a begin from sb: the superblock of write buffer rows alone (its batch is the memory of the scan, and the next window frees it), or the stored ones up to the next */
static uint32
wbuf_window(const SiftV1Tbl *t, uint32 sb)
{
	uint32		next = t->range_hi;

	for (int i = 0; i < t->nwbuf; i++)
	{
		uint32		first = t->wbuf[i].vbase / SIFT_SB_PAGES;

		if (sb >= first && sb <= (t->wbuf[i].vbase + t->wbuf[i].npages - 1) / SIFT_SB_PAGES)
			return Min(t->range_hi, sb + 1);
		if (first > sb)
			next = Min(next, first);
	}
	return next;
}

/* The batches of the write buffer rows outside [sb_lo, sb_hi) are freed: a scan holds the rows of the range it reads, and a pass over them again reads them again */
static void
drop_wbuf(SiftV1Tbl *t, uint32 sb_lo, uint32 sb_hi)
{
	int			n = 0;

	sift_col_drop_batches(t->part[0].src, sb_lo, sb_hi);
	for (int i = 0; i < t->nwb; i++)
		if (t->wb[i].sb >= sb_lo && t->wb[i].sb < sb_hi)
			t->wb[n++] = t->wb[i];
		else
			MemoryContextDelete(t->wb[i].cxt);
	t->nwb = n;
}

static bool
wbuf_has(const SiftV1Tbl *t, uint32 sb)
{
	for (int i = 0; i < t->nwbuf; i++)
		if (sb >= t->wbuf[i].vbase / SIFT_SB_PAGES && sb <= (t->wbuf[i].vbase + t->wbuf[i].npages - 1) / SIFT_SB_PAGES)
			return true;
	return false;
}

/* The write buffer rows of superblock sb, when it holds some that the source does not hold yet, in a context of its own */
static void
add_wbuf(SiftV1Tbl *t, uint32 sb)
{
	MemoryContext cx;
	SiftColBatch *b;

	if (!wbuf_has(t, sb) || wbuf_fed(t, sb))
		return;
	cx = AllocSetContextCreate(t->cxt, "sift_table write buffer batch", ALLOCSET_DEFAULT_SIZES);
	b = sift_tbl_wbuf_batch(t->rel, t->st, t->wbuf, t->nwbuf, sb, cx);
	sift_col_add_batches(t->part[0].src, &b, 1);
	if (t->nwb == t->wbcap)
	{
		t->wbcap = Max(8, t->wbcap * 2);
		SIFT_REGROW_IN(t->cxt, t->wb, t->wbcap * sizeof(TblWbatch));
	}
	t->wb[t->nwb++] = (TblWbatch) {sb, cx};
}

static void
begin_window(SiftV1Tbl *t, uint32 sb_lo)
{
	uint32		sb_hi = wbuf_window(t, sb_lo);

	drop_wbuf(t, sb_lo, sb_hi);
	add_wbuf(t, sb_lo);
	for (int k = 0; k < t->nparts; k++)
	{
		TblPart    *p = &t->part[k];

		if (t->begin_part)
			t->begin_part(t, k, sb_lo, sb_hi);
		else
		{
			SiftColPred preds[SIFT_SRCZONE_MAX];
			int			n = sift_srczone_preds(&p->zones, t->cxt, p->col, t->absent, t->prune, t->nprune, preds);

			sift_col_begin_zones(p->src, preds, n, p->zones.zone, p->zones.nzone, sb_lo, sb_hi);
		}
		p->have = sift_col_next(p->src, &p->unit);
	}
	t->win_hi = sb_hi;
	t->last = -1;
}

void
sift_v1_tbl_begin(void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune)
{
	SiftV1Tbl  *t = impl;

	t->prune = prune;
	t->nprune = nprune;
	t->range_hi = sb_hi;
	begin_window(t, sb_lo);
}

/* The units of the sources in ascending superblock: each source holds its head, and the one returned last advances on the next call; a range is read a window at a time */
bool
sift_v1_tbl_next(void *impl, SiftSrcUnit *unit)
{
	SiftV1Tbl  *t = impl;

	t->sel.compact = false;
	for (;;)
	{
		TblPart    *best = NULL;
		uint32		nrow;

		if (t->last >= 0)
			t->part[t->last].have = sift_col_next(t->part[t->last].src, &t->part[t->last].unit);
		for (int k = 0; k < t->nparts; k++)
			if (t->part[k].have && (best == NULL || t->part[k].unit.sb < best->unit.sb))
				best = &t->part[k];
		if (best == NULL && t->win_hi < t->range_hi)
		{
			begin_window(t, t->win_hi);
			continue;
		}
		if (best == NULL)
			return false;
		t->last = best - t->part;
		nrow = best->unit.nrows;
		if (t->sel.keep && !sift_srcsel_unit(&t->sel, &best->unit, &nrow))
			continue;
		unit->sb = best->unit.sb;
		unit->nrow = nrow;
		unit->nsu = best->unit.nsu;
		unit->priv = best;
		return true;
	}
}


void
sift_v1_tbl_select(void *impl, const SiftFilter *keep)
{
	((SiftV1Tbl *) impl)->sel.keep = keep;
}


void
sift_v1_tbl_live(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	TblPart    *p = unit->priv;

	sift_srcsel_live(&((SiftV1Tbl *) impl)->sel, &p->unit, bits);
}

void
sift_v1_tbl_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	TblPart    *p = unit->priv;

	sift_srcsel_vet(&((SiftV1Tbl *) impl)->sel, p->src, &p->unit, bits);
}

bool
sift_v1_tbl_vet_needed(void *impl, const SiftSrcUnit *unit)
{
	TblPart    *p = unit->priv;

	return sift_srcsel_vet_needed(&p->unit);
}


/* ---- ordered top-N (docs/COL-SEARCH.md): the rows of a unit in the numbering of the executor are the idx-th rows of the unit as stored when a match set compacted it ---- */

void
sift_v1_tbl_tids(void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids)
{
	uint32	   *ords = sift_srcsel_ords(&((SiftV1Tbl *) impl)->sel, rows, n);

	sift_col_row_tids(&((TblPart *) unit->priv)->unit, ords, n, tids);
	pfree(ords);
}


static void *
tbl_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, const void *view)
{
	SiftV1Tbl  *t = MemoryContextAllocZero(mcxt, sizeof(SiftV1Tbl));

	sift_v1_tbl_init(t, heap, snapshot, mcxt, view);
	return t;
}

/* only a path leaf has types, and the layout offers the paths only when every segment has them */
static bool
tbl_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits, bool *known)
{
	SiftV1Tbl  *t = impl;
	TblPart    *p = unit->priv;
	const SiftSrcLeaf *l = &t->lay->leaf[leaf];

	*known = sift_srcsel_filter_types(&t->sel, p->src, &p->unit, p->col[leaf], l->typid, l->astext, types, bits);
	return sift_srcsel_rows_left(bits, unit->nrow);
}

static int
tbl_unit_rows(void *impl, const SiftSrcUnit *unit)
{
	return ((SiftV1Tbl *) impl)->sel.compact ? -1 : sift_v1_unit_rows(&((TblPart *) unit->priv)->unit);
}

/* a segment of an older layout may lack the column: the unit is read, and the scan gives the default of the catalog */
static bool
tbl_unit_agg(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg)
{
	const TblPart *p = unit->priv;

	return p->col[leaf] >= 0 && sift_v1_unit_agg(&p->unit, p->col[leaf], &p->schema->cols[p->col[leaf]], agg);
}

static bool
tbl_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, const SiftSrcRollupNeed *need, SiftSrcHist *h)
{
	SiftV1Tbl  *t = impl;
	const TblPart *p = unit->priv;

	if (p->col[dim] < 0)
		return false;
	if (t->hcx == NULL)
		t->hcx = sift_mem_allocset_create(t->cxt, "sift v1 table histogram");
	MemoryContextReset(t->hcx);
	return sift_v1_unit_hist(&p->unit, p->col[dim], &p->schema->cols[p->col[dim]], t->hcx, p->col, p->leaf_of_col, need, &t->hdict, h);
}

const SiftV1ReaderOps sift_v1_tbl_reader = {
	.layout = sift_v1_tbl_layout,
	.stats = sift_v1_tbl_stats,
	.preagg = sift_v1_tbl_preagg,
	.open = tbl_open,
	.view_size = sift_v1_tbl_view_size,
	.view_export = sift_v1_tbl_view_export,
	.close = sift_v1_tbl_close,
	.pruned = tbl_pruned,
	.source_layout = sift_v1_tbl_source_layout,
	.nsb = sift_v1_tbl_nsb,
	.select = sift_v1_tbl_select,
	.begin = sift_v1_tbl_begin,
	.next = sift_v1_tbl_next,
	.live = sift_v1_tbl_live,
	.vet_needed = sift_v1_tbl_vet_needed,
	.vet = sift_v1_tbl_vet,
	.filter_types = tbl_filter_types,
	.read = sift_v1_tbl_read,
	.tids = sift_v1_tbl_tids,
	.unit_rows = tbl_unit_rows,
	.unit_agg = tbl_unit_agg,
	.unit_hist = tbl_unit_hist
};
