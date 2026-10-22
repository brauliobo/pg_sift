/*
 * agg/leaf.c - reading the leaves of a unit and mapping their values: conversions, dictionaries, unit vectors, id tables, numeric
 * programs
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "portability/instr_time.h"
#include "parser/parse_agg.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/expandeddatum.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/wide.h"
#include "agg/back.h"
#include "agg/kern.h"
#include "agg/part.h"
#include "agg/spec.h"
#include "col/kern.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/topn.h"
#include "core/visibility.h"
#include "core/popcount.h"
#include "agg/agg_int.h"

/* ---------- distinct output values of a key ---------- */

void
ot_init(OutTable *t, Oid eqop, Oid typid, Oid collid)
{
	RegProcedure lhs,
				rhs;

	if (!get_op_hash_functions(eqop, &lhs, &rhs))
	{
		if (get_opcode(eqop) != F_CASH_EQ)
			elog(ERROR, "sift: no hash function for operator %u", eqop);
		lhs = F_HASHINT8;		/* money has no hash operator class, and is its int64 */
	}
	fmgr_info(lhs, &t->hashfn);
	fmgr_info(get_opcode(eqop), &t->eqfn);
	get_typlenbyval(typid, &t->len, &t->byval);
	t->collid = collid;
	t->nslot = 64;
	t->slot = palloc0(t->nslot * sizeof(uint32));
	t->cap = 16;
	t->val = palloc(t->cap * sizeof(Datum));
	t->hv = palloc(t->cap * sizeof(uint32));
}

/* a table of the distinct byte images of values of any type: the equal values of the type that differ (1.0 and 1.00) keep their ids */
void
ot_init_image(OutTable *t, Oid typid)
{
	get_typlenbyval(typid, &t->len, &t->byval);
	t->image = true;
	t->collid = InvalidOid;
	t->nslot = 64;
	t->slot = palloc0(t->nslot * sizeof(uint32));
	t->cap = 16;
	t->val = palloc(t->cap * sizeof(Datum));
	t->hv = palloc(t->cap * sizeof(uint32));
}

