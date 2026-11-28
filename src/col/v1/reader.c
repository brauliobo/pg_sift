/*
 * col/v1/reader.c - the unit reader of a sift_col index (col/v1/reader.h): the units of its segments and write buffers, a match set's rows of them, the rows
 * that are live and visible, and the leaves decoded into the vectors of col/v1/src_exec.h.
 */
#include "postgres.h"

#include "pgstat.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/col.h"
#include "col/v1/pagg.h"
#include "col/v1/src_zone.h"
#include "col/v1/reader_int.h"
#include "core/memtrack.h"

static void sift_v1_idx_wide_dict(SiftV1Idx *r, int leaf, SiftSrcWide *w, const SiftColVec *v, uint32 n);

static void
sift_v1_idx_init(SiftV1Idx *r, Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, uint32 flags, const void *view, const SiftKeyRange *keys)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);

	r->cxt = mcxt;
	r->heap = heap;
	r->index = index;
	r->snapshot = snapshot;
	r->cs = sift_col_open(heap, index, snapshot, mcxt, flags, view, keys);
	pgstat_count_index_scan(index);
	sift_col_scan_note(index, r->cs);
	r->lay = sift_v1_idx_layout(index);
	r->dict = palloc0(sizeof(SiftSrcDict) * r->lay->nleaves);
	r->buf = palloc0(sizeof(SiftSrcLeafBuf) * r->lay->nleaves);
	r->raw = palloc0(sizeof(bool) * r->lay->nleaves);
	r->wide = palloc0(sizeof(SiftSrcWide) * r->lay->nleaves);
	r->wgen = palloc0(sizeof(uint64) * r->lay->nleaves);
	r->ugen = 1;
	r->wide_dict = sift_v1_idx_wide_dict;
	sift_srcsel_init(&r->sel, mcxt, RelationGetRelid(heap));
	MemoryContextSwitchTo(old);
}

void *
sift_v1_idx_open_keys(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, const void *view, const SiftKeyRange *keys)
{
	SiftV1Idx  *r = MemoryContextAllocZero(mcxt, sizeof(SiftV1Idx));

	sift_v1_idx_init(r, heap, index, snapshot, mcxt, SIFT_COLO_NOACL | SIFT_COLO_POINTS | SIFT_COLO_POINT_ROWS, view, keys);
	return r;
}

static void *
idx_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, const void *view)
{
	return sift_v1_idx_open_keys(heap, index, snapshot, mcxt, view, NULL);
}

SiftColSource *
sift_v1_idx_colsource(void *impl)
{
	return ((SiftV1Idx *) impl)->cs;
}

static Size
idx_view_size(void *impl)
{
	return sift_col_view_size(((SiftV1Idx *) impl)->cs);
}

static void
idx_view_export(void *impl, void *buf)
{
	sift_col_view_export(((SiftV1Idx *) impl)->cs, buf);
}

/* As the end of an index scan: the reads of the scan are written to the metapage and the backend looks at the maintenance of the index (col/v1/maint/tick.c) */
static void
sift_v1_idx_close(void *impl)
{
	SiftV1Idx  *r = impl;
	int64		dirty_rows = sift_col_counters(r->cs)->dirty_rows;

	sift_col_close(r->cs);
	sift_col_maint_tick(RelationGetRelid(r->index), dirty_rows);
}

void
sift_v1_units_pruned(const SiftColSource *src, SiftV6GranCounts *out)
{
	const SiftColCounters *c = sift_col_counters(src);

	out->pruned_zone += c->units_pruned - c->units_set;
	out->pruned_skip += c->units_set;
	out->point_rows += c->point_rows;
	out->vetted += c->rows_vetted;
}

static void
idx_pruned(void *impl, SiftV6GranCounts *out)
{
	const SiftV1Idx *r = impl;

	sift_v1_units_pruned(r->cs, out);
	out->pruned_zone += r->sel.units_skipped;
}

