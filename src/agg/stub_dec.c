/*
 * agg/stub_dec.c - the dictionary and the decoding of the index datums of a stub unit into the vectors of a leaf
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <math.h>

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "storage/bufmgr.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"

#include "agg/kern.h"
#include "agg/stub.h"
#include "core/cmp.h"
#include "core/visibility.h"

#include "agg/stub_src_int.h"

/* ---------------------------------------------------------------------------
 * Dictionaries: codes in first-seen order, open addressing over the values
 * ------------------------------------------------------------------------- */

typedef struct StubDict
{
	MemoryContext cxt;
	uint32		n;
	uint32		cap;			/* entries off has room for, less one */
	uint32	   *off;
	char	   *bytes;
	Size		nbytes;
	Size		bcap;
	int32	   *slot;			/* code, or -1 */
	uint32		mask;
} StubDict;

static void
dict_init(MemoryContext cxt, StubDict *d)
{
	d->cxt = cxt;
	d->n = 0;
	d->cap = 64;
	d->off = MemoryContextAlloc(cxt, sizeof(uint32) * (d->cap + 1));
	d->off[0] = 0;
	d->nbytes = 0;
	d->bcap = 1024;
	d->bytes = MemoryContextAlloc(cxt, d->bcap);
	d->mask = 127;
	d->slot = MemoryContextAlloc(cxt, sizeof(int32) * (d->mask + 1));
	memset(d->slot, -1, sizeof(int32) * (d->mask + 1));
}

static uint32
dict_hash(const StubDict *d, uint32 code)
{
	return hash_bytes((const unsigned char *) d->bytes + d->off[code], d->off[code + 1] - d->off[code]);
}

static void
dict_rehash(StubDict *d)
{
	pfree(d->slot);
	d->mask = d->mask * 2 + 1;
	d->slot = MemoryContextAlloc(d->cxt, sizeof(int32) * (d->mask + 1));
	memset(d->slot, -1, sizeof(int32) * (d->mask + 1));
	for (uint32 c = 0; c < d->n; c++)
	{
		uint32		i = dict_hash(d, c) & d->mask;

		while (d->slot[i] >= 0)
			i = (i + 1) & d->mask;
		d->slot[i] = c;
	}
}

static uint32
dict_code(StubDict *d, const char *s, int len)
{
	uint32		i = hash_bytes((const unsigned char *) s, len) & d->mask;
	int32		c;

	for (; (c = d->slot[i]) >= 0; i = (i + 1) & d->mask)
		if (d->off[c + 1] - d->off[c] == (uint32) len && memcmp(d->bytes + d->off[c], s, len) == 0)
			return c;
	if (d->n == d->cap)
		d->off = repalloc(d->off, sizeof(uint32) * ((d->cap *= 2) + 1));
	while (d->nbytes + len > d->bcap)
		d->bytes = repalloc(d->bytes, d->bcap *= 2);
	memcpy(d->bytes + d->nbytes, s, len);
	d->nbytes += len;
	d->slot[i] = d->n;
	d->off[++d->n] = d->nbytes;
	if (d->n * 2 > d->mask)
		dict_rehash(d);
	return d->n - 1;
}

typedef struct StubVal
{
	bool		isnull;
	int64		i;
	double		f;
	const char *s;
	int			len;
} StubVal;

typedef struct StubOut
{
	SiftSrcVecType vt;
	int64		n;
	void	   *data;
	uint8	   *nul;
	bool		anynull;
	StubDict	dict;
} StubOut;

static const int vtype_size[] = {sizeof(int32), sizeof(int64), sizeof(double), sizeof(uint32)};

void *
alloc_vec(MemoryContext cxt, Size bytes)
{
	return (void *) TYPEALIGN(SIFT_SIMD_ALIGN, MemoryContextAllocZero(cxt, bytes + SIFT_AGG_PAD + SIFT_SIMD_ALIGN));
}

static const void *
dict_id(StubDecode *dc)
{
	if (dc->idleft == 0)
	{
		dc->idnext = MemoryContextAlloc(dc->idcxt, 1024);
		dc->idleft = 1024;
	}
	dc->idleft--;
	return dc->idnext++;
}