static uint32
image_hash(const OutTable *t, Datum d)
{
	if (t->byval)
		return hash_bytes((const unsigned char *) &d, sizeof(Datum));
	if (t->len == -1)
	{
		struct varlena *v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

		return hash_bytes((const unsigned char *) VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
	}
	return hash_bytes((const unsigned char *) DatumGetPointer(d), t->len == -2 ? strlen(DatumGetCString(d)) : t->len);
}

static void
ot_rehash(OutTable *t)
{
	t->nslot *= 2;
	pfree(t->slot);
	t->slot = palloc0(t->nslot * sizeof(uint32));
	for (uint32 id = 1; id <= t->n; id++)
	{
		uint32		h = t->hv[id - 1] & (t->nslot - 1);

		while (t->slot[h])
			h = (h + 1) & (t->nslot - 1);
		t->slot[h] = id;
	}
}

/* the id (1..) of a non-null output value; the value is copied into the current context when new */
uint32
ot_id(OutTable *t, Datum d)
{
	uint32		hv = t->image ? image_hash(t, d) : DatumGetUInt32(FunctionCall1Coll(&t->hashfn, t->collid, d)),
				h = hv & (t->nslot - 1);

	while (t->slot[h])
	{
		uint32		id = t->slot[h];

		if (t->hv[id - 1] == hv && (t->image ? datumIsEqual(t->val[id - 1], d, t->byval, t->len) :
									DatumGetBool(FunctionCall2Coll(&t->eqfn, t->collid, t->val[id - 1], d))))
			return id;
		h = (h + 1) & (t->nslot - 1);
	}
	if (t->n == t->cap)
	{
		t->cap *= 2;
		t->val = repalloc(t->val, t->cap * sizeof(Datum));
		t->hv = repalloc(t->hv, t->cap * sizeof(uint32));
	}
	t->val[t->n] = datumCopy(d, t->byval, t->len);
	t->bytes += t->byval ? 0 : datumGetSize(d, t->byval, t->len);
	t->hv[t->n] = hv;
	t->slot[h] = ++t->n;
	if (t->n * 2 > t->nslot)
		ot_rehash(t);
	return t->n;
}

/* ---------- tables over a range of integer values ---------- */

void
rt_init(RangeTab *r, int esize)
{
	r->lo = 1;
	r->hi = 0;
	r->tab = NULL;
	r->mem = NULL;
	r->esize = esize;
}

void
rt_cover(RangeTab *r, int32 lo, int32 hi, FillFn fill, void *arg)
{
	bool		had = r->lo <= r->hi;
	int32		old_lo = r->lo,
				old_hi = r->hi,
				nlo = had ? Min(r->lo, lo) : lo,
				nhi = had ? Max(r->hi, hi) : hi;
	int64		span = (int64) nhi - nlo + 1;

	if (had && lo >= old_lo && hi <= old_hi)
		return;
	if (span > MAX_RANGE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift: a column range of %lld values is too wide for the aggregate scan", (long long) span),
				 errhint("Set sift.enable_agg to off for this query.")));
	if (r->mem == NULL || nlo < r->plo || nhi > r->phi)
	{
		/* twice the room, on the side the range grows (bounded by MAX_RANGE): a range that creeps along is not copied every time */
		int64		room = Min(span * 2, Max(MAX_RANGE, span)) - span,
					plo = nlo - (had && nlo < old_lo ? room : 0),
					phi = nhi + (had && nlo < old_lo ? 0 : room);
		char	   *nt;

		plo = Max(plo, PG_INT32_MIN);
		phi = Min(phi, PG_INT32_MAX);
		nt = MemoryContextAllocZero(CurrentMemoryContext, (phi - plo + 1) * r->esize + SIFT_AGG_PAD);
		if (had)
			memcpy(nt + (int64) (old_lo - plo) * r->esize, r->tab, ((int64) old_hi - old_lo + 1) * r->esize);
		if (r->mem)
			pfree(r->mem);
		r->mem = nt;
		r->plo = (int32) plo;
		r->phi = (int32) phi;
	}
	r->tab = r->mem + (int64) (nlo - r->plo) * r->esize;
	r->lo = nlo;
	r->hi = nhi;
	for (int64 v = nlo; v <= (had ? old_lo - 1 : nhi); v++)
		fill(arg, (int32) v, (char *) r->tab + (v - nlo) * r->esize);
	for (int64 v = had ? old_hi + 1 : nhi + 1; v <= nhi; v++)
		fill(arg, (int32) v, (char *) r->tab + (v - nlo) * r->esize);
}

Datum
leaf_datum(Oid typid, int64 iv)
{
	switch (typid)
	{
		case BOOLOID:
			return BoolGetDatum(iv != 0);
		case INT2OID:
			return Int16GetDatum((int16) iv);
		case INT4OID:
			return Int32GetDatum((int32) iv);
		case DATEOID:
			return DateADTGetDatum((DateADT) iv);
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case TIMEOID:
			return Int64GetDatum(iv);
		case FLOAT4OID:			/* the bits of a float key (float_key) */
			return Float4GetDatum((float4) float_from_key(iv));
		case FLOAT8OID:
			return Float8GetDatum(float_from_key(iv));
		default:
			if (type_is_enum(typid))
				return ObjectIdGetDatum((Oid) iv);
			elog(ERROR, "sift: unexpected integer leaf type %u", typid);
	}
	return (Datum) 0;
}

/* a dictionary value as the Datum of type typid */
Datum
text_datum(Oid typid, int32 typmod, const char *bytes, uint32 len)
{
	Oid			infn,
				ioparam;
	FmgrInfo	f;

	if (typid == TEXTOID || typid == VARCHAROID)
		return PointerGetDatum(cstring_to_text_with_len(bytes, len));
	if (sift_aggwide_type(typid))
		return sift_aggwide_datum(typid, bytes, len);
	getTypeInputInfo(typid, &infn, &ioparam);
	fmgr_info(infn, &f);
	return InputFunctionCall(&f, pnstrdup(bytes, len), ioparam, typmod);
}

/* ---------- evaluation of the single input expressions ---------- */

