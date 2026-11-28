/*
 * table/v1/reader_read.c - the reads of the unit reader of a sift_table: the vectors of a unit's leaves, the default of a column that older segments lack,
 * and the dictionary of the exact datum images of a wide leaf (col/v1/reader.h)
 */
/* check-memory: allow-allocs every array here follows the rows of one unit (at most SIFT_COL_UNIT_MAX_ROWS, 74,496) or of one chunk, not the rows of the table */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/col.h"
#include "col/v1/reader.h"
#include "core/limits.h"
#include "table/v1/table.h"
#include "table/v1/reader.h"

/* The dictionary of the text default of a column that older segments lack (one entry; none when the default is NULL); its id is stable for the life of the source */
static const SiftSrcDict *
absent_dict(SiftV1Tbl *t, int leaf, Datum d, bool isnull)
{
	SiftSrcDict *dict = &t->adict[leaf];

	if (dict->id == NULL)
	{
		text	   *tx = isnull ? NULL : DatumGetTextPP(d);
		uint32	   *off = MemoryContextAllocZero(t->cxt, 2 * sizeof(uint32));
		char	   *bytes = MemoryContextAlloc(t->cxt, (tx ? VARSIZE_ANY_EXHDR(tx) : 0) + 1);

		if (tx)
		{
			off[1] = VARSIZE_ANY_EXHDR(tx);
			memcpy(bytes, VARDATA_ANY(tx), off[1]);
		}
		*dict = (SiftSrcDict) {dict, tx != NULL, off, bytes};
	}
	return dict;
}

/* The integer key of the catalog's value d of an integer leaf (SCV_I32/SCV_I64): an enum is its OID */
static int64
absent_int(const SiftSrcLeaf *l, Datum d)
{
	switch (l->typid)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(d);
		case BOOLOID:
			return DatumGetBool(d);
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case TIMEOID:
		case CASHOID:
			return DatumGetInt64(d);
		case NUMERICOID:
			return sift_col_numeric_key(l->scale, d, SIFT_COL_ROUND_NEAREST);
		default:
			return (int32) DatumGetObjectId(d);
	}
}

/* Per leaf of the layout: the value a column the segment was written before holds (null, or the integer constant of ADD COLUMN .. DEFAULT), which reads and zone tests share */
SiftSrcConst *
sift_v1_tbl_absent(Relation rel, const SiftSrcLayout *lay)
{
	SiftSrcConst *c = palloc0(Max(lay->nleaves, 1) * sizeof(SiftSrcConst));

	for (int i = 0; i < lay->nleaves; i++)
	{
		const SiftSrcLeaf *l = &lay->leaf[i];
		Datum		d = getmissingattr(RelationGetDescr(rel), l->attno, &c[i].isnull);

		if (!c[i].isnull && !l->wide && (l->vtype == SCV_I32 || l->vtype == SCV_I64))
			c[i].v = absent_int(l, d);
	}
	return c;
}

/* A column the segment was written before has the value the catalog gives rows that lack it (null, or the constant of ADD COLUMN .. DEFAULT) */
static void
read_absent(SiftV1Tbl *t, int leaf, size_t n, SiftSrcVec *vec)
{
	const SiftSrcLeaf *l = &t->lay->leaf[leaf];
	bool		isnull;
	Datum		d = getmissingattr(RelationGetDescr(t->rel), l->attno, &isnull);
	SiftSrcLeafBuf *b;

	if (l->wide)
	{
		sift_v1_tbl_wide_read(t, leaf, NULL, NULL, n, d, isnull, vec);
		return;
	}
	b = &t->buf[leaf];
	sift_srcsel_leaf_buf(t->cxt, b, n);
	memset(b->data, 0, (n + SIFT_SRC_PAD) * sift_src_vtype_size[l->vtype]);
	*vec = (SiftSrcVec) {.data = b->data,.n = n};
	if (isnull)
	{
		memset(b->isnull, 1, n);
		vec->isnull = b->isnull;
		if (l->vtype == SCV_CODE)
			vec->dict = absent_dict(t, leaf, d, true);
		return;
	}
	for (size_t i = 0; i < n; i++)
		if (l->vtype == SCV_I32)
			((int32 *) b->data)[i] = (int32) t->absent[leaf].v;
		else if (l->vtype == SCV_I64)
			((int64 *) b->data)[i] = t->absent[leaf].v;
		else if (l->typid == FLOAT4OID)
			((double *) b->data)[i] = DatumGetFloat4(d);
		else if (l->typid == FLOAT8OID)
			((double *) b->data)[i] = DatumGetFloat8(d);
	if (l->vtype == SCV_CODE)
		vec->dict = absent_dict(t, leaf, d, false);
}

