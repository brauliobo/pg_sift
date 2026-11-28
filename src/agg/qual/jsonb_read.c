/*
 * agg/qual/jsonb_read.c - the values the aggregate scan reads from the payload of an exception of a key of a jsonb document (agg/qual/jsonb.c): its `->>` text, the casts of its jsonb
 * and of the float8 a leaf holds to an integer type, with PostgreSQL's own errors for the values that do not convert.
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type_d.h"
#include "nodes/miscnodes.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"

#include "agg/engine.h"
#include "core/compat.h"
#include "core/errctx.h"

/* the `->>` text of the value of an exception, NULL for a JSON null */
static char *
exception_text(const char *payload, uint32 len)
{
	Jsonb	   *jb = memcpy(palloc(len), payload, len);
	JsonbValue	jv;

	if (!JsonbExtractScalar(&jb->root, &jv))
		return JsonbToCString(NULL, &jb->root, (int) len);
	switch (jv.type)
	{
		case jbvString:
			return pnstrdup(jv.val.string.val, jv.val.string.len);
		case jbvNumeric:
			return DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(jv.val.numeric)));
		case jbvBool:
			return pstrdup(jv.val.boolean ? "true" : "false");
		default:
			return NULL;
	}
}

/* the number of the jsonb of a (doc -> 'k')::float8, as its text; NULL: a JSON null (NULL from PostgreSQL 18 on, the only version the cast is read in) or a value the cast raises for (*raises) */
static char *
cast_number_text(const char *payload, uint32 len, bool *raises)
{
	Jsonb	   *jb = memcpy(palloc(len), payload, len);
	JsonbValue	jv;
	const bool	scalar = JsonbExtractScalar(&jb->root, &jv);

	*raises = !scalar || (jv.type != jbvNumeric && jv.type != jbvNull);
	return scalar && jv.type == jbvNumeric ? DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(jv.val.numeric))) : NULL;
}

/* the input function of the cast of the `->>` text to the type */
static PGFunction
text_cast_in(Oid typid)
{
	switch (typid)
	{
		case FLOAT8OID:
			return float8in;
		case INT2OID:
			return int2in;
		case INT4OID:
			return int4in;
		case INT8OID:
			return int8in;
		default:
			return boolin;
	}
}

/* the cast of a jsonb to bool (of a boolean only) or to an integer (of a number only, rounded half away from zero as numeric is, raising for one the type cannot hold) */
static SiftV6JsonExc
jsonb_scalar_read(Oid typid, const char *payload, uint32 len, Datum *out)
{
	Jsonb	   *jb = memcpy(palloc(len), payload, len);
	JsonbValue	jv;
	bool		err = false;
	int64		v;

	if (!JsonbExtractScalar(&jb->root, &jv) || (jv.type != (typid == BOOLOID ? jbvBool : jbvNumeric) && jv.type != jbvNull))
		return SV6JX_RAISES;
	if (jv.type == jbvNull)
		return SV6JX_NULL;
	if (typid == BOOLOID)
		return (*out = BoolGetDatum(jv.val.boolean), SV6JX_VALUE);
	v = sift_numeric_int8(jv.val.numeric, &err);
	if (err || (typid == INT4OID && (v < PG_INT32_MIN || v > PG_INT32_MAX)) || (typid == INT2OID && (v < PG_INT16_MIN || v > PG_INT16_MAX)))
		return SV6JX_RAISES;
	*out = typid == INT8OID ? Int64GetDatum(v) : typid == INT4OID ? Int32GetDatum((int32) v) : Int16GetDatum((int16) v);
	return SV6JX_VALUE;
}

/* the cast of a jsonb to the type, whose errors are PostgreSQL's */
static PGFunction
jsonb_cast_fn(Oid typid)
{
	switch (typid)
	{
		case INT2OID:
			return jsonb_int2;
		case INT4OID:
			return jsonb_int4;
		case INT8OID:
			return jsonb_int8;
		case BOOLOID:
			return jsonb_bool;
		default:
			return jsonb_float8;
	}
}

SiftV6JsonExc
sift_v6_json_exception_read(Oid typid, bool jsonb_cast, const char *payload, uint32 len, MemoryContext cx, SiftV6JsonValue *out)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	bool		raises = false;
	char	   *s;
	SiftV6JsonExc res;

	if (jsonb_cast && typid != FLOAT8OID)
	{
		res = jsonb_scalar_read(typid, payload, len, &out->value);
		MemoryContextSwitchTo(old);
		return res;
	}
	s = jsonb_cast ? cast_number_text(payload, len, &raises) : exception_text(payload, len);
	res = raises ? SV6JX_RAISES : s == NULL ? SV6JX_NULL : SV6JX_VALUE;
	if (res == SV6JX_VALUE && sift_v6_class(typid) == SV6C_TEXT)
		*out = (SiftV6JsonValue) {.text = s,.tlen = strlen(s)};
	else if (res == SV6JX_VALUE && !DirectInputFunctionCallSafe(text_cast_in(typid), s, InvalidOid, -1, (Node *) &escontext, &out->value))
		res = SV6JX_RAISES;
	MemoryContextSwitchTo(old);
	return res;
}

void
sift_v6_json_exception_raise(Oid typid, bool jsonb_cast, const char *payload, uint32 len, MemoryContext cx)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	char	   *s;

	if (jsonb_cast)
		(void) DirectFunctionCall1(jsonb_cast_fn(typid), PointerGetDatum(memcpy(palloc(len), payload, len)));
	else if ((s = exception_text(payload, len)) != NULL)
		(void) DirectFunctionCall1(text_cast_in(typid), CStringGetDatum(s));
	MemoryContextSwitchTo(old);
	SIFT_INVARIANT(false, "the conversion of a value of a key that does not convert raises");
}

SiftV6JsonExc
sift_v6_json_float_int(Oid typid, double f, Datum *out)
{
	const double r = round(f);

	if (fabs(f) < 4503599627370496.0 ? fabs(f - r) == 0.5 : typid == INT8OID && fabs(f) <= 9223372036854775808.0)
		return SV6JX_INEXACT;
	if (typid == INT2OID ? !(r >= PG_INT16_MIN && r <= PG_INT16_MAX) : typid == INT4OID ? !(r >= PG_INT32_MIN && r <= PG_INT32_MAX) : !(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
		return SV6JX_RAISES;
	*out = typid == INT8OID ? Int64GetDatum((int64) r) : typid == INT4OID ? Int32GetDatum((int32) r) : Int16GetDatum((int16) r);
	return SV6JX_VALUE;
}

void
sift_v6_json_float_int_raise(Oid typid, double f)
{
	(void) DirectFunctionCall1(typid == INT2OID ? numeric_int2 : typid == INT4OID ? numeric_int4 : numeric_int8, DirectFunctionCall1(float8_numeric, Float8GetDatum(f)));
	SIFT_INVARIANT(false, "the integer cast of a number out of the range of its type raises");
}
