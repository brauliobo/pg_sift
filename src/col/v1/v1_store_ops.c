/*
 * col/v1/v1_store_ops.c - Format 1 behind the storage seam of the unified engine (core/store_ops.h): its sift_col indexes and sift_table tables are read, never written, as the batches of
 * SiftAggV6 (agg/batch_source.h) through the unit reader (col/v1/v1_reader.h) that decodes their units. The rows of a unit that the tombstones and the snapshot leave go to batches of
 * SIFT_VEC_LEN rows, which may span units; the quals of the operator run over each batch. The optional sub-vtables it hands over are in col/v1_engine/v1_store_ops_ext.c and v1_store_ops_jsonb.c.
 */
/* check-memory: allow-allocs every array here follows the columns of one scan or the rows of one batch, not the rows of the relation */
#include "postgres.h"

#include "access/tableam.h"
#include "access/stratnum.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class_d.h"
#include "commands/defrem.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "port/pg_bitutils.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/batch_source.h"
#include "col/am.h"
#include "col/source_fold.h"
#include "col/v1/v1_reader.h"
#include "col/v1/v1_store_ops.h"
#include "core/compat.h"
#include "core/memtrack.h"
#include "core/store_ops.h"
#include "core/tidbridge.h"
#include "table/v1/v1_tbl_ops.h"

/* ---- the leaves a scan decodes as they are stored ---- */

/* the leaf of attribute att when it is the only one (the lanes of a wide value, which have no type, are its head's): a scalar of the attribute itself, none for a document, a declared path or a list */
const SiftSrcLeaf *
sift_v1_att_leaf(const SiftSrcLayout *lay, int att)
{
	const SiftSrcLeaf *found = NULL;

	if (att >= SIFT_V6_JLEAF_VATT)
		return &lay->leaf[att - SIFT_V6_JLEAF_VATT];
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

Oid
sift_v1_att_typid(const SiftSrcLayout *lay, int att)
{
	const Node *e = att >= 1 && att <= list_length(lay->attexprs) ? list_nth(lay->attexprs, att - 1) : NULL;

	if (att >= SIFT_V6_JLEAF_VATT)
		return lay->leaf[att - SIFT_V6_JLEAF_VATT].typid;

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

const SiftV1ReaderOps *
sift_v1_reader_of(bool is_table)
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
	const SiftSrcLeaf *l = sift_v1_att_leaf(lay, att);
	const Node *e = att >= 1 && att <= list_length(lay->attexprs) ? list_nth(lay->attexprs, att - 1) : NULL;
	SiftStoreAtt a = {.attno = att,.typid = sift_v1_att_typid(lay, att),.typmod = e == NULL ? -1 : exprTypmod(e),.vec_type = SVEC_DATUM};

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

static Oid
att_opfamily(Oid typid)
{
	const Oid	opc = OidIsValid(typid) ? GetDefaultOpClass(typid, BTREE_AM_OID) : InvalidOid;

	return OidIsValid(opc) ? get_opclass_family(opc) : InvalidOid;
}

static uint8
leaf_store(const SiftSrcLeaf *l)
{
	if (l->wide)
		return SIFT_COLS_STR;
	return l->vtype == SCV_F64 ? SIFT_COLS_F8 : l->vtype == SCV_CODE ? SIFT_COLS_STR : SIFT_COLS_INT;
}

static SiftColDef
leaf_def(const SiftSrcLeaf *l)
{
	return (SiftColDef) {.id = l->id,.parent = l->parent,.kind = l->kind == SCL_SCALAR ? SIFT_COLK_SCALAR : SIFT_COLK_GROUP,.store = leaf_store(l),
		.flags = l->lossy || l->astext ? SIFT_COLD_LOSSY : 0,.attno = l->attno,.path = l->path != NIL || l->arrhead ? "path" : "",.scale = l->scale};
}

/* The leaves of the layout as the leaf map the planner reads the columns by: an attribute is held by the leaves that name it, and a scan decodes it when it is the one plain leaf (leaf_class_ok) */
static void
leaf_map(const SiftSrcLayout *lay, SiftStoreSchema *out)
{
	SiftColSchema *sc = palloc0(sizeof(SiftColSchema));
	int		   *first = palloc((out->natts + 1) * sizeof(int));
	int		   *count = palloc0((out->natts + 1) * sizeof(int));
	Oid		   *family = palloc0((out->natts + 1) * sizeof(Oid));
	SiftCellKind *kinds = palloc(Max(lay->nleaves, 1) * sizeof(SiftCellKind));

	sc->ncols = lay->nleaves;
	sc->cols = palloc0(Max(lay->nleaves, 1) * sizeof(SiftColDef));
	memset(first, -1, (out->natts + 1) * sizeof(int));
	for (int i = 0; i < lay->nleaves; i++)
	{
		const SiftSrcLeaf *l = &lay->leaf[i];

		sc->cols[i] = leaf_def(l);
		kinds[i] = l->vtype == SCV_F64 ? SIFT_CELL_F8 : l->vtype == SCV_CODE ? SIFT_CELL_STR : SIFT_CELL_INT;
		if (l->attno < 1 || l->attno > out->natts)
			continue;
		first[l->attno] = first[l->attno] < 0 ? i : first[l->attno];
		count[l->attno]++;
	}
	for (int a = 1; a <= out->natts; a++)
		family[a] = att_opfamily(out->atts[a - 1].typid);
	out->leaves = sc;
	out->att_first = first;
	out->att_nleaves = count;
	out->att_opfamily = family;
	out->leaf_kinds = kinds;
	out->leaf_hints = palloc0(Max(lay->nleaves, 1));
}

static bool
v1_schema(Oid relid, bool is_table, SiftStoreSchema *out)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	const SiftSrcLayout *lay = sift_v1_reader_of(is_table)->layout(r.rel);

	*out = (SiftStoreSchema) {.natts = lay->natts,.is_table = is_table,.atts = palloc0(lay->natts * sizeof(SiftStoreAtt))};
	for (int a = 1; a <= lay->natts; a++)
	{
		out->atts[a - 1] = describe_att(lay, a);
		out->has_jsonb |= out->atts[a - 1].is_jsonb;
	}
	leaf_map(lay, out);
	sift_v1_rel_close(&r);
	return true;
}

static double
v1_point_cover(Oid relid, int col)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, false);
	const SiftSrcLeaf *l = sift_v1_att_leaf(sift_v1_reader_of(false)->layout(r.rel), col + 1);
	SiftSrcStats st;

	sift_v1_reader_of(false)->stats(r.rel, &st);
	sift_v1_rel_close(&r);
	return l != NULL ? st.point_cover[l->id] : 0.0;
}

