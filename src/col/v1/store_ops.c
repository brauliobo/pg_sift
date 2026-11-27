/*
 * col/v1/store_ops.c - Format 1 behind the storage seam of the unified engine (core/store/ops.h): its sift_col indexes and sift_table tables are read, never written, as the batches of
 * SiftAggV6 (agg/batch_source.h) through the unit reader (col/v1/reader.h) that decodes their units. The rows of a unit that the tombstones and the snapshot leave go to batches of
 * SIFT_VEC_LEN rows, which may span units; the quals of the operator run over each batch. The optional sub-vtables it hands over are in col/v1_engine/store_ops_ext.c and v1_store_ops_jsonb.c.
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
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/batch_source.h"
#include "col/am.h"
#include "col/col.h"
#include "col/source_fold.h"
#include "col/v1/reader.h"
#include "col/v1/store_ops.h"
#include "col/v1/store_src.h"
#include "core/compat.h"
#include "core/errctx.h"
#include "core/memtrack.h"
#include "core/store/ops.h"
#include "core/tidbridge.h"
#include "table/v1/rel.h"

/* ---- the leaves a scan decodes as they are stored ---- */

/* the leaf of attribute att when it is the only one (the lanes of a wide value, which have no type, are its head's): a scalar of the attribute itself, none for a document, a declared path or a list */
const SiftSrcLeaf *
sift_v1_att_leaf(const SiftSrcLayout *lay, int att)
{
	const SiftSrcLeaf *found = NULL;

	if (att >= SIFT_V6_JLEAF_VATT)
		return lay->leaf[att - SIFT_V6_JLEAF_VATT].parent < 0 ? &lay->leaf[att - SIFT_V6_JLEAF_VATT] : NULL;
	for (int i = 0; i < lay->nleaves; i++)
	{
		const SiftSrcLeaf *l = &lay->leaf[i];

		if (l->attno != att || l->typid == InvalidOid || l->scope)
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

	return e == NULL ? InvalidOid : getBaseType(exprType(e));
}

AttrNumber
sift_v1_att_heap_column(const SiftSrcLayout *lay, int att)
{
	const Node *e = att >= 1 && att <= list_length(lay->attexprs) ? list_nth(lay->attexprs, att - 1) : NULL;

	return e != NULL && IsA(e, Var) ? ((const Var *) e)->varattno : 0;
}

/* a jsonb document is no leaf: it is the Datum of its heap row, which the scan reads for the rows it keeps */
static bool
doc_holds(const SiftSrcLayout *lay, int att)
{
	return att >= 1 && att <= lay->natts && sift_v1_att_typid(lay, att) == JSONBOID && sift_v1_att_heap_column(lay, att) != 0;
}

/* the list of the elements of an array column (`paths = '+tags[] text'`): the leaf of its lengths, which has no path, and the leaves of the elements below it */
static bool
elem_list(const SiftSrcLeaf *l)
{
	return l->parent >= 0 || (l->kind == SCL_ARRAY && l->path == NIL);
}

/* the head of the leaves that keep attribute att whole: a scalar of the attribute itself, which for an array is the image its lengths and the list of its elements come after */
static const SiftSrcLeaf *
datum_head(const SiftSrcLayout *lay, int att)
{
	const SiftSrcLeaf *found = NULL;

	for (int i = 0; i < lay->nleaves; i++)
	{
		const SiftSrcLeaf *l = &lay->leaf[i];

		if (l->attno != att || l->typid == InvalidOid || l->scope || l->arrlen != 0 || elem_list(l))
			continue;
		if (found != NULL || l->kind != SCL_SCALAR || l->path != NIL || l->noimage || l->lossy || l->astext)
			return NULL;
		found = l;
	}
	return found;
}

static bool
is_text_type(Oid typid)
{
	return typid == TEXTOID || typid == VARCHAROID || typid == BPCHAROID || typid == NAMEOID;
}

/* a type whose Datum is made of the leaf as stored: an integer or float by value, a scaled numeric, an enum label, a text from its dictionary, or the image of a wide value */
static bool
datum_by_value(const SiftSrcLeaf *l, Oid typid)
{
	if (typid == FLOAT4OID || typid == FLOAT8OID)
		return l->vtype == SCV_F64;
	if (typid == NUMERICOID)
		return l->vtype == SCV_I64;
	if (is_text_type(typid))
		return l->vtype == SCV_CODE;
	if (l->vtype == SCV_I32 && get_typtype(typid) == TYPTYPE_ENUM)
		return true;
	return (typid == BOOLOID || typid == INT2OID || typid == INT4OID || typid == INT8OID || typid == DATEOID || typid == TIMEOID || typid == TIMESTAMPOID || typid == TIMESTAMPTZOID ||
			typid == CASHOID) && (l->vtype == SCV_I32 || l->vtype == SCV_I64);
}

static bool
leaf_serves(const SiftSrcLeaf *l, Oid typid, SiftV6Class cls)
{
	if (l->wide || l->arrhead)
		return cls == SV6C_DATUM && !l->noimage && l->vtype == SCV_CODE;
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
	const char	relkind = get_rel_relkind(relid);

	if (is_table)
		return (relkind == RELKIND_RELATION || relkind == RELKIND_MATVIEW) && sift_v1_tbl_owns(relid);
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
	if (a.is_jsonb)
		return a;
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
			a.vec_type = a.typid == NUMERICOID ? SVEC_DATUM : SVEC_I64;
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
		if (l->attno < 1 || l->attno > out->natts || l->scope)
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

static int
v1_card_leaf(Oid relid, int col)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, false);
	const int	leaf = sift_v1_card_leaf(sift_v1_reader_of(false)->layout(r.rel), col);

	sift_v1_rel_close(&r);
	return leaf;
}