static const SiftSrcLayout *
sift_v1_idx_source_layout(const void *impl)
{
	return ((const SiftV1Idx *) impl)->lay;
}

static uint32
sift_v1_idx_nsb(void *impl)
{
	return sift_col_heap_sbs(((SiftV1Idx *) impl)->cs);
}

static void
sift_v1_idx_select(void *impl, const SiftFilter *keep)
{
	((SiftV1Idx *) impl)->sel.keep = keep;
}

static void
idx_begin(void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune)
{
	SiftV1Idx  *r = impl;
	SiftColPred preds[SIFT_SRCZONE_MAX];
	int			n = sift_srczone_preds(&r->zones, r->cxt, NULL, NULL, prune, nprune, preds);

	sift_col_begin_zones(r->cs, preds, n, r->zones.zone, r->zones.nzone, sb_lo, sb_hi);
}

static bool
sift_v1_idx_next(void *impl, SiftSrcUnit *unit)
{
	SiftV1Idx  *r = impl;
	uint32		nrow;

	r->sel.compact = false;
	do
	{
		if (!sift_col_next(r->cs, &r->unit))
			return false;
		r->ugen++;
		nrow = r->unit.nrows;
	} while (r->sel.keep && !sift_srcsel_unit(&r->sel, &r->unit, &nrow));
	unit->sb = r->unit.sb;
	unit->nrow = nrow;
	unit->nsu = r->unit.nsu;
	unit->priv = r;
	return true;
}

static void
sift_v1_idx_live(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	SiftV1Idx  *r = impl;

	sift_srcsel_live(&r->sel, &r->unit, bits);
}

static void
sift_v1_idx_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	SiftV1Idx  *r = impl;

	sift_srcsel_vet(&r->sel, r->cs, &r->unit, bits);
}

static bool
sift_v1_idx_vet_needed(void *impl, const SiftSrcUnit *unit)
{
	return sift_srcsel_vet_needed(&((SiftV1Idx *) impl)->unit);
}

/* rows ascend in the numbering the executor sees; those of a compacted unit are the idx-th rows of the unit as stored */
static void
sift_v1_idx_tids(void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids)
{
	SiftV1Idx  *r = impl;

	if (r->sel.compact)
	{
		uint32	   *ords = palloc(Max(n, 1) * sizeof(uint32));

		for (int i = 0; i < n; i++)
			ords[i] = r->sel.idx[rows[i]];
		sift_col_row_tids(&r->unit, ords, n, tids);
		pfree(ords);
	}
	else
		sift_col_row_tids(&r->unit, rows, n, tids);
}

/* ---- the reads ---- */

/* the dictionary of the wide leaf's rows (n of them) that a read returns: the entries of its distinct values */
static void
sift_v1_idx_wide_dict(SiftV1Idx *r, int leaf, SiftSrcWide *w, const SiftColVec *v, uint32 n)
{
	w->cxt = r->cxt;
	sift_srcwide_build(w, sift_col_def(r->cs, leaf), v, n);
	r->dict[leaf] = (SiftSrcDict) {(const void *) (uintptr_t) (UINT64CONST(0x7000000000000000) + ++r->wseq), w->nd, w->off, w->bytes};
}

/*
 * A wide leaf of the unit as a dictionary, made from its lanes by the first read of the leaf in the unit (the rows the executor
 * sees: those of a unit restricted to a match set are the kept ones) and shared by the reads after it, so that the executor's maps of the
 * dictionary are made once. A dictionary has an id of its own: the executor keeps what it derives by the id.
 */
static const SiftSrcWide *
wide_unit(SiftV1Idx *r, int leaf)
{
	SiftSrcWide *w = &r->wide[leaf];

	if (r->wgen[leaf] != r->ugen)
	{
		const SiftColDef *def = sift_col_def(r->cs, leaf);
		SiftColVec	v[SIFT_COL_MAX_LANES];

		for (int k = 0; k < sift_col_def_value_lanes(def); k++)
			sift_srcsel_read(&r->sel, &r->unit, leaf + k, &v[k]);
		r->wide_dict(r, leaf, w, v, v[0].n);
		r->wgen[leaf] = r->ugen;
	}
	return w;
}

