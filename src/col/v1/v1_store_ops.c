/*
 * col/v1/v1_store_ops.c - Format 1 behind the storage seam of the unified engine (core/store_ops.h): its sift_col indexes and sift_table tables are read, never written, as the batches of
 * SiftAggV6 (agg/batch_source.h) through the unit reader (col/v1/v1_reader.h) that decodes their units. The rows of a unit that the tombstones and the snapshot leave go to batches of
 * SIFT_VEC_LEN rows, which may span units; the quals of the operator run over each batch. Format 1 is sorted by nothing and has no accelerators to hand over.
 */
/* check-memory: allow-allocs every array here follows the columns of one scan or the rows of one batch, not the rows of the relation */
#include "postgres.h"

#include "access/tableam.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "port/pg_bitutils.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/batch_source.h"
#include "col/am.h"
#include "col/v1/v1_reader.h"
#include "col/v1/v1_store_ops.h"
#include "core/compat.h"
#include "core/memtrack.h"
#include "core/store_ops.h"
#include "table/v1/v1_tbl_ops.h"

/* ---- the leaves a scan decodes as they are stored ---- */

/* the leaf of attribute att when it is the only one (the lanes of a wide value, which have no type, are its head's): a scalar of the attribute itself, none for a document, a declared path or a list */
static const SiftSrcLeaf *
att_leaf(const SiftSrcLayout *lay, int att)
{
	const SiftSrcLeaf *found = NULL;

	for (int i = 0; i < lay->nleaves; i++)
	{
		const SiftSrcLeaf *l = &lay->leaf[i];

		if (l->attno != att || l->typid == InvalidOid)
			continue;
		if (found != NULL || l->parent >= 0 || l->kind != SCL_SCALAR || l->path != NIL || l->arrhead || l->arrlen != 0 || l->lossy || l->astext)
			return NULL;
		found = l;
	}
	return found;
}

static Oid
att_typid(const SiftSrcLayout *lay, int att)
{
	const Node *e = att >= 1 && att <= list_length(lay->attexprs) ? list_nth(lay->attexprs, att - 1) : NULL;

	return e == NULL ? InvalidOid : exprType(e);
}

/* a type that is its integer or float as stored: a Datum is made of it without the type's input or a dictionary */
static bool
datum_by_value(const SiftSrcLeaf *l, Oid typid)
{
	if (typid == FLOAT4OID || typid == FLOAT8OID)
		return l->vtype == SCV_F64;
	return (typid == BOOLOID || typid == INT2OID || typid == INT4OID || typid == INT8OID || typid == DATEOID || typid == TIMEOID || typid == TIMESTAMPOID || typid == TIMESTAMPTZOID ||
			typid == CASHOID) && (l->vtype == SCV_I32 || l->vtype == SCV_I64);
}

static bool
leaf_serves(const SiftSrcLeaf *l, Oid typid, SiftV6Class cls)
{
	if (l->wide)
		return cls == SV6C_DATUM;
	switch (cls)
	{
		case SV6C_I32:
		case SV6C_I64:
		case SV6C_BOOL:
			return l->vtype == SCV_I32 || l->vtype == SCV_I64;
		case SV6C_F64:
			return l->vtype == SCV_F64;
		case SV6C_TEXT:
			return l->vtype == SCV_CODE;
		case SV6C_DATUM:
			return datum_by_value(l, typid);
		default:
			return false;
	}
}

static const SiftV6Class vec_class[] = {[SVEC_BOOL] = SV6C_BOOL,[SVEC_I32] = SV6C_I32,[SVEC_I64] = SV6C_I64,[SVEC_NUM64] = SV6C_I64,[SVEC_F64] = SV6C_F64,
	[SVEC_STRVIEW] = SV6C_TEXT,[SVEC_DATUM] = SV6C_DATUM};

/* ---- what the format tells the planner ---- */

static const SiftV1ReaderOps *
reader_of(bool is_table)
{
	return is_table ? &sift_v1_tbl_reader : &sift_v1_idx_reader;
}

