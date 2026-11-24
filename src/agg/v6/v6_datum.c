/*
 * agg/v6/v6_datum.c - SVEC_DATUM vectors and their conversions (agg/v6/v6_datum.h).
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_datum.h"
#include "agg/group/ht.h"
#include "core/memtrack.h"

void
sift_datum_out_init(SiftDatumOut *o, MemoryContext cx, Oid typid)
{
	*o = (SiftDatumOut) {0};
	o->vals = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(Datum) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	o->vec = (SiftVec) {.data = o->vals,.valid = o->valid,.dense = true,.type = SVEC_DATUM,.typid = typid};
	get_typlenbyval(typid, &o->vec.typlen, &o->vec.typbyval);
}

bool
sift_v6_type_native(Oid typid)
{
	return sift_v6_class(getBaseType(typid)) != SV6C_NONE && getBaseType(typid) != NUMERICOID;
}

bool
sift_v6_key_generic(Oid typid)
{
	const SiftV6Class cls = sift_v6_class(getBaseType(typid));

	return cls == SV6C_NONE || cls == SV6C_F64 || getBaseType(typid) == NUMERICOID;
}

static inline int64
int_of(const SiftVec *v, uint16 r)
{
	switch (v->type)
	{
		case SVEC_I32:
			return ((const int32 *) v->data)[r];
		case SVEC_BOOL:
			return ((const uint8 *) v->data)[r];
		default:
			return ((const int64 *) v->data)[r];
	}
}

static Datum
numeric_of(SiftI128 x, int scale)
{
	return sift_v6_scaled_numeric(x, scale);
}

static Datum
text_of(Oid typid, const SiftStrView *sv)
{
	const uint8 *p = sift_strview_bytes(sv);

	if (typid == BYTEAOID)
	{
		bytea	   *b = palloc(VARHDRSZ + sv->len);

		SET_VARSIZE(b, VARHDRSZ + sv->len);
		memcpy(VARDATA(b), p, sv->len);
		return PointerGetDatum(b);
	}
	return PointerGetDatum(cstring_to_text_with_len((const char *) p, sv->len));
}

static Datum
uuid_of(const SiftUuid16 *u)
{
	pg_uuid_t  *out = palloc(sizeof(pg_uuid_t));

	for (int i = 0; i < 8; i++)
	{
		out->data[i] = (uint8) (u->hi >> (56 - 8 * i));
		out->data[8 + i] = (uint8) (u->lo >> (56 - 8 * i));
	}
	return UUIDPGetDatum(out);
}

static Datum
interval_of(const SiftInterval16 *iv)
{
	Interval   *out = palloc(sizeof(Interval));

	out->time = iv->time;
	out->day = iv->day;
	out->month = iv->month;
	return IntervalPGetDatum(out);
}

static Datum
int_datum(const SiftVec *v, uint16 r, Oid typid, int scale)
{
	const int64 x = int_of(v, r);

	switch (typid)
	{
		case INT2OID:
			return Int16GetDatum((int16) x);
		case INT4OID:
		case DATEOID:
			return Int32GetDatum((int32) x);
		case BOOLOID:
			return BoolGetDatum(x != 0);
		case NUMERICOID:
			return numeric_of((SiftI128) {.lo = (uint64) x,.hi = x < 0 ? -1 : 0}, scale);
		default:
			return Int64GetDatum(x);
	}
}

Datum
sift_v6_native_datum(const SiftVec *v, uint16 r, Oid typid, int scale)
{
	typid = getBaseType(typid);
	switch (v->type)
	{
		case SVEC_BOOL:
		case SVEC_I32:
		case SVEC_I64:
			return int_datum(v, r, typid, scale);
		case SVEC_F64:
			return typid == FLOAT4OID ? Float4GetDatum((float4) ((const double *) v->data)[r]) : Float8GetDatum(((const double *) v->data)[r]);
		case SVEC_NUM64:
			return int_datum(&(SiftVec) {.data = v->data,.type = SVEC_I64}, r, NUMERICOID, scale);
		case SVEC_NUM128:
			return numeric_of(((const SiftI128 *) v->data)[r], scale);
		case SVEC_STRVIEW:
			return text_of(typid, &((const SiftStrView *) v->data)[r]);
		case SVEC_UUID:
			return uuid_of(&((const SiftUuid16 *) v->data)[r]);
		case SVEC_INTERVAL:
			return interval_of(&((const SiftInterval16 *) v->data)[r]);
		case SVEC_DATUM:
			return ((const Datum *) v->data)[r];
		case SVEC_DICT32:
			break;
	}
	SIFT_INVARIANT(false, "a vector of dictionary codes is not turned into datums");
	return (Datum) 0;
}

Datum
sift_v6_vec_datum(const SiftVec *v, uint16 r, Oid typid, int scale, bool *isnull)
{
	const bool	nan = v->nan != NULL && ((v->nan[r >> 6] >> (r & 63)) & 1);

	*isnull = !nan && !(v->all_valid || ((v->valid[r >> 6] >> (r & 63)) & 1));
	if (*isnull)
		return (Datum) 0;
	return nan ? DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)) : sift_v6_native_datum(v, r, typid, scale);
}

SiftVecType
sift_v6_result_vec_type(Oid typid)
{
	switch (sift_v6_class(getBaseType(typid)))
	{
		case SV6C_F64:
			return SVEC_F64;
		case SV6C_TEXT:
			return SVEC_STRVIEW;
		default:
			return SVEC_I64;
	}
}

void
sift_v6_result_put(Oid typid, void *data, uint16 r, Datum d)
{
	switch (getBaseType(typid))
	{
		case INT2OID:
			((int64 *) data)[r] = DatumGetInt16(d);
			break;
		case INT4OID:
		case DATEOID:
			((int64 *) data)[r] = DatumGetInt32(d);
			break;
		case BOOLOID:
			((int64 *) data)[r] = DatumGetBool(d);
			break;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			((int64 *) data)[r] = DatumGetInt64(d);
			break;
		case FLOAT4OID:
			((double *) data)[r] = DatumGetFloat4(d);
			break;
		case FLOAT8OID:
			((double *) data)[r] = DatumGetFloat8(d);
			break;
		default:
			{
				const text *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
				const uint32 len = VARSIZE_ANY_EXHDR(t);
				SiftStrView *sv = &((SiftStrView *) data)[r];

				*sv = (SiftStrView) {.len = len};
				if (len > 0)
				{
					memcpy(sv->prefix, VARDATA_ANY(t), Min(len, 4));
					if (len <= 12)
						memcpy(sv->u.tail, VARDATA_ANY(t) + 4, len > 4 ? len - 4 : 0);
					else
						sv->u.ptr = VARDATA_ANY(t);
				}
				break;
			}
	}
}

void
sift_v6_key_codes(SiftGenKey *k, const SiftVec *v, Oid typid, int scale, const SiftBatchSel *bs, MemoryContext scratch, int64 *codes, uint64 *valid)
{
	MemoryContext old = MemoryContextSwitchTo(scratch);

	memset(valid, 0, sizeof(uint64) * SIFT_VEC_WORDS);
	for (uint16 w = 0; w < (bs->n_raw + 63) / 64; w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint16 r = (uint16) (w * 64 + __builtin_ctzll(bits));

			if (!(v->all_valid || ((v->valid[r >> 6] >> (r & 63)) & 1)))
				continue;
			codes[r] = sift_genkey_intern(k, sift_v6_native_datum(v, r, typid, scale));
			valid[r >> 6] |= UINT64CONST(1) << (r & 63);
		}
	MemoryContextSwitchTo(old);
}

struct SiftV6GenKeys
{
	SiftV6Spec	spec;
	MemoryContext scratch;
	int			nkeys;
	SiftGenKey *gen[SIFT_V6_MAX_KEYS];
	Oid			typid[SIFT_V6_MAX_KEYS];
	int			scale[SIFT_V6_MAX_KEYS];
	int64	   *codes[SIFT_V6_MAX_KEYS];
	uint64		valid[SIFT_V6_MAX_KEYS][SIFT_VEC_WORDS];
	SiftVec		vec[SIFT_V6_MAX_KEYS];
	SiftVec    *out[SIFT_V6_MAX_KEYS];
};

SiftV6GenKeys *
sift_v6_genkeys_new(MemoryContext cx, const SiftV6Spec *spec)
{
	SiftV6GenKeys *g = NULL;

	for (int i = 0; i < spec->nkeys; i++)
	{
		if (!sift_v6_key_generic(spec->key[i].typid))
			continue;
		if (g == NULL)
		{
			g = MemoryContextAllocZero(cx, sizeof(SiftV6GenKeys));
			g->spec = *spec;
			g->nkeys = spec->nkeys;
			g->spec.key = MemoryContextAlloc(cx, SIFT_V6_MAX_KEYS * sizeof(SiftV6Col));
			memcpy(g->spec.key, spec->key, spec->nkeys * sizeof(SiftV6Col));
			g->scratch = sift_mem_allocset_create(cx, "sift agg v6 generic key values");
		}
		g->typid[i] = getBaseType(spec->key[i].typid);
		g->scale[i] = spec->key[i].scale;
		g->gen[i] = sift_genkey_new(cx, g->typid[i], spec->key[i].collid);
		g->codes[i] = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		g->spec.key[i].typid = INT8OID;
		g->spec.key[i].scale = 0;
		g->spec.key[i].collid = InvalidOid;
	}
	return g;
}

const SiftV6Spec *
sift_v6_genkeys_spec(const SiftV6GenKeys *g)
{
	return &g->spec;
}

SiftVec *const *
sift_v6_genkeys_apply(SiftV6GenKeys *g, const SiftBatchSel *bs, SiftVec *const *keyvec)
{
	for (int i = 0; i < g->nkeys; i++)
	{
		g->out[i] = keyvec[i];
		if (g->gen[i] == NULL)
			continue;
		sift_v6_key_codes(g->gen[i], keyvec[i], g->typid[i], g->scale[i], bs, g->scratch, g->codes[i], g->valid[i]);
		g->vec[i] = (SiftVec) {.data = g->codes[i],.valid = g->valid[i],.len = bs->n_raw,.dense = true,.type = SVEC_I64};
		g->out[i] = &g->vec[i];
	}
	MemoryContextReset(g->scratch);
	return g->out;
}

void
sift_v6_genkeys_resolve(const SiftV6GenKeys *g, Datum *values, const bool *isnull, uint32 want)
{
	for (int i = 0; i < g->nkeys; i++)
		if (g->gen[i] != NULL && ((want >> i) & 1) && !isnull[i])
			values[i] = sift_genkey_datum(g->gen[i], (uint32) DatumGetInt64(values[i]));
}

uint64
sift_v6_genkeys_bytes(const SiftV6GenKeys *g)
{
	uint64		bytes = 0;

	for (int i = 0; i < g->nkeys; i++)
		if (g->gen[i] != NULL)
			bytes += sift_genkey_bytes(g->gen[i]) + SIFT_VEC_LEN * sizeof(int64);
	return bytes;
}

void
sift_v6_genkeys_free(SiftV6GenKeys *g)
{
	for (int i = 0; i < g->nkeys; i++)
		if (g->gen[i] != NULL)
		{
			sift_genkey_free(g->gen[i]);
			pfree(g->codes[i]);
		}
	MemoryContextDelete(g->scratch);
	pfree(g->spec.key);
	pfree(g);
}