static Datum
eval_with(AggExec *ex, ExprState *e, Datum in, bool in_null, bool *isnull)
{
	ex->lutparams[0].value = in;
	ex->lutparams[0].isnull = in_null;
	return ExecEvalExprSwitchContext(e, ex->lutcx, isnull);
}

bool
eval_bool(AggExec *ex, ExprState *e, Datum in, bool in_null)
{
	bool		isnull;
	Datum		r = eval_with(ex, e, in, in_null, &isnull);
	bool		res = !isnull && DatumGetBool(r);

	ResetExprContext(ex->lutcx);
	return res;
}

/* ---------- dictionaries ---------- */

DictSlot *
dict_slot(AggExec *ex, const SiftSrcDict *d)
{
	DictSlot   *s;
	int			np = Max(ex->spec->npreds_total, 1),
				nk = Max(ex->spec->nkeys + ex->spec->naggs + ex->ncl, 1);

	for (int i = 0; i < NDICT_SLOTS; i++)
		if (ex->dslot[i].id == d->id && d->id != NULL)
			return &ex->dslot[i];
	s = &ex->dslot[ex->dnext];
	ex->dnext = (ex->dnext + 1) % NDICT_SLOTS;
	if (s->lut == NULL)
	{
		s->lut = MemoryContextAllocZero(ex->cxt, np * sizeof(uint64 *));
		s->kmap = MemoryContextAllocZero(ex->cxt, nk * sizeof(uint32 *));
	}
	else
	{
		for (int i = 0; i < np; i++)
			if (s->lut[i])
				pfree(s->lut[i]);
		for (int i = 0; i < nk; i++)
			if (s->kmap[i])
				pfree(s->kmap[i]);
		memset(s->lut, 0, np * sizeof(uint64 *));
		memset(s->kmap, 0, nk * sizeof(uint32 *));
	}
	s->id = d->id;
	return s;
}

/* the key id of a distinct input value of a key whose input is a dictionary entry or an integer */
uint32
key_value_id(AggExec *ex, int ki, Datum in, bool in_null)
{
	KeyState   *k = &ex->ks[ki];
	SiftAggKey *sk = &ex->spec->key[ki];
	Datum		out = in;
	bool		isnull = in_null;
	uint32		id;
	MemoryContext old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);

	if (sk->kind == SAK_LUT)
		out = eval_with(ex, k->expr, in, in_null, &isnull);
	if (isnull)
		id = 0;
	else
	{
		MemoryContextSwitchTo(ex->tcxt);
		id = ot_id(&k->ot, out);
	}
	MemoryContextSwitchTo(old);
	ResetExprContext(ex->lutcx);
	return id;
}

uint32 *
dict_kmap(AggExec *ex, DictSlot *s, const SiftSrcDict *d, int ki)
{
	KeyState   *k = &ex->ks[ki];
	uint32	   *map = s->kmap[ki];
	MemoryContext old;

	if (map)
		return map;
	map = MemoryContextAlloc(ex->tcxt, (d->n + 1) * sizeof(uint32) + SIFT_AGG_PAD);
	old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
	for (uint32 c = 0; c < d->n; c++)
	{
		Datum		v = text_datum(k->param->paramtype, k->param->paramtypmod, d->bytes + d->off[c],
								   d->off[c + 1] - d->off[c]);

		map[c] = key_value_id(ex, ki, v, false);
		ResetExprContext(ex->lutcx);
	}
	map[d->n] = key_value_id(ex, ki, (Datum) 0, true);
	MemoryContextSwitchTo(old);
	s->kmap[ki] = map;
	return map;
}

/* ---------- unit data ---------- */

static uint32
dict_code(const SiftSrcDict *d, const char *key)
{
	size_t		len = strlen(key);

	for (uint32 c = 0; c < d->n; c++)
		if (d->off[c + 1] - d->off[c] == len && memcmp(d->bytes + d->off[c], key, len) == 0)
			return c;
	return PG_UINT32_MAX;
}