static bool
v1_stats(Oid relid, bool is_table, SiftStoreStats *out)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	SiftSrcStats st;

	sift_v1_reader_of(is_table)->stats(r.rel, &st);
	*out = (SiftStoreStats) {.live_rows = r.heap->rd_rel->reltuples >= 0 ? r.heap->rd_rel->reltuples : st.nrows,
		.stored_bytes = is_table ? table_relation_size(r.heap, MAIN_FORKNUM) : (uint64) st.bytes,.allvis_frac = Min(Max(st.allvisible_frac, 0.0), 1.0),
		.point_cover = is_table ? NULL : v1_point_cover};
	sift_v1_rel_close(&r);
	return true;
}

static bool
v1_leaf_holds(Oid relid, bool is_table, int att, int cls)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	const SiftSrcLayout *lay = sift_v1_reader_of(is_table)->layout(r.rel);
	const SiftSrcLeaf *l = sift_v1_att_leaf(lay, att);
	const bool	holds = l != NULL && leaf_serves(l, sift_v1_att_typid(lay, att), (SiftV6Class) cls);

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
	const SiftTidBridge *bridge;
	int			ntypes;
	SiftV1TypeTest types[SIFT_V1_MAX_TYPE_TESTS];
	int			score_col;		/* the column of the input that holds the scores of the bridge, -1 for none */
	uint64		kept[SIFT_VEC_WORDS];
	float4		score[SIFT_VEC_LEN];
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
types_pass(V1Src *s)
{
	for (int t = 0; t < s->ntypes; t++)
		if (!s->rd->filter_types(s->src, &s->unit, s->types[t].leaf, s->types[t].types, s->bits))
			return false;
	return true;
}

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
		if (!types_pass(s))
			continue;
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
				if (i != s->score_col)
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