static bool
v1_owns_rel(Oid relid, bool is_table)
{
	if (is_table)
		return get_rel_relkind(relid) == RELKIND_RELATION && sift_v1_tbl_owns(relid);
	return OidIsValid(sift_col_am_oid()) && get_rel_relam(relid) == sift_col_am_oid() &&
		sift_col_index_oid_engine(relid) == SIFT_FORMAT_MAJOR && !sift_col_index_stale(relid);
}

static SiftStoreAtt
describe_att(const SiftSrcLayout *lay, int att)
{
	const SiftSrcLeaf *l = att_leaf(lay, att);
	const Node *e = att >= 1 && att <= list_length(lay->attexprs) ? list_nth(lay->attexprs, att - 1) : NULL;
	SiftStoreAtt a = {.attno = att,.typid = att_typid(lay, att),.typmod = e == NULL ? -1 : exprTypmod(e),.vec_type = SVEC_DATUM};

	a.is_jsonb = a.typid == JSONBOID;
	if (l == NULL || l->wide)
	{
		a.is_wide = l != NULL;
		return a;
	}
	switch (l->vtype)
	{
		case SCV_I32:
			a.vec_type = a.typid == BOOLOID ? SVEC_BOOL : SVEC_I32;
			break;
		case SCV_I64:
			a.vec_type = a.typid == NUMERICOID ? SVEC_NUM64 : SVEC_I64;
			a.scale = l->scale;
			a.prec = a.typid == NUMERICOID ? 18 : 0;
			break;
		case SCV_F64:
			a.vec_type = SVEC_F64;
			break;
		case SCV_CODE:
			a.vec_type = SVEC_STRVIEW;
			break;
	}
	return a;
}

static bool
v1_schema(Oid relid, bool is_table, SiftStoreSchema *out)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	const SiftSrcLayout *lay = reader_of(is_table)->layout(r.rel);

	*out = (SiftStoreSchema) {.natts = lay->natts,.is_table = is_table,.atts = palloc0(lay->natts * sizeof(SiftStoreAtt))};
	for (int a = 1; a <= lay->natts; a++)
	{
		out->atts[a - 1] = describe_att(lay, a);
		out->has_jsonb |= out->atts[a - 1].is_jsonb;
	}
	sift_v1_rel_close(&r);
	return true;
}

static bool
v1_stats(Oid relid, bool is_table, SiftStoreStats *out)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	SiftSrcStats st;

	reader_of(is_table)->stats(r.rel, &st);
	*out = (SiftStoreStats) {.live_rows = r.heap->rd_rel->reltuples >= 0 ? r.heap->rd_rel->reltuples : st.nrows,
		.stored_bytes = is_table ? table_relation_size(r.heap, MAIN_FORKNUM) : (uint64) st.bytes,.allvis_frac = Min(Max(st.allvisible_frac, 0.0), 1.0)};
	sift_v1_rel_close(&r);
	return true;
}

static bool
v1_leaf_holds(Oid relid, bool is_table, int att, int cls)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	const SiftSrcLayout *lay = reader_of(is_table)->layout(r.rel);
	const SiftSrcLeaf *l = att_leaf(lay, att);
	const bool	holds = l != NULL && leaf_serves(l, att_typid(lay, att), (SiftV6Class) cls);

	sift_v1_rel_close(&r);
	return holds;
}

/* ---- the batches ---- */