/* the value of the element of each row that eq marks, esz bytes of it (a constant at the callers) */
static pg_attribute_always_inline void
vleaf_fill(char *data, uint8 *isnull, const SiftSrcVec *val, const uint64 *eq, const uint32 *offs, int nrow, size_t esz)
{
	for (int r = 0; r < nrow; r++)
	{
		int64		e = last_bit(eq, offs[r], offs[r + 1]);

		if (e >= 0)
		{
			memcpy(data + (size_t) r * esz, (const char *) val->data + (size_t) e * esz, esz);
			isnull[r] = val->isnull ? val->isnull[e] : 0;
		}
	}
}

/* the elements of the unit whose key is the one of vleaf j; a row owns at most one (the member names of an object are distinct). *offs: the elements of each row */
uint64 *
member_elems(AggExec *ex, int j, const uint32 **offs)
{
	const SiftSrcVec *key = get_vec(ex, ex->vl[j].key);
	uint32		m;
	uint64	   *eq;

	*offs = elem_offs(ex, ex->spec->vleaf[j].obj);
	m = (*offs)[ex->unit.nrow];
	eq = MemoryContextAlloc(ex->ucxt, (SIFT_COL_NWORDS(m) + 1) * sizeof(uint64));
	if (ex->vl[j].dict != key->dict->id)
	{
		ex->vl[j].dict = key->dict->id;
		ex->vl[j].code = dict_code(key->dict, ex->spec->vleaf[j].key);
	}
	sift_colk->range[SIFT_COLW_32](key->data, m, ex->vl[j].code, 0, eq);
	return eq;
}

/* the leaf of a SiftAggVLeaf with a key: per row the value of its element with the key, NULL where it has none */
static void
read_member_leaf(AggExec *ex, int leaf, SiftSrcVec *out)
{
	int			j = leaf - ex->nbase,
				nrow = ex->unit.nrow;
	const SiftSrcVec *val = get_vec(ex, ex->vl[j].val);
	size_t		esz = ex->lay->leaf[leaf].vtype == SCV_I64 || ex->lay->leaf[leaf].vtype == SCV_F64 ? 8 : 4;
	const uint32 *offs;
	uint64	   *eq = member_elems(ex, j, &offs);
	char	   *data = (char *) TYPEALIGN(SIFT_SIMD_ALIGN, MemoryContextAlloc(ex->ucxt, nrow * esz + SIFT_AGG_PAD + SIFT_SIMD_ALIGN));
	uint8	   *isnull = MemoryContextAlloc(ex->ucxt, nrow + SIFT_AGG_PAD);

	memset(isnull, 1, nrow);
	if (esz == 8)
		vleaf_fill(data, isnull, val, eq, offs, nrow, 8);
	else
		vleaf_fill(data, isnull, val, eq, offs, nrow, 4);
	out->data = data;
	out->isnull = isnull;
	out->dict = val->dict;
	out->n = nrow;
}

/* the names jsonb_typeof gives, as the dictionary of the leaf of a type: the code of a value is its type's index in read_types */
static const char type_names[] = "stringnumberbooleanobjectarraynull";
static const uint32 type_offs[] = {0, 6, 12, 19, 25, 30, 34};
static const char type_dict_id = 0;
static const SiftSrcDict type_dict = {&type_dict_id, lengthof(type_offs) - 1, type_offs, type_names};

/* the leaf of a SiftAggVLeaf with types: per row the code of the JSON type of the value of its scalar leaf, NULL where the path has no value */
static void
read_types_leaf(AggExec *ex, int leaf, SiftSrcVec *out)
{
	int			nrow = ex->unit.nrow;
	uint8	   *tags = MemoryContextAlloc(ex->ucxt, nrow + SIFT_AGG_PAD),
			   *isnull = MemoryContextAlloc(ex->ucxt, nrow + SIFT_AGG_PAD);
	uint32	   *codes = (uint32 *) TYPEALIGN(SIFT_SIMD_ALIGN, MemoryContextAlloc(ex->ucxt, (nrow + SIFT_AGG_PAD) * sizeof(uint32) + SIFT_SIMD_ALIGN));

	sift_colsrc_read_types(ex->src, &ex->unit, ex->spec->vleaf[leaf - ex->nbase].obj, tags);
	for (int r = 0; r < nrow; r++)
	{
		codes[r] = tags[r] == SIFT_SRC_TYPE_NONE ? 0 : tags[r];
		isnull[r] = tags[r] == SIFT_SRC_TYPE_NONE;
	}
	memset(codes + nrow, 0, SIFT_AGG_PAD * sizeof(uint32));
	out->data = codes;
	out->isnull = isnull;
	out->dict = &type_dict;
	out->n = nrow;
	out->ovf = NULL;
}

