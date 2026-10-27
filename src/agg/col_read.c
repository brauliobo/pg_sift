/*
 * agg/col_read.c - the reads of the column source: the engine decodes a unit into the vectors of the executor
 */
#include "postgres.h"

#include "agg/agg.h"

#if __has_include("col/col.h")

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/kern.h"
#include "agg/pre.h"
#include "agg/sel.h"
#include "agg/wide.h"
#include "core/popcount.h"
#include "col/col.h"
#include "col/gpu.h"
#include "col/pagg.h"
#include "col/am.h"
#include "col/enc.h"
#include "col/pg.h"
#include "agg/colsrc.h"
#include "col/shred_pg.h"

#include "agg/col_int.h"

/* of the unit as stored: a compacted unit of a match set yields fewer */
int64
col_leaf_n(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_col_lane_n(&((ColSource *) impl)->unit, leaf);
}

/*
 * A wide leaf of the unit as a dictionary (agg/wide.h), made from its lanes by the first read of the leaf in the unit (the rows the executor
 * sees: those of a unit restricted to a match set are the kept ones) and shared by the reads after it, so that the executor's maps of the
 * dictionary are made once. A dictionary has an id of its own: the executor keeps what it derives by the id.
 */
static const SiftAggWide *
wide_unit(ColSource *src, int leaf)
{
	SiftAggWide *w = &src->wide[leaf];

	if (src->wgen[leaf] != src->ugen)
	{
		const SiftColDef *def = sift_col_def(src->cs, leaf);
		SiftColVec	v[SIFT_COL_MAX_LANES];

		for (int k = 0; k < sift_col_def_value_lanes(def); k++)
			sift_aggsel_read(&src->sel, &src->unit, leaf + k, &v[k]);
		w->cxt = src->cxt;
		sift_aggwide_build(w, def, v, v[0].n);
		src->wgen[leaf] = src->ugen;
		src->dict[leaf] = (SiftSrcDict) {(const void *) (uintptr_t) (UINT64CONST(0x7000000000000000) + ++src->wseq), w->nd, w->off, w->bytes};
	}
	return w;
}

/* the codes of the rows `rows` (n of them; NULL: all the unit's) of a wide leaf, in the caller's storage */
static void
wide_fill(ColSource *src, int leaf, const uint32 *rows, int n, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	const SiftAggWide *w = wide_unit(src, leaf);
	uint32	   *codes = buf->data;
	int			m = rows ? n : (int) w->n;

	for (int i = 0; i < m; i++)
	{
		uint32		r = rows ? rows[i] : (uint32) i;

		codes[i] = w->codes[r];
		buf->isnull[i] = w->isnull[r];
	}
	memset(codes + m, 0, SIFT_AGG_PAD * sizeof(uint32));
	*vec = (SiftSrcVec) {.data = codes, .isnull = w->has_null ? buf->isnull : NULL, .dict = &src->dict[leaf], .n = m};
}

/* A compacted unit of a match set (agg/sel.h): the kept rows only, which the engine reads by their positions, not by lane */
static void
col_read_selected(ColSource *src, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	SiftColVec	v;

	sift_col_unit_lazy_dict(&src->unit, true);
	sift_aggsel_read(&src->sel, &src->unit, leaf, &v);
	sift_col_unit_lazy_dict(&src->unit, false);
	sift_aggsel_fill(&src->sel, &src->lay->leaf[leaf], src->tol[leaf], &v, buf, &src->dict[leaf], vec);
}

/* the leaf decoded by the engine into the caller's storage, in the lane of the leaf's vector type */
void
col_read_into(void *impl, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];

	if (col_wide(src, leaf))
	{
		wide_fill(src, leaf, NULL, 0, buf, vec);
		return;
	}
	if (src->sel.compact)
	{
		col_read_selected(src, leaf, buf, vec);
		return;
	}
	sift_aggsel_read_lane(&src->sel, &src->unit, leaf, l, src->tol[leaf], buf, &src->dict[leaf], vec);
}

/* read_into over the source's own buffers of the leaf: valid until the next read of the leaf or the next unit */
void
col_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	SiftSrcLeafBuf *b = &src->buf[leaf];

	sift_aggsel_leaf_buf(src->cxt, b, sift_col_lane_n(&src->unit, leaf));

	col_read_into(impl, unit, leaf, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, vec);
}