typedef struct V1Src
{
	SiftV6Source base;
	MemoryContext cx;
	MemoryContext unit_cx;		/* what one unit needs: the bits of its rows, reset when the next one starts */
	MemoryContext batch_cx;		/* the bytes of the long strings and the datums of one batch, reset when the next one starts */
	SiftV1Rel	rels;
	Snapshot	snap;
	const SiftV1ReaderOps *rd;
	void	   *src;
	SiftV6Input in;
	const SiftSrcLayout *lay;
	const SiftSrcLeaf *col_leaf[SIFT_V6_MAX_COLS];
	int			col_read[SIFT_V6_MAX_COLS];		/* of each column and boolean of the input, the entry of read_vec that holds its leaf */
	const SiftSrcLeaf *mask_leaf[SIFT_V6_MAX_MASKS];
	int			mask_read[SIFT_V6_MAX_MASKS];
	int			nread;
	int			read_leaf[SIFT_V6_MAX_COLS + SIFT_V6_MAX_MASKS];	/* the distinct leaves of a unit that are read: one read of a leaf is valid until the next of the same leaf */
	SiftSrcVec	read_vec[SIFT_V6_MAX_COLS + SIFT_V6_MAX_MASKS];
	SiftSrcUnit unit;
	bool		in_unit;
	uint64	   *bits;			/* the rows of the unit the snapshot sees and nothing has been taken of */
	int			pos;
	SiftV6Vexpr *quals;
	SiftVexprCtx *vctx;
	TupleTableSlot *fslot;
	IndexFetchTableData *index_fetch;	/* an index entry names the root of a chain of versions of the heap row; a sift_table row is its own */
	SiftVec		vec[SIFT_V6_MAX_COLS];
	void	   *data[SIFT_V6_MAX_COLS];
	uint64	   *valid[SIFT_V6_MAX_COLS];
	uint64	   *nan[SIFT_V6_MAX_COLS];
	bool		has_nan[SIFT_V6_MAX_COLS];
	int16		typlen[SIFT_V6_MAX_COLS];
	bool		typbyval[SIFT_V6_MAX_COLS];
	SiftVexprMaskIn masks[SIFT_V6_MAX_MASKS];
	uint64	   *mbits[SIFT_V6_MAX_MASKS];
	uint64	   *mvalid[SIFT_V6_MAX_MASKS];
	uint64		mask[SIFT_VEC_WORDS];
	uint64		tag[SIFT_VEC_LEN];
	uint32		ords[SIFT_VEC_LEN];
	ItemPointerData tids[SIFT_VEC_LEN];
} V1Src;

static bool
unit_next(V1Src *s)
{
	for (;;)
	{
		int			nw;

		MemoryContextReset(s->unit_cx);
		if (!s->rd->next(s->src, &s->unit))
			return false;
		nw = (s->unit.nrow + 63) / 64;
		s->bits = MemoryContextAllocZero(s->unit_cx, SIFT_COL_NWORDS(s->unit.nrow) * sizeof(uint64));
		s->rd->live(s->src, &s->unit, s->bits);
		if (s->rd->vet_needed(s->src, &s->unit))
			s->rd->vet(s->src, &s->unit, s->bits);
		for (int w = 0; w < nw; w++)
			if (s->bits[w] != 0)
			{
				for (int k = 0; k < s->nread; k++)
					s->rd->read(s->src, &s->unit, s->read_leaf[k], &s->read_vec[k]);
				s->pos = 0;
				return true;
			}
	}
}

static int
unit_take(V1Src *s, int cap)
{
	const int	nw = (s->unit.nrow + 63) / 64;
	int			k = 0;

	for (int w = s->pos >> 6; w < nw && k < cap; w++)
	{
		uint64		word = s->bits[w];

		if (w == s->pos >> 6)
			word &= ~UINT64CONST(0) << (s->pos & 63);
		for (; word != 0 && k < cap; word &= word - 1)
			s->ords[k++] = w * 64 + pg_rightmost_one_pos64(word);
		if (k == cap)
			break;
	}
	s->pos = k == cap ? (int) s->ords[k - 1] + 1 : s->unit.nrow;
	return k;
}

static void
unreadable(const V1Src *s, const SiftSrcLeaf *l)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("column \"%s\" holds a value the stored form cannot hold", get_attname(s->in.indexoid, l->attno, false)),
					errhint("Run the statement with sift.enable_agg = off.")));
}

static void
put_strview(V1Src *s, SiftStrView *sv, const SiftSrcVec *v, uint32 r, bool valid)
{
	char		buf[SIFT_SRC_DICT_ENTRY];
	uint32		len;
	const char *p = valid ? sift_srcdict_str(v->dict, ((const uint32 *) v->data)[r], &len, buf) : NULL;

	memset(sv, 0, sizeof(*sv));
	if (!valid)
		return;
	sv->len = len;
	memcpy(sv->prefix, p, Min(len, 4));
	if (len > 12)
		sv->u.ptr = memcpy(MemoryContextAlloc(s->batch_cx, len), p, len);
	else if (len > 4)
		memcpy(sv->u.tail, p + 4, len - 4);
}