/* a leaf after the source's: the member of an object by key, the length of an array (the element counts of its leaf are the values), or the JSON type of a path */
static void
read_vleaf(AggExec *ex, int leaf, SiftSrcVec *out)
{
	const SiftAggVLeaf *vl = &ex->spec->vleaf[leaf - ex->nbase];

	if (vl->types)
		read_types_leaf(ex, leaf, out);
	else if (vl->count)
		*out = *get_vec(ex, vl->obj);
	else
		read_member_leaf(ex, leaf, out);
}

const SiftSrcVec *
get_vec(AggExec *ex, int leaf)
{
	if (!ex->uvec_ok[leaf])
	{
		if (leaf >= ex->nbase)
			read_vleaf(ex, leaf, &ex->uvec[leaf]);
		else if (ex->rsel && ex->lay->leaf[leaf].kind == SCL_SCALAR && ex->lay->leaf[leaf].parent < 0)
			sift_colsrc_read_sel(ex->src, &ex->unit, leaf, ex->rsel, &ex->uvec[leaf]);
		else
			sift_colsrc_read(ex->src, &ex->unit, leaf, &ex->uvec[leaf]);
		ex->uvec_ok[leaf] = true;
	}
	return &ex->uvec[leaf];
}

/* row r of the unit owns the elements [offs[r], offs[r + 1]) of the children of the array leaf */
uint32 *
elem_offs(AggExec *ex, int leaf)
{
	if (ex->gofs[leaf] == NULL)
	{
		const SiftSrcVec *cnt = get_vec(ex, leaf);
		const int32 *c = cnt->data;
		int			nrow = ex->unit.nrow;
		uint32	   *offs = MemoryContextAlloc(ex->ucxt, (nrow + 1) * sizeof(uint32)),
					m = 0;

		for (int r = 0; r < nrow; r++)
		{
			offs[r] = m;
			if (!(cnt->isnull && cnt->isnull[r]) && c[r] > 0)
				m += c[r];
		}
		offs[nrow] = m;
		ex->gofs[leaf] = offs;
	}
	return ex->gofs[leaf];
}

/* ---------- numbers ---------- */

void
idmap_init(IdMap *m)
{
	m->cxt = CurrentMemoryContext;
	m->n = 0;
	m->cap = 64;
	m->nslot = 128;
	m->val = palloc(m->cap * sizeof(sift_i128));
	m->slot = palloc0(m->nslot * sizeof(uint32));
}

static inline uint32
idmap_hash(sift_i128 v)
{
	uint64		x = (uint64) v * UINT64CONST(0x9E3779B97F4A7C15) ^ (uint64) (v >> 64) * UINT64CONST(0xC2B2AE3D27D4EB4F);

	x ^= x >> 31;
	return (uint32) ((x * UINT64CONST(0xD6E8FEB86659FD93)) >> 32);
}

uint32
idmap_id(IdMap *m, sift_i128 v)
{
	uint32		mask = m->nslot - 1,
				h = idmap_hash(v) & mask;

	for (; m->slot[h]; h = (h + 1) & mask)
		if (m->val[m->slot[h] - 1] == v)
			return m->slot[h];
	if (m->n == m->cap)
	{
		m->cap *= 2;
		m->val = repalloc(m->val, m->cap * sizeof(sift_i128));
	}
	m->val[m->n] = v;
	m->slot[h] = ++m->n;
	if (m->n * 2 > m->nslot)
	{
		pfree(m->slot);
		m->nslot *= 2;
		m->slot = MemoryContextAllocZero(m->cxt, m->nslot * sizeof(uint32));
		for (uint32 id = 1; id <= m->n; id++)
		{
			for (h = idmap_hash(m->val[id - 1]) & (m->nslot - 1); m->slot[h]; h = (h + 1) & (m->nslot - 1))
				;
			m->slot[h] = id;
		}
	}
	return m->n;
}

