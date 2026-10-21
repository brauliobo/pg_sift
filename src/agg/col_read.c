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

static const size_t vtype_size[] = {sizeof(int32), sizeof(int64), sizeof(double), sizeof(uint32)};
static const SiftColLane vtype_lane[] = {SIFT_COLL_I32, SIFT_COLL_I64, SIFT_COLL_F64, SIFT_COLL_CODE};

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

		for (int k = 0; k < Max(def->lanes, 1); k++)
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

/* the values of a read of the engine into the caller's storage, in the vector type of the leaf */
static void
col_fill(ColSource *src, int leaf, const SiftColVec *v, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];
	size_t		n = v->n;

	vec->ovf = NULL;
	if (v->ovf && !buf->ovf)
		sift_aggsel_overflow_error();
	switch (l->vtype)
	{
		case SCV_I32:
			for (size_t i = 0; i < n; i++)
				((int32 *) buf->data)[i] = (int32) v->i[i];
			break;
		case SCV_I64:
			memcpy(buf->data, v->i, n * sizeof(int64));
			break;
		case SCV_F64:
			memcpy(buf->data, v->f, n * sizeof(double));
			break;
		case SCV_CODE:
			memcpy(buf->data, v->codes, n * sizeof(uint32));
			src->dict[leaf] = (SiftSrcDict) {(const void *) (uintptr_t) v->dict->id, v->dict->n, v->dict->off, v->dict->bytes};
			break;
	}
	memset((char *) buf->data + n * vtype_size[l->vtype], 0, SIFT_AGG_PAD * vtype_size[l->vtype]);
	vec->data = buf->data;
	vec->dict = l->vtype == SCV_CODE ? &src->dict[leaf] : NULL;
	vec->n = n;
	vec->isnull = NULL;
	if (v->ovf && !src->tol[leaf])
	{
		memcpy(buf->ovf, v->ovf, SIFT_COL_NWORDS(n) * sizeof(uint64));
		vec->ovf = buf->ovf;
	}
	if (v->nulls)
	{
		for (size_t i = 0; i < n; i++)
			buf->isnull[i] = sift_colsrc_bit(v->nulls, i);
		vec->isnull = buf->isnull;
	}
	sift_colsrc_nan_values(l, buf->data, buf->isnull, vec);
}

/* A compacted unit of a match set (agg/sel.h): the kept rows only, which the engine reads by their positions, not by lane */
static void
col_read_selected(ColSource *src, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	SiftColVec	v;

	sift_aggsel_read(&src->sel, &src->unit, leaf, &v);
	col_fill(src, leaf, &v, buf, vec);
}

/* the leaf decoded by the engine into the caller's storage, in the lane of the leaf's vector type */
void
col_read_into(void *impl, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];
	SiftColLaneInfo info;

	if (l->wide)
	{
		wide_fill(src, leaf, NULL, 0, buf, vec);
		return;
	}
	if (src->sel.keep && src->sel.compact)
	{
		col_read_selected(src, leaf, buf, vec);
		return;
	}
	sift_col_read_lane(&src->unit, leaf, vtype_lane[l->vtype], buf->data, buf->isnull, &info);
	vec->ovf = NULL;
	info.ovf &= !src->tol[leaf];
	if (info.ovf && buf->ovf)
	{
		SiftColVec	v;

		sift_col_read(&src->unit, leaf, NULL, &v);
		memcpy(buf->ovf, v.ovf, SIFT_COL_NWORDS(info.n) * sizeof(uint64));
		vec->ovf = buf->ovf;
	}
	else if (info.ovf)
		sift_aggsel_overflow_error();
	memset((char *) buf->data + info.n * vtype_size[l->vtype], 0, SIFT_AGG_PAD * vtype_size[l->vtype]);
	vec->data = buf->data;
	vec->n = info.n;
	vec->isnull = info.nulls ? buf->isnull : NULL;
	vec->dict = NULL;
	sift_colsrc_nan_values(l, buf->data, buf->isnull, vec);
	if (l->vtype == SCV_CODE)
	{
		src->dict[leaf] = (SiftSrcDict) {(const void *) (uintptr_t) info.dict.id, info.dict.n, info.dict.off, info.dict.bytes};
		vec->dict = &src->dict[leaf];
	}
}

/* the leaf's buffers for n elements and SIFT_AGG_PAD more */
static ColLeafBuf *
leaf_buf(ColSource *src, int leaf, size_t n)
{
	ColLeafBuf *b = &src->buf[leaf];
	size_t		need = n + SIFT_AGG_PAD;

	if (b->cap < need)
	{
		if (b->data)
		{
			pfree(b->data);
			pfree(b->isnull);
			pfree(b->ovf);
		}
		b->cap = Max(need, 4096);
		b->data = MemoryContextAlloc(src->cxt, b->cap * 8);
		b->isnull = MemoryContextAlloc(src->cxt, b->cap);
		b->ovf = MemoryContextAlloc(src->cxt, SIFT_COL_NWORDS(b->cap) * sizeof(uint64));
	}
	return b;
}