static Datum
datum_of(V1Src *s, int i, const SiftSrcVec *v, uint32 r)
{
	const SiftSrcLeaf *l = s->col_leaf[i];

	if (l->wide)
	{
		char		buf[SIFT_SRC_DICT_ENTRY];
		uint32		len;
		const char *p = sift_srcdict_str(v->dict, ((const uint32 *) v->data)[r], &len, buf);

		return PointerGetDatum(memcpy(MemoryContextAlloc(s->batch_cx, len), p, len));
	}
	switch (s->in.col[i].typid)
	{
		case BOOLOID:
			return BoolGetDatum(sift_src_int(v->data, l->vtype, r) != 0);
		case INT2OID:
			return Int16GetDatum((int16) sift_src_int(v->data, l->vtype, r));
		case INT4OID:
		case DATEOID:
			return Int32GetDatum((int32) sift_src_int(v->data, l->vtype, r));
		case FLOAT4OID:
			return Float4GetDatum((float4) ((const double *) v->data)[r]);
		case FLOAT8OID:
			return Float8GetDatum(((const double *) v->data)[r]);
		default:
			return Int64GetDatum(sift_src_int(v->data, l->vtype, r));
	}
}

static void
put_col(V1Src *s, int i, uint32 r, int n)
{
	const SiftSrcVec *v = &s->read_vec[s->col_read[i]];
	const SiftSrcLeaf *l = s->col_leaf[i];
	const bool	nan = v->ovf != NULL && sift_src_bit(v->ovf, r);
	const bool	valid = (v->isnull == NULL || !v->isnull[r]) && !nan;

	if (nan && !sift_src_nan_leaf(l))
		unreadable(s, l);
	s->valid[i][n >> 6] |= (uint64) valid << (n & 63);
	s->nan[i][n >> 6] |= (uint64) nan << (n & 63);
	s->has_nan[i] |= nan;
	switch (s->in.col[i].type)
	{
		case SVEC_BOOL:
			((uint8 *) s->data[i])[n] = valid && sift_src_int(v->data, l->vtype, r) != 0;
			break;
		case SVEC_I32:
			((int32 *) s->data[i])[n] = valid ? (int32) sift_src_int(v->data, l->vtype, r) : 0;
			break;
		case SVEC_I64:
		case SVEC_NUM64:
			((int64 *) s->data[i])[n] = valid ? sift_src_int(v->data, l->vtype, r) : 0;
			break;
		case SVEC_F64:
			((double *) s->data[i])[n] = valid ? ((const double *) v->data)[r] : 0;
			break;
		case SVEC_STRVIEW:
			put_strview(s, &((SiftStrView *) s->data[i])[n], v, r, valid);
			break;
		default:
			((Datum *) s->data[i])[n] = valid ? datum_of(s, i, v, r) : (Datum) 0;
			break;
	}
}

static void
put_mask(V1Src *s, int m, uint32 r, int n)
{
	const SiftSrcVec *v = &s->read_vec[s->mask_read[m]];
	const bool	valid = v->isnull == NULL || !v->isnull[r];

	s->mvalid[m][n >> 6] |= (uint64) valid << (n & 63);
	s->mbits[m][n >> 6] |= (uint64) (valid && sift_src_int(v->data, s->mask_leaf[m]->vtype, r) != 0) << (n & 63);
}