/* The values of a read of the rows idx[0..m) of a unit of nrow rows, at their rows in the leaf's buffer; the other rows are zero */
static void
col_scatter(ColSource *src, int leaf, const SiftColVec *v, const uint32 *idx, size_t nrow, SiftSrcVec *vec)
{
	SiftSrcLeafBuf *b = &src->buf[leaf];

	sift_aggsel_leaf_buf(src->cxt, b, nrow);
	sift_aggsel_scatter(&src->lay->leaf[leaf], src->tol[leaf], v, idx, nrow, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, &src->dict[leaf], vec);
}

/*
 * The dictionary of a wide leaf for the rows of sel only, with their codes at their rows in the leaf's buffer (the other rows are zero): a
 * scan that keeps a few rows of a unit converts the values of those, not one per distinct value of the unit. It is not the dictionary of the
 * unit, so a whole read of the leaf in the same unit makes that one again.
 */
static void
wide_scatter(ColSource *src, int leaf, const SiftColSel *sel, const uint32 *idx, size_t nrow, SiftSrcVec *vec)
{
	SiftAggWide *w = &src->wide[leaf];
	const SiftColDef *def = sift_col_def(src->cs, leaf);
	SiftColVec	v[SIFT_COL_MAX_LANES];
	SiftSrcLeafBuf *b = &src->buf[leaf];
	uint32	   *codes;

	sift_aggsel_leaf_buf(src->cxt, b, nrow);
	codes = b->data;
	for (int k = 0; k < sift_col_def_value_lanes(def); k++)
		sift_col_read(&src->unit, leaf + k, sel, &v[k]);
	w->cxt = src->cxt;
	sift_aggwide_build(w, def, v, sel->count);
	src->wgen[leaf] = 0;
	src->dict[leaf] = (SiftSrcDict) {(const void *) (uintptr_t) (UINT64CONST(0x7000000000000000) + ++src->wseq), w->nd, w->off, w->bytes};
	memset(codes, 0, (nrow + SIFT_AGG_PAD) * sizeof(uint32));
	if (w->has_null)
		memset(b->isnull, 0, nrow);
	for (uint32 k = 0; k < sel->count; k++)
	{
		codes[idx[k]] = w->codes[k];
		if (w->has_null)
			b->isnull[idx[k]] = w->isnull[k];
	}
	*vec = (SiftSrcVec) {.data = codes, .isnull = w->has_null ? b->isnull : NULL, .dict = &src->dict[leaf], .n = nrow};
}

void
col_read_sel(void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	SiftColSel	sel;
	SiftColVec	v;
	uint32	   *idx;

	if (src->sel.compact)	/* the rows of a compacted unit are not the engine's: the whole leaf */
	{
		col_read(impl, unit, leaf, vec);
		return;
	}
	sift_aggsel_bits(src->cs, unit->nrow, bits, &sel);
	idx = MemoryContextAlloc(src->cxt, (sel.count + SIFT_COL_IDXPAD) * sizeof(uint32));
	sift_col_sel_idx(&sel, idx);
	if (col_wide(src, leaf))
		wide_scatter(src, leaf, &sel, idx, unit->nrow, vec);
	else
	{
		sift_col_unit_lazy_dict(&src->unit, true);
		sift_col_read(&src->unit, leaf, &sel, &v);
		sift_col_unit_lazy_dict(&src->unit, false);
		col_scatter(src, leaf, &v, idx, unit->nrow, vec);
	}
	pfree(idx);
}

/* the rows ascend in the numbering the executor sees; those of a compacted unit are the idx-th rows of the unit as stored */
void
col_read_rows(void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	uint32	   *ords;
	SiftColVec	v;
	SiftSrcLeafBuf *b = &src->buf[leaf];

	sift_aggsel_leaf_buf(src->cxt, b, n);
	if (col_wide(src, leaf))
	{
		wide_fill(src, leaf, rows, n, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, vec);
		return;
	}
	ords = sift_aggsel_ords(&src->sel, rows, n);
	sift_col_read_idx(&src->unit, leaf, ords, n, &v);
	sift_aggsel_fill(&src->sel, &src->lay->leaf[leaf], src->tol[leaf], &v, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, &src->dict[leaf], vec);
	pfree(ords);
}

void
col_live(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	ColSource  *src = impl;

	sift_aggsel_live(&src->sel, &src->unit, bits);
	if (src->rck && !src->rck->active)
		recheck_collect(src, unit->nrow, bits);
}

void
col_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	ColSource  *src = impl;

	sift_aggsel_vet(&src->sel, src->cs, &src->unit, bits);
}

bool
col_vet_needed(void *impl, const SiftSrcUnit *unit)
{
	ColSource  *src = impl;

	return sift_aggsel_vet_needed(&src->unit);
}

#endif							/* __has_include("col/col.h") */