static void
out_init(StubDecode *dc, StubOut *o, SiftSrcVecType vt, int64 n)
{
	o->vt = vt;
	o->n = n;
	o->data = alloc_vec(dc->cxt, n * vtype_size[vt]);
	o->nul = alloc_vec(dc->cxt, n);
	o->anynull = false;
	if (vt == SCV_CODE)
		dict_init(dc->cxt, &o->dict);
}

static void
out_put(StubOut *o, int64 idx, const StubVal *v)
{
	if (v->isnull)
	{
		o->nul[idx] = 1;
		o->anynull = true;
		return;
	}
	switch (o->vt)
	{
		case SCV_I32:
			((int32 *) o->data)[idx] = (int32) v->i;
			break;
		case SCV_I64:
			((int64 *) o->data)[idx] = v->i;
			break;
		case SCV_F64:
			((double *) o->data)[idx] = v->f;
			break;
		case SCV_CODE:
			((uint32 *) o->data)[idx] = dict_code(&o->dict, v->s, v->len);
			break;
	}
}

static void
out_finish(StubDecode *dc, StubOut *o, StubVecSlot *s)
{
	s->vec.data = o->data;
	s->vec.isnull = o->anynull ? o->nul : NULL;
	s->vec.n = o->n;
	s->vec.dict = NULL;
	dc->bytes += o->n * vtype_size[o->vt] + (o->anynull ? o->n : 0);
	if (o->vt == SCV_CODE)
	{
		s->dict.id = dict_id(dc);
		s->dict.n = o->dict.n;
		s->dict.off = o->dict.off;
		s->dict.bytes = o->dict.bytes;
		s->vec.dict = &s->dict;
		dc->bytes += o->dict.nbytes + sizeof(uint32) * (o->dict.n + 1);
	}
	s->done = true;
}

/* A value of the type of the plain attribute */
static void
val_from_datum(Oid typid, Datum d, StubVal *v)
{
	switch (typid)
	{
		case INT2OID:
			v->i = DatumGetInt16(d);
			break;
		case INT4OID:
			v->i = DatumGetInt32(d);
			break;
		case DATEOID:
			v->i = DatumGetDateADT(d);
			break;
		case BOOLOID:
			v->i = DatumGetBool(d);
			break;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			v->i = DatumGetInt64(d);
			break;
		case FLOAT4OID:
			v->f = DatumGetFloat4(d);
			break;
		case FLOAT8OID:
			v->f = DatumGetFloat8(d);
			break;
		case NAMEOID:
			v->s = NameStr(*DatumGetName(d));
			v->len = strlen(v->s);
			break;
		default:				/* text, varchar, char(n) */
			{
				struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

				v->s = VARDATA_ANY(t);
				v->len = VARSIZE_ANY_EXHDR(t);
			}
	}
}

/*
 * The JSON value converted to the leaf's type as `(x #>> '{}')::type` does: its text, then the input function of the
 * type. NULL for an absent value and a JSON null.
 */
static void
val_from_json(StubDecode *dc, const SiftSrcLeaf *lf, const StubLeafX *x, const JsonbValue *jv, StubVal *v)
{
	char	   *str;
	Datum		d;

	if (!jv || jv->type == jbvNull)
	{
		v->isnull = true;
		return;
	}
	if (lf->typid == TEXTOID && jv->type == jbvString)
	{
		v->s = jv->val.string.val;
		v->len = jv->val.string.len;
		return;
	}
	switch (jv->type)
	{
		case jbvString:
			str = pnstrdup(jv->val.string.val, jv->val.string.len);
			break;
		case jbvNumeric:
			str = DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(jv->val.numeric)));
			break;
		case jbvBool:
			str = pstrdup(jv->val.boolean ? "true" : "false");
			break;
		default:
			str = JsonbToCString(NULL, jv->val.binary.data, jv->val.binary.len);
	}
	if (lf->typid == TEXTOID)
	{
		v->s = str;
		v->len = strlen(str);
		return;
	}
	if (dc->esc)
		dc->esc->error_occurred = false;
	if (!InputFunctionCallSafe((FmgrInfo *) &x->infn, str, x->ioparam, -1, (Node *) dc->esc, &d))
		v->isnull = true;
	else
		val_from_datum(lf->typid, d, v);
}