static bool
v1_stats(Oid relid, bool is_table, SiftStoreStats *out)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	SiftSrcStats st;
	const bool	priced = !is_table;

	sift_v1_reader_of(is_table)->stats(r.rel, &st);
	*out = (SiftStoreStats) {.live_rows = r.heap->rd_rel->reltuples >= 0 ? r.heap->rd_rel->reltuples : st.nrows,
		.stored_bytes = is_table ? table_relation_size(r.heap, MAIN_FORKNUM) : (uint64) st.bytes,.allvis_frac = Min(Max(st.allvisible_frac, 0.0), 1.0),.row_frac = st.row_frac,
		.card_view = priced ? sift_v1_card_view(sift_v1_reader_of(false)->layout(r.rel), &st) : NULL,
		.card_leaf = priced ? v1_card_leaf : NULL};
	sift_v1_rel_close(&r);
	return true;
}

/* a leaf that kept no value of some rows (a string or a bytea past the limits of its unit) leaves those rows' values in the heap: a scan of the leaf alone would read them as NULL */
static bool
leaf_complete(const SiftV1Rel *r, bool is_table, int att, const SiftSrcLeaf *l)
{
	SiftSrcStats st;

	if (att >= SIFT_V6_JLEAF_VATT)
		return true;
	sift_v1_reader_of(is_table)->stats(r->rel, &st);
	return !st.ovf[l->id];
}

static bool
v1_leaf_holds(Oid relid, bool is_table, int att, int cls)
{
	const SiftV1Rel r = sift_v1_rel_open(relid, is_table);
	const SiftSrcLayout *lay = sift_v1_reader_of(is_table)->layout(r.rel);
	const SiftSrcLeaf *l = cls == SV6C_DATUM ? datum_head(lay, att) : sift_v1_att_leaf(lay, att);
	const bool	holds = (cls == SV6C_DATUM && doc_holds(lay, att)) ||
		(l != NULL && leaf_serves(l, sift_v1_att_typid(lay, att), (SiftV6Class) cls) && leaf_complete(&r, is_table, att, l));

	sift_v1_rel_close(&r);
	return holds;
}

