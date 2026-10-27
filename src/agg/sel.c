/*
 * agg/sel.c - the rows of a column unit that a match set keeps (agg/sel.h).
 */
#include "postgres.h"

#include "catalog/pg_type.h"

#include "agg/kern.h"
#include "agg/sel.h"
#include "core/popcount.h"
#include "agg/colsrc.h"
#include "col/wbuf.h"

/* Few kept rows are decoded one by one; many are decoded with the whole chunk, and the mask keeps the others out */
#define SEL_SPARSE	8

void
sift_aggsel_init(SiftAggSel *a, MemoryContext mcxt, Oid heapoid)
{
	memset(a, 0, sizeof(*a));
	a->mcxt = mcxt;
	a->heapoid = heapoid;
}

/* idx: the stored ordinal of each kept row of a compacted unit */
static void
compact_idx(SiftAggSel *a)
{
	if (a->idxcap < a->sel.count + SIFT_COL_IDXPAD)
	{
		if (a->idx)
			pfree(a->idx);
		a->idxcap = Max(a->sel.count + SIFT_COL_IDXPAD, 1024);
		a->idx = MemoryContextAlloc(a->mcxt, a->idxcap * sizeof(uint32));
	}
	sift_col_sel_idx(&a->sel, a->idx);
}

bool
sift_aggsel_unit(SiftAggSel *a, const SiftColUnit *cu, uint32 *nrow)
{
	sift_col_sel_all(cu, &a->sel);
	a->nrow = a->sel.n;
	if (!sift_col_filter_tids(cu, a->keep, &a->sel))
	{
		a->units_skipped++;
		return false;
	}
	a->compact = (uint64) a->sel.count * SEL_SPARSE <= a->nrow;
	a->units_compact += a->compact;
	*nrow = a->compact ? a->sel.count : a->nrow;
	if (a->compact)
		compact_idx(a);
	return true;
}

void
sift_aggsel_narrow(SiftAggSel *a, SiftColSource *src, const SiftColUnit *cu, const uint64 *bits, uint32 *nrow)
{
	SiftColSel	kept;

	Assert(!a->compact);
	sift_col_sel_alloc(src, a->keep ? a->nrow : cu->nrows, &kept);
	memcpy(kept.bits, bits, kept.nwords * sizeof(uint64));
	sift_col_sel_count(&kept);
	a->nrow = kept.n;
	a->sel = kept;
	a->compact = true;
	compact_idx(a);
	*nrow = kept.count;
}

void
sift_aggsel_read(SiftAggSel *a, const SiftColUnit *cu, int col, SiftColVec *vec)
{
	if (a->compact)
		sift_col_read_idx(cu, col, a->idx, a->sel.count, vec);
	else
		sift_col_read(cu, col, NULL, vec);
}

void
sift_aggsel_live(SiftAggSel *a, const SiftColUnit *cu, uint64 *bits)
{
	SiftColSel	all;

	if (a->compact)
	{
		memset(bits, 0, ((a->sel.count + 63) / 64) * sizeof(uint64));
		for (uint32 i = 0; i < a->sel.count; i++)
			bits[i >> 6] |= UINT64CONST(1) << (i & 63);
		return;
	}
	if (a->keep)
	{
		memcpy(bits, a->sel.bits, a->sel.nwords * sizeof(uint64));
		return;
	}
	sift_col_sel_all(cu, &all);
	sift_col_point_narrow(cu, &all);
	memcpy(bits, all.bits, all.nwords * sizeof(uint64));
}

/* Runs op on the unit as stored: a compacted unit's rows are expanded to their stored ordinals, op restricts them, and the survivors are compressed back into bits */
void
sift_aggsel_stored(SiftAggSel *a, SiftColSource *src, const SiftColUnit *cu, uint64 *bits, SiftAggSelOp op, void *arg)
{
	SiftColSel	vs;
	const uint32 *idx = a->idx;

	if (a->compact)
	{
		sift_col_sel_alloc(src, a->nrow, &vs);
		for (uint32 i = 0; i < a->sel.count; i++)
			if (bits[i >> 6] >> (i & 63) & 1)
				vs.bits[idx[i] >> 6] |= UINT64CONST(1) << (idx[i] & 63);
		sift_col_sel_count(&vs);
		op(cu, &vs, arg);
		for (uint32 i = 0; i < a->sel.count; i++)
			if (!(vs.bits[idx[i] >> 6] >> (idx[i] & 63) & 1))
				bits[i >> 6] &= ~(UINT64CONST(1) << (i & 63));
		return;
	}
	sift_col_sel_alloc(src, a->keep ? a->nrow : cu->nrows, &vs);
	memcpy(vs.bits, bits, vs.nwords * sizeof(uint64));
	sift_col_sel_count(&vs);
	op(cu, &vs, arg);
	memcpy(bits, vs.bits, vs.nwords * sizeof(uint64));
}

static void
vet_op(const SiftColUnit *cu, SiftColSel *vs, void *arg)
{
	sift_col_vet(cu, vs);
}