/* the codes of the rows `rows` (n of them; NULL: all the unit's) of a wide leaf, in the caller's storage */
static void
wide_fill(SiftV1Idx *r, int leaf, const uint32 *rows, int n, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	const SiftSrcWide *w = wide_unit(r, leaf);
	uint32	   *codes = buf->data;
	int			m = rows ? n : (int) w->n;

	for (int i = 0; i < m; i++)
	{
		uint32		row = rows ? rows[i] : (uint32) i;

		codes[i] = w->codes[row];
		buf->isnull[i] = w->isnull[row];
	}
	memset(codes + m, 0, SIFT_SRC_PAD * sizeof(uint32));
	*vec = (SiftSrcVec) {.data = codes,.isnull = w->has_null ? buf->isnull : NULL,.dict = &r->dict[leaf],.n = m};
}

/* A compacted unit of a match set (col/v1/src_sel.h): the kept rows only, which the engine reads by their positions, not by lane */
static void
read_selected(SiftV1Idx *r, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	SiftColVec	v;

	sift_col_unit_lazy_dict(&r->unit, true);
	sift_srcsel_read(&r->sel, &r->unit, leaf, &v);
	sift_col_unit_lazy_dict(&r->unit, false);
	sift_srcsel_fill(&r->sel, &r->lay->leaf[leaf], &v, buf, &r->dict[leaf], vec);
}

/* the leaf decoded by the engine into the caller's storage, in the lane of the leaf's vector type */
static void
sift_v1_idx_read_into(void *impl, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	SiftV1Idx  *r = impl;

	if (sift_v1_idx_wide(r, leaf))
		wide_fill(r, leaf, NULL, 0, buf, vec);
	else if (r->sel.compact)
		read_selected(r, leaf, buf, vec);
	else
		sift_srcsel_read_lane(&r->sel, &r->unit, leaf, &r->lay->leaf[leaf], buf, &r->dict[leaf], vec);
}

/* read_into over the reader's own buffers of the leaf: valid until the next read of the leaf or the next unit */
static void
sift_v1_idx_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	SiftV1Idx  *r = impl;
	SiftSrcLeafBuf *b = &r->buf[leaf];

	sift_srcsel_leaf_buf(r->cxt, b, sift_col_lane_n(&r->unit, leaf));
	sift_v1_idx_read_into(impl, unit, leaf, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, vec);
}

static bool
idx_dict_match(void *impl, const SiftSrcUnit *unit, int leaf, SiftColOp op, const SiftColValue *c, uint64 *lut)
{
	return sift_col_unit_dict_match(&((SiftV1Idx *) impl)->unit, leaf, op, c, lut);
}

static bool
idx_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits, bool *known)
{
	SiftV1Idx  *r = impl;
	const SiftSrcLeaf *l = &r->lay->leaf[leaf];

	*known = sift_srcsel_filter_types(&r->sel, r->cs, &r->unit, leaf, l->typid, l->astext, types, bits);
	return sift_srcsel_rows_left(bits, unit->nrow);
}

static int
idx_unit_rows(void *impl, const SiftSrcUnit *unit)
{
	SiftV1Idx  *r = impl;

	return r->sel.compact ? -1 : sift_v1_unit_rows(&r->unit);
}

static bool
idx_unit_agg(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg)
{
	SiftV1Idx  *r = impl;

	return sift_v1_unit_agg(&r->unit, leaf, sift_col_def(r->cs, leaf), agg);
}

/* the memory of the last histogram or key presence of a unit, emptied for the next */
static MemoryContext
unit_cx(SiftV1Idx *r)
{
	if (r->hcx == NULL)
		r->hcx = sift_mem_allocset_create(r->cxt, "sift v1 histogram");
	MemoryContextReset(r->hcx);
	return r->hcx;
}