static void
val_finish(const StubDecode *dc, const SiftSrcLeaf *lf, StubVal *v)
{
	if (lf->vtype != SCV_CODE || v->isnull || v->len <= SIFT_COLSTUB_MAX_STR)
		return;
	if (!dc->esc)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("sift_colstub: a text value of %d bytes exceeds the limit of %d bytes", v->len, SIFT_COLSTUB_MAX_STR),
						errhint("Index left(column, %d) or a hash of it instead.", SIFT_COLSTUB_MAX_STR)));
	v->isnull = true;
}

/* The jsonb value at the object keys of path below c (c itself for NIL); false if the path leads nowhere */
static bool
json_walk(JsonbContainer *c, List *path, JsonbValue *out)
{
	out->type = jbvBinary;
	out->val.binary.data = c;
	out->val.binary.len = 0;
	foreach_ptr(String, key, path)
	{
		if (out->type != jbvBinary || !JsonContainerIsObject(out->val.binary.data))
			return false;
		if (!getKeyJsonValueFromContainer(out->val.binary.data, strVal(key), strlen(strVal(key)), out))
			return false;
	}
	return true;
}

/* The array (object) a group leaf describes in the jsonb datum d; NULL: absent */
static JsonbContainer *
group_container(const SiftSrcLeaf *g, Datum d)
{
	JsonbValue	jv;
	JsonbContainer *c;

	if (!json_walk(&DatumGetJsonbP(d)->root, g->path, &jv) || jv.type != jbvBinary)
		return NULL;
	c = jv.val.binary.data;
	if (g->kind == SCL_OBJECT ? !JsonContainerIsObject(c) : (!JsonContainerIsArray(c) || JsonContainerIsScalar(c)))
		return NULL;
	return c;
}

static Datum
unit_datum(const StubUnit *u, int row, int attno, bool *isnull)
{
	*isnull = u->nulls[row * u->natts + attno - 1];
	return u->vals[row * u->natts + attno - 1];
}

static void
decode_scalars(StubDecode *dc, StubUnit *u, const SiftSrcLeaf *lf, StubOut *o)
{
	const StubLeafX *x = &dc->lay->x[lf->id];

	out_init(dc, o, lf->vtype, u->nrow);
	for (int r = 0; r < u->nrow; r++)
	{
		StubVal		v = {0};
		bool		isnull;
		Datum		d = unit_datum(u, r, lf->attno, &isnull);
		JsonbValue	jv;

		MemoryContextReset(dc->tmp);
		if (isnull)
			v.isnull = true;
		else if (lf->path == NIL)
			val_from_datum(lf->typid, d, &v);
		else if (!json_walk(&DatumGetJsonbP(d)->root, lf->path, &jv))
			v.isnull = true;
		else
			val_from_json(dc, lf, x, &jv, &v);
		val_finish(dc, lf, &v);
		out_put(o, r, &v);
	}
}

static void
decode_counts(StubDecode *dc, StubUnit *u, const SiftSrcLeaf *lf, StubOut *o)
{
	out_init(dc, o, SCV_I32, u->nrow);
	for (int r = 0; r < u->nrow; r++)
	{
		StubVal		v = {0};
		bool		isnull;
		Datum		d = unit_datum(u, r, lf->attno, &isnull);
		JsonbContainer *c;

		MemoryContextReset(dc->tmp);
		c = isnull ? NULL : group_container(lf, d);
		v.isnull = !c;
		v.i = c ? JsonContainerSize(c) : 0;
		out_put(o, r, &v);
	}
}

