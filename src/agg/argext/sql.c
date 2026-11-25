/*
 * agg/argext/sql.c - sift.arg_max(val, cmp) and sift.arg_min(val, cmp): the transition, combine, serial, deserial and final functions of the aggregates (sql/argext_api.sql), the ClickHouse argMax and
 * argMin: the val of the row with the greatest (least) cmp in the btree order of the type of cmp under the collation of the call; a row whose cmp is NULL is ignored; a NULL val on the winning row is the
 * result; no row with a cmp is NULL. The first of the rows that tie on cmp wins in one process; after a parallel combine the choice among tying rows is as unspecified as an ORDER BY that ties.
 * The state is the one best row (its cmp and val are copies in the aggregate's memory context), so it is bounded whatever the number of rows; it is serialized with datumSerialize, which is exact for every type.
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "agg/argext/argext.h"
#include "agg/argext/sql.h"

typedef struct SiftArgState
{
	Oid			cmp_type;
	Oid			val_type;
	Oid			collation;
	int16		cmp_len;
	int16		val_len;
	bool		cmp_byval;
	bool		val_byval;
	FmgrInfo	cmp_fn;
	bool		has;
	bool		val_null;
	Datum		cmp;
	Datum		val;
} SiftArgState;

typedef struct SiftArgWire
{
	Oid			cmp_type;
	Oid			val_type;
	Oid			collation;
	uint8		has;
	uint8		val_null;
} SiftArgWire;

PG_FUNCTION_INFO_V1(sift_argmax_trans);
PG_FUNCTION_INFO_V1(sift_argmin_trans);
PG_FUNCTION_INFO_V1(sift_argmax_combine);
PG_FUNCTION_INFO_V1(sift_argmin_combine);
PG_FUNCTION_INFO_V1(sift_argext_serial);
PG_FUNCTION_INFO_V1(sift_argext_deserial);
PG_FUNCTION_INFO_V1(sift_argext_final);

static SiftArgState *
state_new(MemoryContext cx, Oid cmp_type, Oid val_type, Oid collation)
{
	SiftArgState *s = MemoryContextAllocZero(cx, sizeof(SiftArgState));
	TypeCacheEntry *tce = lookup_type_cache(cmp_type, TYPECACHE_CMP_PROC_FINFO);

	if (!OidIsValid(tce->cmp_proc_finfo.fn_oid))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg("could not identify a comparison function for type %s", format_type_be(cmp_type))));
	fmgr_info_copy(&s->cmp_fn, &tce->cmp_proc_finfo, cx);
	s->cmp_type = cmp_type;
	s->val_type = val_type;
	s->collation = collation;
	get_typlenbyval(cmp_type, &s->cmp_len, &s->cmp_byval);
	get_typlenbyval(val_type, &s->val_len, &s->val_byval);
	return s;
}

static void
state_release(SiftArgState *s)
{
	if (!s->has)
		return;
	if (!s->cmp_byval)
		pfree(DatumGetPointer(s->cmp));
	if (!s->val_byval && !s->val_null)
		pfree(DatumGetPointer(s->val));
}

static void
state_set(SiftArgState *s, MemoryContext cx, Datum cmp, Datum val, bool val_null)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	Datum		ncmp = datumCopy(cmp, s->cmp_byval, s->cmp_len);
	Datum		nval = val_null ? (Datum) 0 : datumCopy(val, s->val_byval, s->val_len);

	state_release(s);
	s->cmp = ncmp;
	s->val = nval;
	s->val_null = val_null;
	s->has = true;
	MemoryContextSwitchTo(old);
}

static bool
state_better(SiftArgState *s, SiftArgDir dir, Datum cmp)
{
	int32		c = DatumGetInt32(FunctionCall2Coll(&s->cmp_fn, s->collation, cmp, s->cmp));

	return dir == SIFT_ARG_MAX ? c > 0 : c < 0;
}

static MemoryContext
agg_context(FunctionCallInfo fcinfo)
{
	MemoryContext cx;

	SIFT_INVARIANT(AggCheckCallContext(fcinfo, &cx), "sift.arg_max: called outside an aggregate");
	return cx;
}

static Datum
argext_trans(FunctionCallInfo fcinfo, SiftArgDir dir)
{
	MemoryContext cx = agg_context(fcinfo);
	SiftArgState *s = PG_ARGISNULL(0) ? NULL : (SiftArgState *) PG_GETARG_POINTER(0);

	if (PG_ARGISNULL(2))
	{
		if (s == NULL)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(s);
	}
	if (s == NULL)
		s = state_new(cx, get_fn_expr_argtype(fcinfo->flinfo, 2), get_fn_expr_argtype(fcinfo->flinfo, 1), PG_GET_COLLATION());
	else if (s->has && !state_better(s, dir, PG_GETARG_DATUM(2)))
		PG_RETURN_POINTER(s);
	state_set(s, cx, PG_GETARG_DATUM(2), PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1), PG_ARGISNULL(1));
	PG_RETURN_POINTER(s);
}

Datum
sift_argmax_trans(PG_FUNCTION_ARGS)
{
	return argext_trans(fcinfo, SIFT_ARG_MAX);
}

Datum
sift_argmin_trans(PG_FUNCTION_ARGS)
{
	return argext_trans(fcinfo, SIFT_ARG_MIN);
}

static Datum
argext_combine(FunctionCallInfo fcinfo, SiftArgDir dir)
{
	MemoryContext cx = agg_context(fcinfo);
	SiftArgState *a = PG_ARGISNULL(0) ? NULL : (SiftArgState *) PG_GETARG_POINTER(0);
	SiftArgState *b = PG_ARGISNULL(1) ? NULL : (SiftArgState *) PG_GETARG_POINTER(1);

	if (b == NULL || !b->has)
	{
		if (a == NULL)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(a);
	}
	if (a == NULL)
		a = state_new(cx, b->cmp_type, b->val_type, b->collation);
	if (!a->has || state_better(a, dir, b->cmp))
		state_set(a, cx, b->cmp, b->val, b->val_null);
	PG_RETURN_POINTER(a);
}

Datum
sift_argmax_combine(PG_FUNCTION_ARGS)
{
	return argext_combine(fcinfo, SIFT_ARG_MAX);
}

Datum
sift_argmin_combine(PG_FUNCTION_ARGS)
{
	return argext_combine(fcinfo, SIFT_ARG_MIN);
}

Datum
sift_argext_serial(PG_FUNCTION_ARGS)
{
	SiftArgState *s = (SiftArgState *) PG_GETARG_POINTER(0);
	SiftArgWire w = {s->cmp_type, s->val_type, s->collation, s->has, s->val_null};
	Size		sz = sizeof(w);
	bytea	   *r;
	char	   *p;

	if (s->has)
		sz += datumEstimateSpace(s->cmp, false, s->cmp_byval, s->cmp_len) + datumEstimateSpace(s->val, s->val_null, s->val_byval, s->val_len);
	r = (bytea *) palloc(VARHDRSZ + sz);
	SET_VARSIZE(r, VARHDRSZ + sz);
	p = VARDATA(r);
	memcpy(p, &w, sizeof(w));
	p += sizeof(w);
	if (s->has)
	{
		datumSerialize(s->cmp, false, s->cmp_byval, s->cmp_len, &p);
		datumSerialize(s->val, s->val_null, s->val_byval, s->val_len, &p);
	}
	PG_RETURN_BYTEA_P(r);
}

Datum
sift_argext_deserial(PG_FUNCTION_ARGS)
{
	MemoryContext cx = agg_context(fcinfo);
	bytea	   *in = PG_GETARG_BYTEA_P(0);
	char	   *p = VARDATA(in);
	SiftArgWire w;
	SiftArgState *s;

	memcpy(&w, p, sizeof(w));
	p += sizeof(w);
	s = state_new(cx, w.cmp_type, w.val_type, w.collation);
	if (w.has)
	{
		MemoryContext old = MemoryContextSwitchTo(cx);
		bool		isnull;

		s->cmp = datumRestore(&p, &isnull);
		s->val = datumRestore(&p, &isnull);
		s->val_null = isnull;
		s->has = true;
		MemoryContextSwitchTo(old);
	}
	PG_RETURN_POINTER(s);
}

Datum
sift_argext_final(PG_FUNCTION_ARGS)
{
	SiftArgState *s = PG_ARGISNULL(0) ? NULL : (SiftArgState *) PG_GETARG_POINTER(0);

	if (s == NULL || !s->has || s->val_null)
		PG_RETURN_NULL();
	PG_RETURN_DATUM(datumCopy(s->val, s->val_byval, s->val_len));
}

int
sift_argext_aggref_dir(const Aggref *a)
{
	char	   *name = get_func_name(a->aggfnoid);
	Oid			sift_ns = get_namespace_oid("sift", true);
	int			dir = -1;

	if (name != NULL && OidIsValid(sift_ns) && get_func_namespace(a->aggfnoid) == sift_ns && a->aggdistinct == NIL && a->aggorder == NIL && a->aggkind == AGGKIND_NORMAL && !a->aggvariadic)
	{
		if (strcmp(name, "arg_max") == 0)
			dir = SIFT_ARG_MAX;
		else if (strcmp(name, "arg_min") == 0)
			dir = SIFT_ARG_MIN;
	}
	if (name != NULL)
		pfree(name);
	return dir;
}