/* only the rows the bridge holds, the scores of its hits in the score column */
static bool
bridge_narrow(V1Src *s, SiftV6Batch *b)
{
	const int	words = (b->n + 63) / 64;
	uint64		live[SIFT_VEC_WORDS];

	memset(live, 0xFF, words * sizeof(uint64));
	if (b->n & 63)
		live[words - 1] = (UINT64CONST(1) << (b->n & 63)) - 1;
	if (sift_tidbridge_probe_vec(s->bridge, b->tag, live, b->n, s->kept, s->score) == 0)
		return false;
	if (s->score_col >= 0)
	{
		for (int r = 0; r < b->n; r++)
			((double *) s->data[s->score_col])[r] = s->score[r];
		memcpy(s->valid[s->score_col], live, words * sizeof(uint64));
	}
	b->mask = s->kept;
	return true;
}

static bool
v1_next(SiftV6Source *base, SiftV6Batch *out)
{
	V1Src	   *s = (V1Src *) base;
	int			n;

	while ((n = batch_fill(s)) > 0)
	{
		*out = (SiftV6Batch) {.n = (uint16) n,.col = s->vec,.masks = s->masks,.tag = s->tag};
		if (s->bridge != NULL && !bridge_narrow(s, out))
			continue;
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
	const SiftSrcLeaf *l = sift_v1_att_leaf(s->lay, att);

	SIFT_INVARIANT(l != NULL && leaf_serves(l, sift_v1_att_typid(s->lay, att), cls), "attribute %d of relation %u is not a leaf Format 1 decodes as class %d", att, s->in.indexoid, (int) cls);
	return l;
}

static void
alloc_column(V1Src *s, int i, size_t width)
{
	s->data[i] = palloc_aligned((SIFT_VEC_LEN + SIFT_VEC_PAD) * width, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	s->valid[i] = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	s->nan[i] = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
}

static void
bind_column(V1Src *s, int i)
{
	SiftV6InCol *c = &s->in.col[i];
	const SiftSrcLeaf *l = input_leaf(s, c->attno, vec_class[c->type]);
	const size_t width = c->type == SVEC_STRVIEW ? sizeof(SiftStrView) : c->type == SVEC_I32 ? sizeof(int32) : c->type == SVEC_BOOL ? 1 : sizeof(int64);

	SIFT_INVARIANT(c->type != SVEC_NUM64 || l->scale == c->scale, "numeric attribute %d of relation %u is stored at scale %d, not %d", c->attno, s->in.indexoid, l->scale, c->scale);
	if (c->type == SVEC_DATUM)
	{
		c->typid = sift_v1_att_typid(s->lay, c->attno);
		get_typlenbyval(c->typid, &s->typlen[i], &s->typbyval[i]);
	}
	s->col_leaf[i] = l;
	s->col_read[i] = read_slot(s, l);
	alloc_column(s, i, width);
}

static void
bind_score(V1Src *s, int i)
{
	SIFT_INVARIANT(s->bridge != NULL && s->in.col[i].type == SVEC_F64, "relation %u was asked for scores of a scan without a text bridge", s->in.indexoid);
	s->score_col = i;
	alloc_column(s, i, sizeof(double));
}

static void
bind_columns(V1Src *s)
{
	s->score_col = -1;
	for (int i = 0; i < s->in.ncols; i++)
		if (s->in.col[i].attno == SIFT_V6_SCORE_ATTNO)
			bind_score(s, i);
		else
			bind_column(s, i);
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

static void
bind_types(V1Src *s, const SiftV1Scan *scan)
{
	for (int t = 0; t < scan->ntypes; t++)
	{
		bool		read = false;

		for (int i = 0; i < s->in.ncols; i++)
			read |= s->col_leaf[i] != NULL && s->col_leaf[i]->id == scan->types[t].leaf;
		s->types[t] = (SiftV1TypeTest) {.leaf = scan->types[t].leaf,.types = scan->types[t].types | (read ? SIFT_SRC_JT_READ : 0)};
	}
	s->ntypes = scan->ntypes;
}

/* A unit whose zone of an integer column lies wholly outside the bounds the program puts on it is not read; the program still tests every row of the units that are */
static void
prune_begin(V1Src *s, const SiftV1Range *range)
{
	const uint32 nsb = s->rd->nsb(s->src);
	SiftSrcBound bound[SIFT_SRC_MAX_BOUNDS];
	SiftSrcPrune *prune = palloc0(SIFT_SRC_MAX_BOUNDS * sizeof(SiftSrcPrune));
	const int	nb = s->quals != NULL ? sift_srcfold_bounds(s->quals, bound) : 0;
	int			np = 0;

	for (int i = 0; i < nb; i++)
	{
		const SiftSrcLeaf *l = s->col_leaf[bound[i].slot];

		if (l == NULL || l->wide || (l->vtype != SCV_I32 && l->vtype != SCV_I64))
			continue;
		prune[np++] = (SiftSrcPrune) {.leaf = l->id,.lo = bound[i].has_lo ? bound[i].lo : PG_INT64_MIN,.hi = bound[i].has_hi ? bound[i].hi : PG_INT64_MAX,
			.nvals = bound[i].nvals,.vals = bound[i].vals};
	}
	s->rd->begin(s->src, range ? range->sb_lo : 0, range ? Min(range->sb_hi, nsb) : nsb, prune, np);
}

SiftV6Source *
sift_v1_source_open(MemoryContext parent, Snapshot snap, const SiftV6Input *in, const SiftV6Vexpr *quals, const SiftV1Scan *scan)
{
	const SiftV1Scan none = {0};
	const SiftV1Range *range;
	MemoryContext cx = sift_mem_allocset_create(parent, "sift v1 source");
	V1Src	   *s = MemoryContextAllocZero(cx, sizeof(V1Src));
	MemoryContext old = MemoryContextSwitchTo(cx);

	scan = scan != NULL ? scan : &none;
	range = scan->range;
	s->cx = cx;
	s->unit_cx = sift_mem_allocset_create(cx, "sift v1 source unit");
	s->batch_cx = sift_mem_allocset_create(cx, "sift v1 source batch");
	s->in = *in;
	s->snap = snap;
	s->bridge = scan->bridge;
	s->rels = sift_v1_rel_open(in->indexoid, in->is_table);
	s->rd = sift_v1_reader_of(in->is_table);
	s->src = s->rd->open(s->rels.heap, in->is_table ? NULL : s->rels.rel, s->snap, cx, range ? range->view : NULL);
	s->lay = s->rd->source_layout(s->src);
	bind_columns(s);
	s->fslot = table_slot_create(s->rels.heap, NULL);
	if (!in->is_table)
		s->index_fetch = sift_table_index_fetch_begin(s->rels.heap);
	if (quals != NULL)
		s->quals = sift_v6_vexpr_dup(cx, quals);
	s->vctx = sift_vexpr_ctx_new(cx);
	if (scan->keep != NULL)
		s->rd->select(s->src, scan->keep);
	bind_types(s, scan);
	prune_begin(s, range);
	MemoryContextSwitchTo(old);
	s->base = (SiftV6Source) {.next = v1_next,.fetch = v1_fetch,.close = v1_close};
	return &s->base;
}

static SiftV6Source *
v1_open(EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted)
{
	SIFT_INVARIANT(!sorted, "Format 1 sorts nothing: a sorted source of relation %u was asked for", in->indexoid);
	return sift_v1_source_open(estate->es_query_cxt, estate->es_snapshot, in, quals, NULL);
}

static const SiftStoreOps v1_store_ops = {
	.name = "v1",
	.owns_rel = v1_owns_rel,
	.schema = v1_schema,
	.stats = v1_stats,
	.open = v1_open,
	.leaf_holds = v1_leaf_holds,
	.zone = &sift_v1_zone_ops,
	.dict = &sift_v1_dict_ops,
	.par = &sift_v1_par_ops,
	.order = &v1_order_ops,
	.preagg = &v1_preagg_ops,
	.jsonb = &v1_jsonb_ops,
	.hybrid = &v1_hybrid_ops
};

void
sift_v1_store_ops_init(void)
{
	sift_store_ops_register(&v1_store_ops);
}