static void
batch_start(V1Src *s)
{
	MemoryContextReset(s->batch_cx);
	for (int i = 0; i < s->in.ncols; i++)
	{
		memset(s->valid[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
		memset(s->nan[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
		s->has_nan[i] = false;
	}
	for (int m = 0; m < s->in.nmasks; m++)
	{
		memset(s->mbits[m], 0, SIFT_VEC_WORDS * sizeof(uint64));
		memset(s->mvalid[m], 0, SIFT_VEC_WORDS * sizeof(uint64));
	}
}

/* up to SIFT_VEC_LEN rows of the relation in the vectors, from as many units as that takes */
static int
batch_fill(V1Src *s)
{
	int			n = 0;

	batch_start(s);
	while (n < SIFT_VEC_LEN && (s->in_unit || (s->in_unit = unit_next(s))))
	{
		const int	cap = SIFT_VEC_LEN - n;
		const int	k = unit_take(s, cap);

		s->rd->tids(s->src, &s->unit, s->ords, k, s->tids);
		for (int j = 0; j < k; j++)
		{
			for (int i = 0; i < s->in.ncols; i++)
				put_col(s, i, s->ords[j], n + j);
			for (int m = 0; m < s->in.nmasks; m++)
				put_mask(s, m, s->ords[j], n + j);
			s->tag[n + j] = (uint64) ItemPointerGetBlockNumber(&s->tids[j]) << 16 | ItemPointerGetOffsetNumber(&s->tids[j]);
		}
		n += k;
		s->in_unit = k == cap;
	}
	for (int i = 0; i < s->in.ncols; i++)
		s->vec[i] = (SiftVec) {.data = s->data[i],.valid = s->valid[i],.len = (uint16) n,.dense = true,.type = s->in.col[i].type,.scale = s->in.col[i].scale,
			.nan = s->has_nan[i] ? s->nan[i] : NULL,.typid = s->in.col[i].type == SVEC_DATUM ? s->in.col[i].typid : InvalidOid,.typlen = s->typlen[i],.typbyval = s->typbyval[i]};
	return n;
}

static bool
v1_next(SiftV6Source *base, SiftV6Batch *out)
{
	V1Src	   *s = (V1Src *) base;
	int			n;

	while ((n = batch_fill(s)) > 0)
	{
		*out = (SiftV6Batch) {.n = (uint16) n,.col = s->vec,.masks = s->masks,.tag = s->tag};
		if (s->quals == NULL)
			return true;
		sift_v6_vexpr_cover(s->quals, &s->in, out);
		if (sift_v6_vexpr_filter(s->vctx, s->quals, out, s->mask) > 0)
		{
			out->mask = s->mask;
			return true;
		}
	}
	return false;
}

static bool
v1_fetch(SiftV6Source *base, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	V1Src	   *s = (V1Src *) base;
	ItemPointerData tid;
	bool		again = false,
				dead = false;

	ItemPointerSet(&tid, (BlockNumber) (tag >> 16), (OffsetNumber) (tag & 0xFFFF));
	if (s->index_fetch != NULL ? !table_index_fetch_tuple(s->index_fetch, &tid, s->snap, s->fslot, &again, &dead) :
		!table_tuple_fetch_row_version(s->rels.heap, &tid, s->snap, s->fslot))
		return false;
	for (int i = 0; i < n; i++)
		values[i] = slot_getattr(s->fslot, attnos[i], &isnull[i]);
	return true;
}

static void
v1_close(SiftV6Source *base)
{
	V1Src	   *s = (V1Src *) base;

	s->rd->close(s->src);
	if (s->index_fetch != NULL)
		table_index_fetch_end(s->index_fetch);
	ExecDropSingleTupleTableSlot(s->fslot);
	sift_v1_rel_close(&s->rels);
	MemoryContextDelete(s->cx);
}

/* ---- the open ---- */

static int
read_slot(V1Src *s, const SiftSrcLeaf *l)
{
	for (int k = 0; k < s->nread; k++)
		if (s->read_leaf[k] == l->id)
			return k;
	s->read_leaf[s->nread] = l->id;
	return s->nread++;
}

static const SiftSrcLeaf *
input_leaf(const V1Src *s, int att, SiftV6Class cls)
{
	const SiftSrcLeaf *l = att_leaf(s->lay, att);

	SIFT_INVARIANT(l != NULL && leaf_serves(l, att_typid(s->lay, att), cls), "attribute %d of relation %u is not a leaf Format 1 decodes as class %d", att, s->in.indexoid, (int) cls);
	return l;
}

static void
bind_columns(V1Src *s)
{
	for (int i = 0; i < s->in.ncols; i++)
	{
		SiftV6InCol *c = &s->in.col[i];
		const SiftSrcLeaf *l = input_leaf(s, c->attno, vec_class[c->type]);
		const size_t width = c->type == SVEC_STRVIEW ? sizeof(SiftStrView) : c->type == SVEC_I32 ? sizeof(int32) : c->type == SVEC_BOOL ? 1 : sizeof(int64);

		SIFT_INVARIANT(c->type != SVEC_NUM64 || l->scale == c->scale, "numeric attribute %d of relation %u is stored at scale %d, not %d", c->attno, s->in.indexoid, l->scale, c->scale);
		if (c->type == SVEC_DATUM)
		{
			c->typid = att_typid(s->lay, c->attno);
			get_typlenbyval(c->typid, &s->typlen[i], &s->typbyval[i]);
		}
		s->col_leaf[i] = l;
		s->col_read[i] = read_slot(s, l);
		s->data[i] = palloc_aligned((SIFT_VEC_LEN + SIFT_VEC_PAD) * width, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
		s->valid[i] = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
		s->nan[i] = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	}
	for (int m = 0; m < s->in.nmasks; m++)
	{
		SIFT_INVARIANT(s->in.mask[m].key == NULL, "a source of relation %u was asked for the keys of a document", s->in.indexoid);
		s->mask_leaf[m] = input_leaf(s, s->in.mask[m].attno, SV6C_BOOL);
		s->mask_read[m] = read_slot(s, s->mask_leaf[m]);
		s->mbits[m] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		s->mvalid[m] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		s->masks[m] = (SiftVexprMaskIn) {.bits = s->mbits[m],.valid = s->mvalid[m]};
	}
}

static SiftV6Source *
open_source(EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals, const SiftFilter *keep)
{
	MemoryContext cx = sift_mem_allocset_create(estate->es_query_cxt, "sift v1 source");
	V1Src	   *s = MemoryContextAllocZero(cx, sizeof(V1Src));
	MemoryContext old = MemoryContextSwitchTo(cx);

	s->cx = cx;
	s->unit_cx = sift_mem_allocset_create(cx, "sift v1 source unit");
	s->batch_cx = sift_mem_allocset_create(cx, "sift v1 source batch");
	s->in = *in;
	s->snap = estate->es_snapshot;
	s->rels = sift_v1_rel_open(in->indexoid, in->is_table);
	s->rd = reader_of(in->is_table);
	s->src = s->rd->open(s->rels.heap, in->is_table ? NULL : s->rels.rel, s->snap, cx);
	s->lay = s->rd->source_layout(s->src);
	bind_columns(s);
	s->fslot = table_slot_create(s->rels.heap, NULL);
	if (!in->is_table)
		s->index_fetch = sift_table_index_fetch_begin(s->rels.heap);
	if (quals != NULL)
		s->quals = sift_v6_vexpr_dup(cx, quals);
	s->vctx = sift_vexpr_ctx_new(cx);
	if (keep != NULL)
		s->rd->select(s->src, keep);
	s->rd->begin(s->src, 0, s->rd->nsb(s->src));
	MemoryContextSwitchTo(old);
	s->base = (SiftV6Source) {.next = v1_next,.fetch = v1_fetch,.close = v1_close};
	return &s->base;
}

static SiftV6Source *
v1_open(EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted)
{
	SIFT_INVARIANT(!sorted, "Format 1 sorts nothing: a sorted source of relation %u was asked for", in->indexoid);
	return open_source(estate, in, quals, NULL);
}

static const SiftStoreOps v1_store_ops = {
	.name = "v1",
	.owns_rel = v1_owns_rel,
	.schema = v1_schema,
	.stats = v1_stats,
	.open = v1_open,
	.leaf_holds = v1_leaf_holds
};

void
sift_v1_store_ops_init(void)
{
	sift_store_ops_register(&v1_store_ops);
}
