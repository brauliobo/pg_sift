/*
 * col/v1/v1_reader.c - the unit reader of a sift_col index (col/v1/v1_reader.h): the units of its segments and write buffers, a match set's rows of them, the rows
 * that are live and visible, and the leaves decoded into the vectors of col/v1/src_exec.h.
 */
#include "postgres.h"

#include "pgstat.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/col.h"
#include "col/v1/v1_reader_int.h"

void
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
	r->tol = palloc0(sizeof(bool) * r->lay->nleaves);
	r->raw = palloc0(sizeof(bool) * r->lay->nleaves);
	r->wide = palloc0(sizeof(SiftSrcWide) * r->lay->nleaves);
	r->wgen = palloc0(sizeof(uint64) * r->lay->nleaves);
	r->ugen = 1;
	r->wide_dict = sift_v1_idx_wide_dict;
	sift_srcsel_init(&r->sel, mcxt, RelationGetRelid(heap));
	MemoryContextSwitchTo(old);
}

static void *
idx_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt)
{
	SiftV1Idx  *r = MemoryContextAllocZero(mcxt, sizeof(SiftV1Idx));

	sift_v1_idx_init(r, heap, index, snapshot, mcxt, SIFT_COLO_NOACL | SIFT_COLO_POINTS | SIFT_COLO_POINT_ROWS, NULL, NULL);
	return r;
}

void
sift_v1_idx_close(void *impl)
{
	sift_col_close(((SiftV1Idx *) impl)->cs);
}

const SiftSrcLayout *
sift_v1_idx_source_layout(const void *impl)
{
	return ((const SiftV1Idx *) impl)->lay;
}

uint32
sift_v1_idx_nsb(void *impl)
{
	return sift_col_heap_sbs(((SiftV1Idx *) impl)->cs);
}

void
sift_v1_idx_select(void *impl, const SiftFilter *keep)
{
	((SiftV1Idx *) impl)->sel.keep = keep;
}

static void
idx_begin(void *impl, uint32 sb_lo, uint32 sb_hi)
{
	sift_col_begin_zones(((SiftV1Idx *) impl)->cs, NULL, 0, NULL, 0, sb_lo, sb_hi);
}

bool
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

void
sift_v1_idx_live(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	SiftV1Idx  *r = impl;

	sift_srcsel_live(&r->sel, &r->unit, bits);
}

void
sift_v1_idx_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	SiftV1Idx  *r = impl;

	sift_srcsel_vet(&r->sel, r->cs, &r->unit, bits);
}

bool
sift_v1_idx_vet_needed(void *impl, const SiftSrcUnit *unit)
{
	return sift_srcsel_vet_needed(&((SiftV1Idx *) impl)->unit);
}

/* rows ascend in the numbering the executor sees; those of a compacted unit are the idx-th rows of the unit as stored */
void
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

/* of the unit as stored: a compacted unit of a match set yields fewer */
int64
sift_v1_idx_leaf_n(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_col_lane_n(&((SiftV1Idx *) impl)->unit, leaf);
}

/* the dictionary of the wide leaf's rows (n of them) that a read returns: the entries of its distinct values */
void
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
	sift_srcsel_fill(&r->sel, &r->lay->leaf[leaf], r->tol[leaf], &v, buf, &r->dict[leaf], vec);
}

/* the leaf decoded by the engine into the caller's storage, in the lane of the leaf's vector type */
void
sift_v1_idx_read_into(void *impl, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	SiftV1Idx  *r = impl;

	if (sift_v1_idx_wide(r, leaf))
		wide_fill(r, leaf, NULL, 0, buf, vec);
	else if (r->sel.compact)
		read_selected(r, leaf, buf, vec);
	else
		sift_srcsel_read_lane(&r->sel, &r->unit, leaf, &r->lay->leaf[leaf], r->tol[leaf], buf, &r->dict[leaf], vec);
}

/* read_into over the reader's own buffers of the leaf: valid until the next read of the leaf or the next unit */
void
sift_v1_idx_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	SiftV1Idx  *r = impl;
	SiftSrcLeafBuf *b = &r->buf[leaf];

	sift_srcsel_leaf_buf(r->cxt, b, sift_col_lane_n(&r->unit, leaf));
	sift_v1_idx_read_into(impl, unit, leaf, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, vec);
}

/* The values of a read of the rows idx[0..m) of a unit of nrow rows, at their rows in the leaf's buffer; the other rows are zero */
static void
read_scatter(SiftV1Idx *r, int leaf, const SiftColVec *v, const uint32 *idx, size_t nrow, SiftSrcVec *vec)
{
	SiftSrcLeafBuf *b = &r->buf[leaf];

	sift_srcsel_leaf_buf(r->cxt, b, nrow);
	sift_srcsel_scatter(&r->lay->leaf[leaf], r->tol[leaf], v, idx, nrow, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, &r->dict[leaf], vec);
}