/* The n elements of the group container c, the values of the child leaf lf */
static void
decode_container(StubDecode *dc, const SiftSrcLeaf *g, const SiftSrcLeaf *lf, JsonbContainer *c, int n, StubOut *o, int64 *idx)
{
	const StubLeafX *x = &dc->lay->x[lf->id];
	JsonbIterator *it = JsonbIteratorInit(c);
	JsonbValue	jv;
	JsonbValue	fv;
	JsonbIteratorToken tok = JsonbIteratorNext(&it, &jv, true);
	int			seen = 0;

	for (; seen < n; seen++, (*idx)++)
	{
		StubVal		v = {0};

		if (g->kind == SCL_ARRAY)
		{
			bool		isobj;

			tok = JsonbIteratorNext(&it, &jv, true);
			isobj = tok == WJB_ELEM && jv.type == jbvBinary && JsonContainerIsObject(jv.val.binary.data);
			if (tok != WJB_ELEM)
				break;
			val_from_json(dc, lf, x, isobj && getKeyJsonValueFromContainer(jv.val.binary.data, lf->name, x->namelen, &fv) ? &fv : NULL, &v);
		}
		else
		{
			tok = JsonbIteratorNext(&it, &jv, true);
			if (tok != WJB_KEY)
				break;
			if (x->role == STUB_ROLE_KEY)
			{
				v.s = jv.val.string.val;
				v.len = jv.val.string.len;
			}
			(void) JsonbIteratorNext(&it, &fv, true);
			if (x->role == STUB_ROLE_PAIR)
				val_from_json(dc, lf, x, &fv, &v);
		}
		val_finish(dc, lf, &v);
		out_put(o, *idx, &v);
	}
	if (seen != n)
		elog(ERROR, "sift_colstub: group of %d elements has %d", n, seen);
}

static void
decode_elems(StubDecode *dc, StubUnit *u, const SiftSrcLeaf *lf, StubOut *o)
{
	const SiftSrcLeaf *g = &dc->lay->l.leaf[lf->parent];
	const SiftSrcVec *cv = &u->slot[g->id].vec;
	const int32 *cnt = cv->data;
	int64		total = 0;
	int64		idx = 0;

	for (int r = 0; r < u->nrow; r++)
		total += (cv->isnull && cv->isnull[r]) ? 0 : cnt[r];
	out_init(dc, o, lf->vtype, total);
	for (int r = 0; r < u->nrow; r++)
	{
		bool		isnull;
		Datum		d = unit_datum(u, r, lf->attno, &isnull);

		if ((cv->isnull && cv->isnull[r]) || cnt[r] == 0)
			continue;
		MemoryContextReset(dc->tmp);
		decode_container(dc, g, lf, group_container(g, d), cnt[r], o, &idx);
	}
}

const StubVecSlot *
leaf_slot(StubDecode *dc, StubUnit *u, int leaf)
{
	StubVecSlot *s = &u->slot[leaf];
	const SiftSrcLeaf *lf = &dc->lay->l.leaf[leaf];
	StubOut		o;
	MemoryContext old;

	if (s->done)
		return s;
	if (lf->parent >= 0)
		leaf_slot(dc, u, lf->parent);
	old = MemoryContextSwitchTo(dc->tmp);
	if (lf->parent >= 0)
		decode_elems(dc, u, lf, &o);
	else if (lf->kind == SCL_SCALAR)
		decode_scalars(dc, u, lf, &o);
	else
		decode_counts(dc, u, lf, &o);
	MemoryContextSwitchTo(old);
	out_finish(dc, &o, s);
	return s;
}

void
sift_aggstub_check_row(const StubLayout *lay, const Datum *values, const bool *isnull)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift_colstub check", ALLOCSET_SMALL_SIZES);
	StubDecode	dc = {.lay = lay, .cxt = cxt, .idcxt = cxt,
					  .tmp = AllocSetContextCreate(cxt, "sift_colstub check row", ALLOCSET_SMALL_SIZES)};
	StubUnit   *u = MemoryContextAllocZero(cxt, sizeof(StubUnit));

	u->nrow = 1;
	u->natts = lay->l.natts;
	u->vals = (Datum *) values;
	u->nulls = (bool *) isnull;
	for (int leaf = 0; leaf < lay->l.nleaves; leaf++)
		leaf_slot(&dc, u, leaf);
	MemoryContextDelete(cxt);
}

#endif							/* SIFT_TESTING */