/* A wide leaf is read through a dictionary of its datum images, unless the executor wants the codes of the stored dictionary (set_raw) and the segment has one (an older layout holds the images inline) */
static void
read_vec(SiftV1Tbl *t, int leaf, const TblPart *p, const SiftColVec *v, SiftSrcVec *vec)
{
	if (t->lay->leaf[leaf].wide && !(t->raw[leaf] && v->dict != NULL))
		sift_v1_tbl_wide_read(t, leaf, p, v, v->n, (Datum) 0, false, vec);
	else
	{
		SiftSrcLeafBuf *b = &t->buf[leaf];

		sift_srcsel_leaf_buf(t->cxt, b, v->n);
		sift_srcsel_fill(&t->sel, &t->lay->leaf[leaf], v, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, &t->dict[leaf], vec);
	}
}

void
sift_v1_tbl_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	SiftV1Tbl  *t = impl;
	TblPart    *p = unit->priv;
	SiftColVec	v;

	if (p->col[leaf] < 0)
	{
		read_absent(t, leaf, unit->nrow, vec);
		return;
	}
	if (!t->lay->leaf[leaf].wide && !t->sel.compact)
	{
		/* decoded into the leaf's buffer in its own vector type, with no vector of keys to convert and copy */
		SiftSrcLeafBuf *b = &t->buf[leaf];

		sift_srcsel_leaf_buf(t->cxt, b, sift_col_lane_n(&p->unit, p->col[leaf]));

		sift_srcsel_read_lane(&t->sel, &p->unit, p->col[leaf], &t->lay->leaf[leaf], &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, &t->dict[leaf], vec);
		return;
	}
	sift_srcsel_read(&t->sel, &p->unit, p->col[leaf], &v);
	read_vec(t, leaf, p, &v, vec);
}


void
sift_v1_tbl_read_rows(void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec)
{
	SiftV1Tbl  *t = impl;
	TblPart    *p = unit->priv;
	uint32	   *ords;
	SiftColVec	v;

	if (p->col[leaf] < 0)
	{
		read_absent(t, leaf, n, vec);
		return;
	}
	ords = sift_srcsel_ords(&t->sel, rows, n);
	sift_col_read_idx(&p->unit, p->col[leaf], ords, n, &v);
	read_vec(t, leaf, p, &v, vec);
	pfree(ords);
}

/* Only the rows of bits, at their rows in the leaf's buffer; the unit as stored, a column the segment lacks and a wide leaf are read whole */
void
sift_v1_tbl_read_sel(void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec)
{
	SiftV1Tbl  *t = impl;
	TblPart    *p = unit->priv;
	SiftColSel	sel;
	SiftColVec	v;
	uint32	   *idx;
	SiftSrcLeafBuf *b = &t->buf[leaf];

	if (t->sel.compact || p->col[leaf] < 0 || t->lay->leaf[leaf].wide)
	{
		sift_v1_tbl_read(impl, unit, leaf, vec);
		return;
	}
	sift_srcsel_bits(p->src, unit->nrow, bits, &sel);
	idx = palloc((sel.count + SIFT_COL_IDXPAD) * sizeof(uint32));
	sift_col_sel_idx(&sel, idx);
	sift_col_read(&p->unit, p->col[leaf], &sel, &v);
	sift_srcsel_leaf_buf(t->cxt, b, unit->nrow);
	sift_srcsel_scatter(&t->lay->leaf[leaf], &v, idx, unit->nrow, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, &t->dict[leaf], vec);
	pfree(idx);
}


/* ---- a wide leaf: the dictionary of the exact datum images of the rows read, an entry the datum itself (agg/wide.h) ---- */

typedef struct WideBuild
{
	const char *col;			/* for the error of a dictionary that outgrows its limit */
	uint32		nd;
	uint32		dcap;
	uint32	   *off;			/* nd + 1: the entries start on 8 bytes */
	uint32	   *len;			/* nd: the size of the image */
	char	   *bytes;
	size_t		bcap;
	uint32		nslot;
	uint32	   *slot;			/* the entry + 1 of a hash of an image */
} WideBuild;