/*
 * The dictionary of a wide leaf for the rows of sel only, with their codes at their rows in the leaf's buffer (the other rows are zero): a
 * scan that keeps a few rows of a unit converts the values of those, not one per distinct value of the unit. It is not the dictionary of the
 * unit, so a whole read of the leaf in the same unit makes that one again.
 */
static void
wide_scatter(SiftV1Idx *r, int leaf, const SiftColSel *sel, const uint32 *idx, size_t nrow, SiftSrcVec *vec)
{
	SiftSrcWide *w = &r->wide[leaf];
	const SiftColDef *def = sift_col_def(r->cs, leaf);
	SiftColVec	v[SIFT_COL_MAX_LANES];
	SiftSrcLeafBuf *b = &r->buf[leaf];
	uint32	   *codes;

	sift_srcsel_leaf_buf(r->cxt, b, nrow);
	codes = b->data;
	for (int k = 0; k < sift_col_def_value_lanes(def); k++)
		sift_col_read(&r->unit, leaf + k, sel, &v[k]);
	r->wide_dict(r, leaf, w, v, sel->count);
	r->wgen[leaf] = 0;
	memset(codes, 0, (nrow + SIFT_SRC_PAD) * sizeof(uint32));
	if (w->has_null)
		memset(b->isnull, 0, nrow);
	for (uint32 k = 0; k < sel->count; k++)
	{
		codes[idx[k]] = w->codes[k];
		if (w->has_null)
			b->isnull[idx[k]] = w->isnull[k];
	}
	*vec = (SiftSrcVec) {.data = codes,.isnull = w->has_null ? b->isnull : NULL,.dict = &r->dict[leaf],.n = nrow};
}

void
sift_v1_idx_read_sel(void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec)
{
	SiftV1Idx  *r = impl;
	SiftColSel	sel;
	SiftColVec	v;
	uint32	   *idx;

	if (r->sel.compact)			/* the rows of a compacted unit are not the engine's: the whole leaf */
	{
		sift_v1_idx_read(impl, unit, leaf, vec);
		return;
	}
	sift_srcsel_bits(r->cs, unit->nrow, bits, &sel);
	idx = MemoryContextAlloc(r->cxt, (sel.count + SIFT_COL_IDXPAD) * sizeof(uint32));
	sift_col_sel_idx(&sel, idx);
	if (sift_v1_idx_wide(r, leaf))
		wide_scatter(r, leaf, &sel, idx, unit->nrow, vec);
	else
	{
		sift_col_unit_lazy_dict(&r->unit, true);
		sift_col_read(&r->unit, leaf, &sel, &v);
		sift_col_unit_lazy_dict(&r->unit, false);
		read_scatter(r, leaf, &v, idx, unit->nrow, vec);
	}
	pfree(idx);
}

/* the rows ascend in the numbering the executor sees; those of a compacted unit are the idx-th rows of the unit as stored */
void
sift_v1_idx_read_rows(void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec)
{
	SiftV1Idx  *r = impl;
	uint32	   *ords;
	SiftColVec	v;
	SiftSrcLeafBuf *b = &r->buf[leaf];

	sift_srcsel_leaf_buf(r->cxt, b, n);
	if (sift_v1_idx_wide(r, leaf))
	{
		wide_fill(r, leaf, rows, n, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, vec);
		return;
	}
	ords = sift_srcsel_ords(&r->sel, rows, n);
	sift_col_read_idx(&r->unit, leaf, ords, n, &v);
	sift_srcsel_fill(&r->sel, &r->lay->leaf[leaf], r->tol[leaf], &v, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, &r->dict[leaf], vec);
	pfree(ords);
}

const SiftV1ReaderOps sift_v1_idx_reader = {
	.layout = sift_v1_idx_layout,
	.stats = sift_v1_idx_stats,
	.open = idx_open,
	.close = sift_v1_idx_close,
	.source_layout = sift_v1_idx_source_layout,
	.nsb = sift_v1_idx_nsb,
	.select = sift_v1_idx_select,
	.begin = idx_begin,
	.next = sift_v1_idx_next,
	.live = sift_v1_idx_live,
	.vet_needed = sift_v1_idx_vet_needed,
	.vet = sift_v1_idx_vet,
	.read = sift_v1_idx_read,
	.tids = sift_v1_idx_tids
};