bool
sift_aggsel_vet_needed(const SiftColUnit *cu)
{
	return !sift_col_unit_allvisible(cu);
}

void
sift_aggsel_vet(SiftAggSel *a, SiftColSource *src, const SiftColUnit *cu, uint64 *bits)
{
	if (sift_aggsel_vet_needed(cu))
		sift_aggsel_stored(a, src, cu, bits, vet_op, NULL);
}

/* The dictionary of a read that stayed in the segment, loaded whole */
static void
dict_full(const void *gctx, SiftSrcDict *self)
{
	SiftColDict d = {.id = (uint64) (uintptr_t) self->id};

	sift_col_dict_full(gctx, &d);
	*self = (SiftSrcDict) {self->id, d.n, d.off, d.bytes, d.order, d.octx};
}

SiftSrcDict
sift_aggsel_src_dict(const SiftColDict *d)
{
	if (d->off == NULL)
		return (SiftSrcDict) {(const void *) (uintptr_t) d->id, d->n, .get = d->get, .full = dict_full, .gctx = d->gctx};
	return (SiftSrcDict) {(const void *) (uintptr_t) d->id, d->n, d->off, d->bytes, d->order, d->octx};
}

static const SiftColLane vtype_lane[] = {SIFT_COLL_I32, SIFT_COLL_I64, SIFT_COLL_F64, SIFT_COLL_CODE};

void
sift_aggsel_leaf_buf(MemoryContext cxt, SiftSrcLeafBuf *b, size_t n)
{
	size_t		need = n + SIFT_AGG_PAD;

	if (b->cap >= need)
		return;
	if (b->data)
	{
		pfree(b->data);
		pfree(b->isnull);
		pfree(b->ovf);
	}
	b->cap = Max(need, 4096);
	b->data = MemoryContextAlloc(cxt, b->cap * 8);
	b->isnull = MemoryContextAlloc(cxt, b->cap);
	b->ovf = MemoryContextAlloc(cxt, SIFT_COL_NWORDS(b->cap) * sizeof(uint64));
}

void
sift_aggsel_fill(SiftAggSel *a, const SiftSrcLeaf *l, bool tol, const SiftColVec *v, const SiftSrcBuf *buf, SiftSrcDict *dict, SiftSrcVec *vec)
{
	size_t		n = v->n;

	vec->ovf = NULL;
	vec->eoff = NULL;
	if (v->ovf && !buf->ovf)
		sift_aggsel_overflow_error(a->heapoid);
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
			*dict = sift_aggsel_src_dict(v->dict);
			break;
	}
	memset((char *) buf->data + n * sift_src_vtype_size[l->vtype], 0, SIFT_AGG_PAD * sift_src_vtype_size[l->vtype]);
	vec->data = buf->data;
	vec->dict = l->vtype == SCV_CODE ? dict : NULL;
	vec->n = n;
	vec->isnull = NULL;
	if (v->ovf && !tol)
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

uint32 *
sift_aggsel_ords(const SiftAggSel *a, const uint32 *rows, int n)
{
	uint32	   *ords = palloc((Max(n, 1) + SIFT_COL_IDXPAD) * sizeof(uint32));

	for (int i = 0; i < n; i++)
		ords[i] = a->compact ? a->idx[rows[i]] : rows[i];
	memset(ords + n, 0, SIFT_COL_IDXPAD * sizeof(uint32));
	return ords;
}

void
sift_aggsel_bits(SiftColSource *src, uint32 nrow, const uint64 *bits, SiftColSel *sel)
{
	sift_col_sel_alloc(src, nrow, sel);
	memcpy(sel->bits, bits, sel->nwords * sizeof(uint64));
	sift_col_sel_count(sel);
}

void
sift_aggsel_read_lane(SiftAggSel *a, const SiftColUnit *cu, int col, const SiftSrcLeaf *l, bool tol, const SiftSrcBuf *buf, SiftSrcDict *dict, SiftSrcVec *vec)
{
	SiftColLaneInfo info;

	sift_col_read_lane(cu, col, vtype_lane[l->vtype], buf->data, buf->isnull, &info);
	vec->ovf = NULL;
	info.ovf &= !tol;
	if (info.ovf && buf->ovf)
	{
		SiftColVec	v;

		sift_col_read(cu, col, NULL, &v);
		memcpy(buf->ovf, v.ovf, SIFT_COL_NWORDS(info.n) * sizeof(uint64));
		vec->ovf = buf->ovf;
	}
	else if (info.ovf)
		sift_aggsel_overflow_error(a->heapoid);
	memset((char *) buf->data + info.n * sift_src_vtype_size[l->vtype], 0, SIFT_AGG_PAD * sift_src_vtype_size[l->vtype]);
	vec->data = buf->data;
	vec->n = info.n;
	vec->isnull = info.nulls ? buf->isnull : NULL;
	vec->dict = NULL;
	vec->eoff = a->compact ? NULL : sift_col_group_offsets(cu, col);
	sift_colsrc_nan_values(l, buf->data, buf->isnull, vec);
	if (l->vtype == SCV_CODE)
	{
		*dict = sift_aggsel_src_dict(&info.dict);
		vec->dict = dict;
	}
}