/* the entry of an image, added when new */
static uint32
wide_entry(WideBuild *w, const char *img, uint32 len)
{
	uint32		mask = w->nslot - 1,
				s = hash_bytes((const unsigned char *) img, len) & mask,
				e;
	size_t		end;

	for (; (e = w->slot[s]) != 0; s = (s + 1) & mask)
		if (w->len[e - 1] == len && memcmp(w->bytes + w->off[e - 1], img, len) == 0)
			return e - 1;
	end = (size_t) w->off[w->nd] + MAXALIGN(Max(len, 1));
	if (end > (size_t) sift_limit(wide_dict_bytes, MaxAllocSize))
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("the distinct values of column \"%s\" in the rows of one unit take more than %zu bytes", w->col,
							   (size_t) sift_limit(wide_dict_bytes, MaxAllocSize)),
						errhint("A unit holds up to %d rows: values of a wide column that are that large and distinct cannot be read together.",
								SIFT_COL_UNIT_MAX_ROWS)));
	if (w->nd == w->dcap)
	{
		w->dcap *= 2;
		w->off = repalloc(w->off, (w->dcap + 1) * sizeof(uint32));
		w->len = repalloc(w->len, w->dcap * sizeof(uint32));
	}
	if (end > w->bcap)
	{
		w->bcap = Max(end, w->bcap * 2);
		w->bytes = repalloc(w->bytes, w->bcap);
	}
	memcpy(w->bytes + w->off[w->nd], img, len);
	w->len[w->nd] = len;
	w->off[w->nd + 1] = end;
	w->slot[s] = ++w->nd;
	return w->nd - 1;
}

/* the image of a datum of a wide leaf: a varlena in its plain form, or the bytes of a fixed size */
static const char *
datum_image(Datum d, int16 typlen, uint32 *len)
{
	if (typlen > 0)
	{
		*len = typlen;
		return DatumGetPointer(d);
	}
	d = PointerGetDatum(PG_DETOAST_DATUM(d));
	*len = VARSIZE(DatumGetPointer(d));
	return DatumGetPointer(d);
}

/*
 * The leaf of the n rows `v` holds (NULL: the rows of a column the segment lacks, all the default `dflt`) as codes into a dictionary of their images,
 * made in a context of its own that the next read of the leaf resets: the dictionary and codes are valid until then.
 */
void
sift_v1_tbl_wide_read(SiftV1Tbl *t, int leaf, const TblPart *p, const SiftColVec *v, size_t n, Datum dflt, bool dnull, SiftSrcVec *vec)
{
	const SiftSrcLeaf *l = &t->lay->leaf[leaf];
	int16		typlen = TupleDescAttr(RelationGetDescr(t->rel), l->attno - 1)->attlen;
	SiftSrcLeafBuf *b = &t->buf[leaf];
	MemoryContext old;
	WideBuild	w = {.col = NameStr(TupleDescAttr(RelationGetDescr(t->rel), l->attno - 1)->attname)};
	bool		has_null = false;

	sift_srcsel_leaf_buf(t->cxt, b, n);
	if (t->wcxt[leaf] == NULL)
		t->wcxt[leaf] = AllocSetContextCreate(t->cxt, "sift table wide leaf", ALLOCSET_DEFAULT_SIZES);
	MemoryContextReset(t->wcxt[leaf]);
	old = MemoryContextSwitchTo(t->wcxt[leaf]);
	w.dcap = 64;
	w.off = palloc0((w.dcap + 1) * sizeof(uint32));
	w.len = palloc(w.dcap * sizeof(uint32));
	w.bcap = 4096;
	w.bytes = palloc(w.bcap);
	for (w.nslot = 64; w.nslot < 2 * Max(n, 1); w.nslot *= 2)
		;
	w.slot = palloc0(w.nslot * sizeof(uint32));
	for (size_t i = 0; i < n; i++)
	{
		bool		isnull = v ? v->nulls && sift_src_bit(v->nulls, i) : dnull;
		uint32		len;
		const char *img;

		((uint32 *) b->data)[i] = 0;
		b->isnull[i] = isnull;
		if (isnull)
		{
			has_null = true;
			continue;
		}
		img = datum_image(v ? sift_tbl_leaf_datum(t->rel, &p->schema->cols[p->col[leaf]], v, i) : dflt, typlen, &len);
		((uint32 *) b->data)[i] = wide_entry(&w, img, len);
	}
	MemoryContextSwitchTo(old);
	memset((uint32 *) b->data + n, 0, SIFT_SRC_PAD * sizeof(uint32));
	t->dict[leaf] = (SiftSrcDict) {(const void *) (uintptr_t) (UINT64CONST(0x7000000000000000) + ++t->wseq), w.nd, w.off, w.bytes};
	*vec = (SiftSrcVec) {.data = b->data,.isnull = has_null ? b->isnull : NULL,.dict = &t->dict[leaf],.n = n};
}