/* v / d rounded as the program step asks: ROUND half away from zero as numeric_round, TRUNC toward zero, FLOOR, CEIL */
static sift_i128
num_div(SiftAggNxOp op, sift_i128 v, sift_i128 d)
{
	sift_i128	q = v / d,
				r = v % d,
				ar = r < 0 ? -r : r;

	switch (op)
	{
		case SNX_ROUND:
			return ar >= d - ar ? q + (v < 0 ? -1 : 1) : q;
		case SNX_FLOOR:
			return r < 0 ? q - 1 : q;
		case SNX_CEIL:
			return r > 0 ? q + 1 : q;
		default:
			return q;
	}
}

#define NSTK(i)	(ex->nstk + (Size) (i) * (VEC + PADW))
#define NFLAG(i)	(ex->nflag + (Size) (i) * (VEC + PADW))

/* PostgreSQL's error for the cast of a NaN to an integer of this many bytes */
static void
nan_cast_error(int bytes)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot convert NaN to %s", bytes == 2 ? "smallint" : bytes == 4 ? "integer" : "bigint")));
}

/* the operands of a binary step at the top of the stack (sp entries) become one value: NaN if either was */
static void
nx_pop_flags(AggExec *ex, bool has_nan, int sp, int nsel)
{
	for (int j = 0; has_nan && j < nsel; j++)
		NFLAG(sp - 2)[j] |= NFLAG(sp - 1)[j];
}

/*
 * The values of a numeric program for nsel tuples, scaled by 10^p->scale, a NaN being SIFT_NX_NAN (the greatest value, equal to itself).
 * The planner proved that no step overflows, but the data slot of a NULL leaf holds anything: its tuple's value is garbage and is dropped
 * through the null flags (isnull, when given). iq indexes the data of the leaves at the query's level, ir those of the row level.
 * The NaN flag of each value is kept once a leaf has a NaN in the unit (has_nan). Every step gives NaN for a NaN operand, as numeric_add
 * and its kin do, but two: the comparison (SNX_CMP3) orders NaN as numeric_cmp does, and the cast to an integer raises for a NaN, as
 * numeric_int4 does, when its tuple is in alive (NULL: all of them).
 */