void
sift_aggsel_scatter(const SiftSrcLeaf *l, bool tol, const SiftColVec *v, const uint32 *idx, size_t nrow, const SiftSrcBuf *b, SiftSrcDict *dict, SiftSrcVec *vec)
{
	size_t		m = v->n;

	memset(b->data, 0, (nrow + SIFT_AGG_PAD) * sift_src_vtype_size[l->vtype]);
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
			*dict = sift_aggsel_src_dict(v->dict);
			break;
	}
	vec->data = b->data;
	vec->dict = l->vtype == SCV_CODE ? dict : NULL;
	vec->n = nrow;
	vec->isnull = NULL;
	vec->ovf = NULL;
	vec->eoff = NULL;
	if (v->ovf && !tol)
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

void
sift_aggsel_overflow_error(Oid heapoid)
{
	sift_cwbuf_invalidate_plans(heapoid);
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sift: a column of the aggregate scan holds values its index cannot store"),
			 errhint("The plan was made before the value was written: run the statement again, it is planned afresh. Or set sift.enable_agg to off for this query.")));
}

/* ---- the JSON type of the values of a leaf: the tags of the overflow values of the chunks ---- */

StaticAssertDecl(SIFT_COLTAG_STRING == 1 && SIFT_COLTAG_JNULL == 7, "SIFT_SRC_JT_* are the chunk tags shifted by one");

/* the tag of the values a leaf of this type stores */
static uint8
stored_tag(Oid typid)
{
	return typid == BOOLOID ? SIFT_COLTAG_BOOLEAN : typid == TEXTOID ? SIFT_COLTAG_STRING : SIFT_COLTAG_NUMBER;
}

void
sift_aggsel_types_unknown_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sift: a column of the aggregate scan holds values whose JSON type its index did not store"),
			 errhint("REINDEX the index to store them, or set sift.enable_agg to off for this query.")));
}

typedef struct TypesTest
{
	SiftColSource *cs;
	Oid			heapoid;
	int			col;
	Oid			typid;
	bool		astext;
	uint32		mask;			/* bit t: the type of tag t */
	bool		read;			/* the query reads the leaf elsewhere */
} TypesTest;

/* the rows of the unit whose value has one of the types; a row whose value of those types the leaf cannot hold is an error if a snapshot sees it and the query reads the leaf */
static void
types_op(const SiftColUnit *cu, SiftColSel *vs, void *arg)
{
	TypesTest  *t = arg;
	bool		known,
				left;
	uint64	   *ovf;
	SiftColSel	bad;

	left = sift_col_filter_types(cu, t->col, t->mask, stored_tag(t->typid), t->astext, vs, &known);
	if (!known)
		sift_aggsel_types_unknown_error();
	ovf = palloc(SIFT_COL_NWORDS(cu->nrows) * sizeof(uint64));
	if (!left || !t->read || !sift_col_ovf_bits(cu, t->col, ovf))
		return;
	sift_col_sel_alloc(t->cs, cu->nrows, &bad);
	for (uint32 w = 0; w < bad.nwords; w++)
		bad.bits[w] = vs->bits[w] & ovf[w];
	if (sift_col_sel_count(&bad) > 0 && sift_col_vet(cu, &bad))
		sift_aggsel_overflow_error(t->heapoid);
}

void
sift_aggsel_filter_types(SiftAggSel *a, SiftColSource *cs, const SiftColUnit *cu, int col, Oid typid, bool astext, uint32 types, uint64 *bits)
{
	TypesTest	t = {cs, a->heapoid, col, typid, astext, (types & SIFT_SRC_JT_ALL) << 1, (types & SIFT_SRC_JT_READ) != 0};

	sift_aggsel_stored(a, cs, cu, bits, types_op, &t);
}

/* a chunk tag as read_types gives it: the index of the name jsonb_typeof gives, or none */
static uint8
src_type(uint8 tag)
{
	return tag == SIFT_COLTAG_NONE ? SIFT_SRC_TYPE_NONE : tag == SIFT_COLTAG_JNULL ? 5 : tag - 1;
}

void
sift_aggsel_read_types(SiftAggSel *a, const SiftColUnit *cu, int col, Oid typid, bool astext, uint32 nrow, uint8 *tags)
{
	bool		compact = a->compact;
	uint8	   *all = compact ? palloc(a->nrow) : tags;

	if (!sift_col_row_types(cu, col, stored_tag(typid), astext, all))
		sift_aggsel_types_unknown_error();
	for (uint32 i = 0; i < nrow; i++)
		tags[i] = src_type(all[compact ? a->idx[i] : i]);
}

bool
sift_aggsel_rows_left(const uint64 *bits, uint32 nrow)
{
	uint64		left = 0;

	for (uint32 w = 0; w < (nrow + 63) / 64; w++)
		left += sift_pc64(bits[w]);
	return left > 0;
}