/* ---- the batches ---- */

static bool
types_pass(SiftV1Src *s)
{
	for (int t = 0; t < s->ntypes; t++)
		if (s->types[t].key ? !sift_v1_src_filter_key(s, &s->types[t], s->bits) : !sift_v1_src_filter_types(s, &s->lay->leaf[s->types[t].leaf], s->types[t].types, s->bits))
			return false;
	return true;
}

static bool
unit_next(SiftV1Src *s)
{
	for (;;)
	{
		int			nw;

		MemoryContextReset(s->unit_cx);
		if (!s->rd->next(s->src, &s->unit))
			return false;
		if (s->cube != NULL && s->cube->offer(s->cube->arg, s->rd, s->src, &s->unit))
			continue;
		nw = (s->unit.nrow + 63) / 64;
		s->bits = MemoryContextAllocZero(s->unit_cx, SIFT_COL_NWORDS(s->unit.nrow) * sizeof(uint64));
		s->rd->live(s->src, &s->unit, s->bits);
		s->vet_unit = s->rd->vet_needed(s->src, &s->unit);
		if (!types_pass(s))
			continue;
		for (int w = 0; w < nw; w++)
			if (s->bits[w] != 0)
			{
				for (int i = 0; i < s->in.ncols; i++)
					if (s->jkind[i] == SIFT_V1_JSON_TYPE)
						sift_v1_src_read_types(s, i);
					else if (s->jkind[i] == SIFT_V1_JSON_PRESENT)
						sift_v1_src_read_presence(s, i);
				s->units_read++;
				s->rows_read += s->unit.nrow;
				for (int k = 0; k < s->nread; k++)
					s->rd->read(s->src, &s->unit, s->read_leaf[k], &s->read_vec[k]);
				for (int i = 0; i < s->in.ncols; i++)
					if (s->col_mem[i] != NULL)
						sift_v1_member_unit(s->col_mem[i], s->read_vec);
				s->pos = 0;
				return true;
			}
	}
}

static int
unit_take(SiftV1Src *s, int cap)
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
unreadable(const SiftV1Src *s, const SiftSrcLeaf *l)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("column \"%s\" holds a value the stored form cannot hold", get_attname(s->in.indexoid, l->attno, false)),
					errhint("Run the statement with sift.enable_agg = off.")));
}

static void
put_strview(SiftV1Src *s, SiftStrView *sv, const SiftSrcVec *v, uint32 r, bool valid)
{
	if (valid)
		sift_v1_strview(s->batch_cx, sv, v->dict, ((const uint32 *) v->data)[r]);
	else
		memset(sv, 0, sizeof(*sv));
}

static Datum
numeric_datum(const SiftSrcLeaf *l, const SiftSrcVec *v, uint32 r)
{
	return v->ovf != NULL && sift_src_bit(v->ovf, r) ? sift_col_nan_numeric() : sift_col_key_numeric(l->scale, sift_src_int(v->data, l->vtype, r));
}

static Datum
code_datum(SiftV1Src *s, Oid typid, const SiftSrcVec *v, uint32 r, bool image)
{
	char		buf[SIFT_SRC_DICT_ENTRY];
	uint32		len;
	const char *p = sift_srcdict_str(v->dict, ((const uint32 *) v->data)[r], &len, buf);
	NameData   *name;

	if (image)
		return PointerGetDatum(memcpy(MemoryContextAlloc(s->batch_cx, len), p, len));
	if (typid != NAMEOID)
		return PointerGetDatum(cstring_to_text_with_len(p, len));
	name = MemoryContextAllocZero(s->batch_cx, NAMEDATALEN);
	memcpy(NameStr(*name), p, Min(len, NAMEDATALEN - 1));
	return NameGetDatum(name);
}