static bool
idx_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, const SiftSrcRollupNeed *need, SiftSrcHist *h)
{
	SiftV1Idx  *r = impl;

	return sift_v1_unit_hist(&r->unit, dim, sift_col_def(r->cs, dim % SIFT_SRC_CAL), unit_cx(r), NULL, NULL, need, &r->hdict, h);
}

static bool
idx_unit_rollup(void *impl, const SiftSrcUnit *unit, const SiftSrcRollupNeed *need, SiftSrcRollup *out)
{
	SiftV1Idx  *r = impl;

	return sift_v1_unit_rollup(&r->unit, unit_cx(r), NULL, NULL, need, r->rdict, out);
}

static void
idx_super(void *impl, bool on)
{
	sift_col_super(((SiftV1Idx *) impl)->cs, on);
}

static void
idx_split(void *impl, const SiftSrcUnit *unit)
{
	SiftV1Idx  *r = impl;

	sift_col_unit_split(r->cs, &r->unit);
}

static bool
idx_unit_except(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_col_unit_except(&((SiftV1Idx *) impl)->unit, leaf);
}

static bool
idx_unit_presence(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcPresence *p)
{
	SiftV1Idx  *r = impl;

	return sift_v1_unit_presence(&r->unit, leaf, unit_cx(r), p);
}

static const void *
idx_unit_seg(void *impl, const SiftSrcUnit *unit)
{
	SiftColSegment *seg;
	uint32		u;

	return sift_col_unit_stored(&((SiftV1Idx *) impl)->unit, &seg, &u) ? seg : NULL;
}

static bool
idx_seg_count(void *impl, const SiftSrcUnit *unit, int leaf, SiftColOp op, const SiftColValue *c, uint64 *rows)
{
	SiftV1Idx  *r = impl;

	return !r->sel.compact && sift_col_unit_seg_count(&r->unit, leaf, op, c, rows);
}

static bool
idx_seg_range_count(void *impl, const SiftSrcUnit *unit, int leaf, const SiftColRange *range, MemoryContext cx, uint64 *rows, Bitmapset **read)
{
	SiftV1Idx  *r = impl;

	return !r->sel.compact && sift_col_unit_seg_range(&r->unit, leaf, range, cx, rows, read);
}

static bool
idx_seg_hist(void *impl, const SiftSrcUnit *unit, int dim, SiftSrcHist *h)
{
	SiftV1Idx  *r = impl;

	return !r->sel.compact && sift_v1_seg_hist(&r->unit, dim, h);
}

const SiftV1ReaderOps sift_v1_idx_reader = {
	.layout = sift_v1_idx_layout,
	.stats = sift_v1_idx_stats,
	.preagg = sift_v1_idx_preagg,
	.key_leaf = sift_v1_idx_key_leaf,
	.open = idx_open,
	.view_size = idx_view_size,
	.view_export = idx_view_export,
	.close = sift_v1_idx_close,
	.pruned = idx_pruned,
	.source_layout = sift_v1_idx_source_layout,
	.nsb = sift_v1_idx_nsb,
	.select = sift_v1_idx_select,
	.begin = idx_begin,
	.next = sift_v1_idx_next,
	.live = sift_v1_idx_live,
	.vet_needed = sift_v1_idx_vet_needed,
	.vet = sift_v1_idx_vet,
	.filter_types = idx_filter_types,
	.read = sift_v1_idx_read,
	.dict_match = idx_dict_match,
	.tids = sift_v1_idx_tids,
	.unit_rows = idx_unit_rows,
	.unit_agg = idx_unit_agg,
	.unit_hist = idx_unit_hist,
	.unit_rollup = idx_unit_rollup,
	.unit_presence = idx_unit_presence,
	.unit_seg = idx_unit_seg,
	.seg_count = idx_seg_count,
	.seg_range_count = idx_seg_range_count,
	.super = idx_super,
	.split = idx_split,
	.unit_except = idx_unit_except,
	.seg_hist = idx_seg_hist
};