const sift_i128 *
eval_nx(AggExec *ex, const SiftAggNum *p, int nsel, const uint32 *iq, const uint32 *ir, uint8 *isnull, const uint64 *alive)
{
	int			sp = 0;
	bool		has_nan = false;

	if (ex->nstk == NULL)
	{
		ex->nstk = MemoryContextAlloc(ex->cxt, (Size) SIFT_AGG_MAX_VX * (VEC + PADW) * sizeof(sift_i128));
		ex->nflag = MemoryContextAlloc(ex->cxt, (Size) SIFT_AGG_MAX_VX * (VEC + PADW));
	}
	if (isnull)
		memset(isnull, 0, nsel);
	for (int k = 0; k < p->n; k++)
	{
		const SiftAggNx *x = &p->step[k];
		sift_i128  *top = NSTK(sp),
					m,
					d;

		switch (x->op)
		{
			case SNX_LEAF:
				{
					const SiftSrcVec *v = get_vec(ex, x->leaf);
					const uint32 *idx = leaf_lvl(ex, x->leaf) == ex->qlevel ? iq : ir;

					if (ex->lay->leaf[x->leaf].vtype == SCV_I32)
						for (int j = 0; j < nsel; j++)
							top[j] = ((const int32 *) v->data)[idx[j]];
					else
						for (int j = 0; j < nsel; j++)
							top[j] = ((const int64 *) v->data)[idx[j]];
					if (isnull && v->isnull)
						for (int j = 0; j < nsel; j++)
							isnull[j] |= v->isnull[idx[j]];
					if (v->ovf && !has_nan)
					{
						memset(ex->nflag, 0, (Size) SIFT_AGG_MAX_VX * (VEC + PADW));
						has_nan = true;
					}
					if (v->ovf)
						for (int j = 0; j < nsel; j++)
							NFLAG(sp)[j] = sift_colsrc_bit(v->ovf, idx[j]);
					else if (has_nan)
						memset(NFLAG(sp), 0, nsel);
					sp++;
				}
				break;
			case SNX_CONST:
				for (int j = 0; j < nsel; j++)
					top[j] = x->c;
				if (has_nan)
					memset(NFLAG(sp), 0, nsel);
				sp++;
				break;
			case SNX_ADD:
			case SNX_SUB:
				d = sift_pow10(x->a);
				m = x->op == SNX_ADD ? sift_pow10(x->b) : -sift_pow10(x->b);
				for (int j = 0; j < nsel; j++)
					NSTK(sp - 2)[j] = NSTK(sp - 2)[j] * d + NSTK(sp - 1)[j] * m;
				nx_pop_flags(ex, has_nan, sp--, nsel);
				break;
			case SNX_MUL:
				for (int j = 0; j < nsel; j++)
					NSTK(sp - 2)[j] *= NSTK(sp - 1)[j];
				nx_pop_flags(ex, has_nan, sp--, nsel);
				break;
			case SNX_CMP3:
				d = sift_pow10(x->a);
				m = sift_pow10(x->b);
				for (int j = 0; j < nsel; j++)
				{
					sift_i128	l = NSTK(sp - 2)[j] * d,
								r = NSTK(sp - 1)[j] * m;

					NSTK(sp - 2)[j] = has_nan && (NFLAG(sp - 2)[j] || NFLAG(sp - 1)[j]) ? (int) NFLAG(sp - 2)[j] - (int) NFLAG(sp - 1)[j] : (l > r) - (l < r);
					if (has_nan)
						NFLAG(sp - 2)[j] = 0;
				}
				sp--;
				break;
			case SNX_NEG:
			case SNX_ABS:
				for (int j = 0; j < nsel; j++)
					NSTK(sp - 1)[j] = (x->op == SNX_ABS && NSTK(sp - 1)[j] >= 0) ? NSTK(sp - 1)[j] : -NSTK(sp - 1)[j];
				break;
			case SNX_INTCAST:
				for (int j = 0; has_nan && j < nsel; j++)
					if (NFLAG(sp - 1)[j] && (!alive || sift_colsrc_bit(alive, j)))
						nan_cast_error(x->a);
				break;
			default:
				d = sift_pow10(x->a);
				m = sift_pow10(x->b);
				for (int j = 0; j < nsel; j++)
					NSTK(sp - 1)[j] = num_div(x->op, NSTK(sp - 1)[j], d) * m;
				break;
		}
	}
	for (int j = 0; has_nan && j < nsel; j++)
		if (NFLAG(0)[j])
			NSTK(0)[j] = SIFT_NX_NAN;
	return ex->nstk;
}

/* the row that owns each element of the unit */
const uint32 *
unit_parent(AggExec *ex)
{
	if (ex->parent == NULL)
	{
		ex->parent = MemoryContextAlloc(ex->ucxt, (Size) ex->nelem * sizeof(uint32) + SIFT_AGG_PAD);
		sift_colk->expand_parents(ex->offs + 1, ex->unit.nrow, ex->parent);
	}
	return ex->parent;
}

/* v / 10^scale as a numeric of that scale: the text carries all the digits, so 1.50 keeps its display scale; SIFT_NX_NAN is the NaN */
Datum
scaled_numeric(__int128 v, int scale)
{
	char		tmp[48],
				buf[64];
	char	   *o = buf;
	int			n = 0;
	unsigned __int128 u = v < 0 ? -(unsigned __int128) v : (unsigned __int128) v;

	if (v == SIFT_NX_NAN)
		return DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	do
	{
		tmp[n++] = '0' + (int) (u % 10);
		u /= 10;
	} while (u > 0 || n <= scale);
	if (v < 0)
		*o++ = '-';
	for (int i = n - 1; i >= 0; i--)
	{
		*o++ = tmp[i];
		if (i == scale && scale > 0)
			*o++ = '.';
	}
	*o = '\0';
	return DirectFunctionCall3(numeric_in, CStringGetDatum(buf), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}