static Datum
datum_of(SiftV1Src *s, int i, const SiftSrcVec *v, uint32 r)
{
	const SiftSrcLeaf *l = s->col_leaf[i];
	const Oid	typid = s->in.col[i].typid;

	if (l->vtype == SCV_CODE)
		return code_datum(s, typid, v, r, l->wide || l->arrhead);
	if (typid == NUMERICOID)
		return numeric_datum(l, v, r);
	switch (typid)
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
			return get_typtype(typid) == TYPTYPE_ENUM ? ObjectIdGetDatum((Oid) sift_src_int(v->data, l->vtype, r)) : Int64GetDatum(sift_src_int(v->data, l->vtype, r));
	}
}

static void
put_col(SiftV1Src *s, int i, uint32 r, int n)
{
	const SiftSrcVec *v = &s->read_vec[s->col_read[i]];
	const SiftSrcLeaf *l = s->col_leaf[i];
	const bool	datum = s->in.col[i].type == SVEC_DATUM;
	const bool	ovf = v->ovf != NULL && sift_src_bit(v->ovf, r);
	const bool	exc = ovf && s->jkind[i] == SIFT_V1_JSON_VALUE;
	const bool	nan = ovf && !exc && !datum;
	const bool	valid = (v->isnull == NULL || !v->isnull[r]) && !exc && !nan;

	if (nan && !sift_src_nan_leaf(l))
		unreadable(s, l);
	if (exc)
	{
		s->jexc[i][n >> 6] |= UINT64CONST(1) << (n & 63);
		s->has_jexc[i] = true;
	}
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
put_mask(SiftV1Src *s, int m, uint32 r, int n)
{
	const SiftSrcVec *v = &s->read_vec[s->mask_read[m]];
	const bool	valid = v->isnull == NULL || !v->isnull[r];

	s->mvalid[m][n >> 6] |= (uint64) valid << (n & 63);
	s->mbits[m][n >> 6] |= (uint64) (valid && sift_src_int(v->data, s->mask_leaf[m]->vtype, r) != 0) << (n & 63);
}

static void
batch_start(SiftV1Src *s)
{
	MemoryContextReset(s->batch_cx);
	for (int i = 0; i < s->in.ncols; i++)
	{
		memset(s->valid[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
		memset(s->nan[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
		s->has_nan[i] = false;
		if (s->col_mem[i] != NULL)
			sift_v1_member_batch(s->col_mem[i]);
		if (s->jexc[i] != NULL)
			memset(s->jexc[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
		s->has_jexc[i] = false;
	}
	for (int m = 0; m < s->in.nmasks; m++)
	{
		memset(s->mbits[m], 0, SIFT_VEC_WORDS * sizeof(uint64));
		memset(s->mvalid[m], 0, SIFT_VEC_WORDS * sizeof(uint64));
	}
}

/* up to SIFT_VEC_LEN rows of the relation in the vectors, from as many units as that takes */
static int
batch_fill(SiftV1Src *s)
{
	int			n = 0;

	batch_start(s);
	while (n < SIFT_VEC_LEN && (s->in_unit || (s->in_unit = unit_next(s))))
	{
		const int	cap = SIFT_VEC_LEN - n;
		const int	k = unit_take(s, cap);

		s->vet_from = n;
		s->rd->tids(s->src, &s->unit, s->ords, k, s->tids);
		for (int j = 0; j < k; j++)
		{
			for (int i = 0; i < s->in.ncols; i++)
				if (s->col_mem[i] != NULL)
					sift_v1_member_put(s->col_mem[i], s->ords[j], n + j, s->data[i], s->valid[i]);
				else if (s->jkind[i] == SIFT_V1_JSON_TYPE)
					sift_v1_src_put_type(s, i, s->ords[j], n + j);
				else if (s->jkind[i] == SIFT_V1_JSON_PRESENT)
					sift_v1_src_put_presence(s, i, s->ords[j], n + j);
				else if (i != s->score_col && s->col_leaf[i] != NULL)
					put_col(s, i, s->ords[j], n + j);
			for (int m = 0; m < s->in.nmasks; m++)
				put_mask(s, m, s->ords[j], n + j);
			s->tag[n + j] = (uint64) ItemPointerGetBlockNumber(&s->tids[j]) << 16 | ItemPointerGetOffsetNumber(&s->tids[j]);
		}
		n += k;
		s->in_unit = k == cap;
		if (n > 0 && (s->elems != NULL || s->vet_unit))
			break;
	}
	for (int i = 0; i < s->in.ncols; i++)
		s->vec[i] = (SiftVec) {.data = s->data[i],.valid = s->valid[i],.len = (uint16) n,.dense = true,.type = s->in.col[i].type,.scale = s->in.col[i].scale,
			.nan = s->has_nan[i] ? s->nan[i] : NULL,.typid = s->in.col[i].type == SVEC_DATUM ? s->in.col[i].typid : InvalidOid,.typlen = s->typlen[i],.typbyval = s->typbyval[i]};
	return n;
}

/* only the rows the bridge holds, the scores of its hits in the score column */
static bool
bridge_narrow(SiftV1Src *s, SiftV6Batch *b)
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

/* the elements of the rows the batch keeps, from the leaves of the unit it is of (a batch of a source that reads elements never spans two) */
static void
fill_elems(SiftV1Src *s, const SiftV6Batch *b)
{
	const SiftSrcVec *child[SIFT_V1_MAX_ELEM_COLS];

	for (int c = 0; c < s->nelem_child; c++)
		child[c] = &s->read_vec[s->elem_child[c]];
	s->elem_batch = sift_v1_elems_batch(s->elems, &s->read_vec[s->elem_group], child, s->ords, b->n, b->mask, s->batch_cx);
}

static bool
next_batch(SiftV1Src *s, SiftV6Batch *out)
{
	int			n;

	while ((n = batch_fill(s)) > 0)
	{
		*out = (SiftV6Batch) {.n = (uint16) n,.col = s->vec,.masks = s->masks,.tag = s->tag};
		if (s->bridge != NULL && !bridge_narrow(s, out))
			continue;
		if (s->quals != NULL)
		{
			sift_v6_vexpr_cover(s->quals, &s->in, out);
			if (sift_v6_vexpr_filter(s->vctx, s->quals, out, s->mask) == 0)
				continue;
			out->mask = s->mask;
		}
		if (s->tid_range && !sift_v1_src_tid_narrow(s, out))
			continue;
		if (s->vet_unit && !sift_v1_src_vet(s, out))
			continue;
		sift_v1_src_fill_heap(s, out);
		if (s->elems != NULL)
			fill_elems(s, out);
		return true;
	}
	return false;
}

/* the decoders of the units report damage against the relation of the scope (core/damage.h): the one the source reads, as its open does */
static bool
v1_next(SiftV6Source *base, SiftV6Batch *out)
{
	SiftV1Src	   *s = (SiftV1Src *) base;
	SiftErrCtx	ctx;
	bool		got;

	sift_errctx_push(&ctx, s->in.indexoid);
	got = next_batch(s, out);
	sift_errctx_pop(&ctx);
	return got;
}

const SiftV1ElemBatch *
sift_v1_source_elems(SiftV6Source *base)
{
	return ((SiftV1Src *) base)->elem_batch;
}

bool
sift_v1_src_fetch(SiftV6Source *base, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	SiftV1Src	   *s = (SiftV1Src *) base;

	if (!sift_v1_src_fetch_row(s, tag))
		return false;
	for (int i = 0; i < n; i++)
		values[i] = slot_getattr(s->fslot, attnos[i], &isnull[i]);
	return true;
}

static void
v1_granules(SiftV6Source *base, SiftV6GranCounts *out)
{
	SiftV1Src	   *s = (SiftV1Src *) base;

	out->read += s->units_read;
	out->rows += s->rows_read;
	s->rd->pruned(s->src, out);
}

static void
v1_close(SiftV6Source *base)
{
	SiftV1Src	   *s = (SiftV1Src *) base;

	s->rd->close(s->src);
	if (s->index_fetch != NULL)
		table_index_fetch_end(s->index_fetch);
	ExecDropSingleTupleTableSlot(s->fslot);
	sift_v1_rel_close(&s->rels);
	MemoryContextDelete(s->cx);
}

/* ---- the open ---- */

static int
read_slot(SiftV1Src *s, const SiftSrcLeaf *l)
{
	for (int k = 0; k < s->nread; k++)
		if (s->read_leaf[k] == l->id)
			return k;
	s->read_leaf[s->nread] = l->id;
	return s->nread++;
}

static const SiftSrcLeaf *
input_leaf(const SiftV1Src *s, int att, SiftV6Class cls)
{
	const SiftSrcLeaf *l = cls == SV6C_DATUM ? datum_head(s->lay, att) : sift_v1_att_leaf(s->lay, att);

	SIFT_INVARIANT(l != NULL && leaf_serves(l, sift_v1_att_typid(s->lay, att), cls), "attribute %d of relation %u is not a leaf Format 1 decodes as class %d", att, s->in.indexoid, (int) cls);
	return l;
}

static void
alloc_column(SiftV1Src *s, int i, size_t width)
{
	s->data[i] = palloc_aligned((SIFT_VEC_LEN + SIFT_VEC_PAD) * width, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	s->valid[i] = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	s->nan[i] = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
}

static void
bind_doc(SiftV1Src *s, int i)
{
	SiftV6InCol *c = &s->in.col[i];

	SIFT_INVARIANT(doc_holds(s->lay, c->attno), "attribute %d of relation %u is not a jsonb document Format 1 reads from the heap row", c->attno, s->in.indexoid);
	c->typid = JSONBOID;
	get_typlenbyval(c->typid, &s->typlen[i], &s->typbyval[i]);
	s->doc_col[s->ndocs] = i;
	s->doc_att[s->ndocs++] = sift_v1_att_heap_column(s->lay, c->attno);
	alloc_column(s, i, sizeof(Datum));
}

/* the JSON type of a key of a document (SIFT_V6_JTYPE_VATT + the leaf of the key) and whether a document has the key (SIFT_V6_JPRES_VATT + the leaf) are columns of the input of their own */
static void
bind_type(SiftV1Src *s, int i)
{
	const bool	present = sift_v6_vatt_is_presence(s->in.col[i].attno);
	SiftSrcStats st;

	SIFT_INVARIANT(sift_v6_vatt_is_type(s->in.col[i].attno) && s->in.col[i].type == (present ? SVEC_I64 : SVEC_STRVIEW), "attribute %d of relation %u is not the JSON type of a key of a document",
				   s->in.col[i].attno, s->in.indexoid);
	s->jkind[i] = present ? SIFT_V1_JSON_PRESENT : SIFT_V1_JSON_TYPE;
	s->jleaf[i] = &s->lay->leaf[sift_v6_vatt_leaf(s->in.col[i].attno)];
	if (present)
	{
		s->rd->stats(s->rels.rel, &st);
		s->jrootless[i] = st.rootless[s->jleaf[i]->id] > 0;
	}
	alloc_column(s, i, present ? sizeof(int64) : sizeof(SiftStrView));
}

static void
bind_member(SiftV1Src *s, int i)
{
	const SiftV6InCol *c = &s->in.col[i];
	SiftV1Member *m = sift_v1_member_new(s->cx, s->lay, c->attno - SIFT_V6_JLEAF_VATT, c->path);

	for (int part = 0; part < 3; part++)
		sift_v1_member_bind(m, part, read_slot(s, &s->lay->leaf[sift_v1_member_leaf(m, part)]));
	s->col_mem[i] = m;
	s->mem_att[i] = sift_v1_att_heap_column(s->lay, s->lay->leaf[sift_v1_member_leaf(m, 0)].attno);
	alloc_column(s, i, sizeof(double));
}

static void
bind_column(SiftV1Src *s, int i)
{
	SiftV6InCol *c = &s->in.col[i];
	const SiftSrcLeaf *l;
	size_t		width;

	if (c->path != NIL)
	{
		bind_member(s, i);
		return;
	}

	if (c->type == SVEC_DATUM && c->typid == JSONBOID)
	{
		bind_doc(s, i);
		return;
	}
	if (sift_v6_vatt_is_json(c->attno))
	{
		bind_type(s, i);
		return;
	}
	l = sift_v6_attno_found(c->attno) >= 0 ? sift_v1_found_leaf(s->rels.rel, s->lay, c->attno) : input_leaf(s, c->attno, vec_class[c->type]);
	width = c->type == SVEC_STRVIEW ? sizeof(SiftStrView) : c->type == SVEC_I32 ? sizeof(int32) : c->type == SVEC_BOOL ? 1 : sizeof(int64);
	SIFT_INVARIANT(c->type != SVEC_NUM64 || l->scale == c->scale, "numeric attribute %d of relation %u is stored at scale %d, not %d", c->attno, s->in.indexoid, l->scale, c->scale);
	if (c->type == SVEC_DATUM)
	{
		c->typid = sift_v1_att_typid(s->lay, c->attno);
		get_typlenbyval(c->typid, &s->typlen[i], &s->typbyval[i]);
	}
	s->col_leaf[i] = l;
	s->col_read[i] = read_slot(s, l);
	alloc_column(s, i, width);
	if (c->attno >= SIFT_V6_JLEAF_VATT)
	{
		s->jkind[i] = SIFT_V1_JSON_VALUE;
		s->jatt[i] = sift_v1_att_heap_column(s->lay, l->attno);
		s->jexc[i] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
	}
}

static void
bind_score(SiftV1Src *s, int i)
{
	SIFT_INVARIANT(s->bridge != NULL && s->in.col[i].type == SVEC_F64, "relation %u was asked for scores of a scan without a text bridge", s->in.indexoid);
	s->score_col = i;
	alloc_column(s, i, sizeof(double));
}

static void
bind_columns(SiftV1Src *s)
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
bind_elems(SiftV1Src *s, const SiftV1ElemSpec *spec)
{
	s->elems = sift_v1_elems_new(s->cx, spec, s->lay);
	s->elem_group = read_slot(s, &s->lay->leaf[spec->group_leaf]);
	s->nelem_child = spec->nchild;
	for (int c = 0; c < spec->nchild; c++)
		s->elem_child[c] = read_slot(s, &s->lay->leaf[spec->child_leaf[c]]);
}

static void
bind_types(SiftV1Src *s, const SiftV1Scan *scan)
{
	SiftSrcStats st;

	memcpy(s->types, scan->types, scan->ntypes * sizeof(SiftV1TypeTest));
	s->ntypes = scan->ntypes;
	for (int t = 0; t < s->ntypes; t++)
		if (s->types[t].key)
		{
			s->rd->stats(s->rels.rel, &st);
			s->types[t].rootless = st.rootless[s->types[t].leaf] > 0;
		}
}

typedef struct SrcKeys
{
	const SiftV1Src *s;
	SiftSrcBound bound[SIFT_SRC_MAX_BOUNDS];
	int			nbound;
} SrcKeys;

/* The keys of leaf the bounds of the quals admit (SiftColLeafKeys): those of its integer bounds, the conjuncts the units are pruned by */
static bool
bound_keys(int leaf, const void *arg, int64 *lo, int64 *hi, bool *null)
{
	const SrcKeys *k = arg;
	bool		found = false;

	*lo = PG_INT64_MIN;
	*hi = PG_INT64_MAX;
	*null = false;
	for (int i = 0; i < k->nbound; i++)
	{
		const SiftSrcLeaf *l = k->s->col_leaf[k->bound[i].slot];

		if (l == NULL || l->id != leaf || l->wide || (l->vtype != SCV_I32 && l->vtype != SCV_I64))
			continue;
		*lo = k->bound[i].has_lo ? Max(*lo, k->bound[i].lo) : *lo;
		*hi = k->bound[i].has_hi ? Min(*hi, k->bound[i].hi) : *hi;
		found = true;
	}
	return found;
}

/* The reader of the source: an index read by the statement alone opens only the partitions of its key_cut the bounds of the quals may find rows in, a worker the directory of its leader */
static void *
open_reader(SiftV1Src *s, const SiftV1Range *range)
{
	SrcKeys		k = {.s = s};
	SiftKeyRange keys;

	if (s->in.is_table)
		return s->rd->open(s->rels.heap, NULL, s->snap, s->cx, range ? range->view : NULL);
	k.nbound = s->quals != NULL && range == NULL ? sift_srcfold_bounds(s->quals, k.bound) : 0;
	return sift_v1_idx_open_keys(s->rels.heap, s->rels.rel, s->snap, s->cx, range ? range->view : NULL,
								 k.nbound > 0 && sift_col_key_range_by(s->rels.rel, bound_keys, &k, &keys) ? &keys : NULL);
}

SiftV6Source *
sift_v1_source_open(MemoryContext parent, Snapshot snap, const SiftV6Input *in, const SiftV6Vexpr *quals, const SiftV1Scan *scan)
{
	const SiftV1Scan none = {0};
	const SiftV1Range *range;
	MemoryContext cx = sift_mem_allocset_create(parent, "sift v1 source");
	SiftV1Src	   *s = MemoryContextAllocZero(cx, sizeof(SiftV1Src));
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftErrCtx	ctx;

	sift_errctx_push(&ctx, in->indexoid);
	scan = scan != NULL ? scan : &none;
	range = scan->range;
	s->cx = cx;
	s->unit_cx = sift_mem_allocset_create(cx, "sift v1 source unit");
	s->batch_cx = sift_mem_allocset_create(cx, "sift v1 source batch");
	s->in = *in;
	s->snap = snap;
	s->bridge = scan->bridge;
	s->cube = scan->cube;
	s->tid_range = scan->tid_range;
	s->tid_lo = scan->tid_lo;
	s->tid_hi = scan->tid_hi;
	s->rels = sift_v1_rel_open(in->indexoid, in->is_table);
	s->rd = sift_v1_reader_of(in->is_table);
	s->lay = s->rd->layout(in->is_table ? s->rels.heap : s->rels.rel);
	bind_columns(s);
	if (scan->elem != NULL)
		bind_elems(s, scan->elem);
	if (quals != NULL)
		s->quals = sift_v6_vexpr_dup(cx, quals);
	s->src = open_reader(s, range);
	s->fslot = table_slot_create(s->rels.heap, NULL);
	if (!in->is_table)
		s->index_fetch = sift_table_index_fetch_begin(s->rels.heap);
	s->vctx = sift_vexpr_ctx_new(cx);
	if (scan->keep != NULL)
		s->rd->select(s->src, scan->keep);
	bind_types(s, scan);
	sift_v1_src_prune_begin(s, scan);
	sift_errctx_pop(&ctx);
	MemoryContextSwitchTo(old);
	s->base = (SiftV6Source) {.next = v1_next,.fetch = sift_v1_src_fetch,.close = v1_close,.granules = v1_granules};
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
	.hybrid = &v1_hybrid_ops,
	.conn = &sift_v1_conn_ops,
	.scope = &sift_v1_scope_ops
};

void
sift_v1_store_ops_init(void)
{
	sift_store_ops_register(&v1_store_ops);
}