/* read_into over the source's own buffers of the leaf: valid until the next read of the leaf or the next unit */
void
col_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	ColLeafBuf *b = leaf_buf(src, leaf, sift_col_lane_n(&src->unit, leaf));

	col_read_into(impl, unit, leaf, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, vec);
}

/* The values of a read of the rows idx[0..m) of a unit of nrow rows, at their rows in the leaf's buffer; the other rows are zero */
static void
col_scatter(ColSource *src, int leaf, const SiftColVec *v, const uint32 *idx, size_t nrow, SiftSrcVec *vec)
{
	const SiftSrcLeaf *l = &src->lay->leaf[leaf];
	size_t		m = v->n;
	ColLeafBuf *b = leaf_buf(src, leaf, nrow);

	memset(b->data, 0, (nrow + SIFT_AGG_PAD) * vtype_size[l->vtype]);
	switch (l->vtype)
	{
		case SCV_I32:
			for (size_t k = 0; k < m; k++)
				((int32 *) b->data)[idx[k]] = (int32) v->i[k];
			break;
		case SCV_I64:
			for (size_t k = 0; k < m; k++)
				((int64 *) b->data)[idx[k]] = v->i[k];
			break;
		case SCV_F64:
			for (size_t k = 0; k < m; k++)
				((double *) b->data)[idx[k]] = v->f[k];
			break;
		case SCV_CODE:
			for (size_t k = 0; k < m; k++)
				((uint32 *) b->data)[idx[k]] = v->codes[k];
			src->dict[leaf] = (SiftSrcDict) {(const void *) (uintptr_t) v->dict->id, v->dict->n, v->dict->off, v->dict->bytes};
			break;
	}
	vec->data = b->data;
	vec->dict = l->vtype == SCV_CODE ? &src->dict[leaf] : NULL;
	vec->n = nrow;
	vec->isnull = NULL;
	vec->ovf = NULL;
	if (v->ovf && !src->tol[leaf])
	{
		memset(b->ovf, 0, SIFT_COL_NWORDS(nrow) * sizeof(uint64));
		for (size_t k = 0; k < m; k++)
			if (sift_colsrc_bit(v->ovf, k))
				b->ovf[idx[k] >> 6] |= UINT64CONST(1) << (idx[k] & 63);
		vec->ovf = b->ovf;
	}
	if (v->nulls)
	{
		memset(b->isnull, 0, nrow);
		for (size_t k = 0; k < m; k++)
			b->isnull[idx[k]] = sift_colsrc_bit(v->nulls, k);
		vec->isnull = b->isnull;
	}
	sift_colsrc_nan_values(l, b->data, b->isnull, vec);
}

/* A selection of the unit's rows, of the bits of the executor */
static void
col_sel(ColSource *src, const SiftSrcUnit *unit, const uint64 *bits, SiftColSel *sel)
{
	sift_col_sel_alloc(src->cs, unit->nrow, sel);
	memcpy(sel->bits, bits, sel->nwords * sizeof(uint64));
	sift_col_sel_count(sel);
}

void
col_read_sel(void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	SiftColSel	sel;
	SiftColVec	v;
	uint32	   *idx;

	if ((src->sel.keep && src->sel.compact) || src->lay->leaf[leaf].wide)	/* the rows of a compacted unit are not the engine's: the whole leaf */
	{
		col_read(impl, unit, leaf, vec);
		return;
	}
	col_sel(src, unit, bits, &sel);
	idx = MemoryContextAlloc(src->cxt, (sel.count + SIFT_COL_IDXPAD) * sizeof(uint32));
	sift_col_sel_idx(&sel, idx);
	sift_col_read(&src->unit, leaf, &sel, &v);
	col_scatter(src, leaf, &v, idx, unit->nrow, vec);
	pfree(idx);
}

/* the rows ascend in the numbering the executor sees; those of a compacted unit are the idx-th rows of the unit as stored */
void
col_read_rows(void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec)
{
	ColSource  *src = impl;
	uint32	   *ords;
	SiftColVec	v;
	ColLeafBuf *b;

	if (src->lay->leaf[leaf].wide)
	{
		b = leaf_buf(src, leaf, n);
		wide_fill(src, leaf, rows, n, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, vec);
		return;
	}
	ords = palloc((n + SIFT_COL_IDXPAD) * sizeof(uint32));
	for (int i = 0; i < n; i++)
		ords[i] = src->sel.keep && src->sel.compact ? src->sel.idx[rows[i]] : rows[i];
	memset(ords + n, 0, SIFT_COL_IDXPAD * sizeof(uint32));
	sift_col_read_idx(&src->unit, leaf, ords, n, &v);
	b = leaf_buf(src, leaf, n);
	col_fill(src, leaf, &v, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, vec);
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
