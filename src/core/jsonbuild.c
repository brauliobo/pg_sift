/*
 * core/jsonbuild.c - sift.json_build_object: json_build_object with the category and output function of each argument looked up once per call site
 *
 * The core function asks the type cache for every argument of every call (json_categorize_type: the type, its output function, a cast to json); a
 * query that builds one object per output row pays that per row. The text is the core's: the categories that need more than an output function
 * (numbers, datetimes, arrays, composites, casts) are written by the core's own datum_to_json, so only the writing of a string, a boolean, a
 * json value and a NULL is repeated here (test json_build_object compares both byte for byte).
 */
#include "postgres.h"

#include "catalog/pg_proc.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "utils/acl.h"
#include "common/shortest_dec.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/json.h"
#include "utils/jsonfuncs.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "agg/agg.h"

#include "core/sift.h"
#include "core/sql.h"

typedef struct JbSlot
{
	JsonTypeCategory cat;
	Oid			outf;
	bool		raw;			/* an unknown literal, read as the C string it is: the text it would be converted to writes the same bytes */
	bool		stable;			/* a key that is a constant or a parameter of the query: the same for every call of the site */
	char	   *pre;			/* what a call writes for the key, with the separator before and the colon after it (a stable key, from the first call) */
	int			prelen;
	FmgrInfo	out;
} JbSlot;

typedef struct JbCache
{
	uint64		gen;
	int			n;
	bool		direct;			/* the arguments are the call's own: no VARIADIC array, no unknown literal turned into text */
	Oid		   *types;
	JbSlot	   *slot;
	StringInfoData buf;			/* the text of a call is built here, in the memory of the call site, and copied out once */
	bool		busy;			/* the buffer is in use: a call that comes back to the site (a cast that calls it) builds in its own */
} JbCache;

/* a change of a type, its output function or a cast to json: every call site looks its arguments up again */
static uint64 jb_gen = 1;
static bool jb_registered;

static void
jb_invalidate(Datum arg, int cacheid, uint32 hashvalue)
{
	jb_gen++;
}

static void
jb_fill(JbCache *c, MemoryContext cx, int n, const Oid *types)
{
	if (c->types)
	{
		for (int i = 0; i < c->n; i++)
			if (c->slot[i].pre)
				pfree(c->slot[i].pre);
		pfree(c->types);
		pfree(c->slot);
	}
	c->types = MemoryContextAlloc(cx, Max(n, 1) * sizeof(Oid));
	c->slot = MemoryContextAllocZero(cx, Max(n, 1) * sizeof(JbSlot));
	c->n = 0;
	for (int i = 0; i < n; i++)
	{
		JbSlot	   *s = &c->slot[i];

		if (types[i] == InvalidOid)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("could not determine input data type")));
		json_categorize_type(types[i], false, &s->cat, &s->outf);
		if (OidIsValid(s->outf))
			fmgr_info_cxt(s->outf, &s->out, cx);
		c->types[i] = types[i];
	}
	c->n = n;
	c->gen = jb_gen;
}

/* an integer or a float8 the way the core writes it, without the output function's string and the text datum_to_json makes of it */
static bool
jb_number(StringInfo out, const JbSlot *s, Datum v)
{
	char		buf[32];
	int			len;

	switch (s->outf)
	{
		case F_INT2OUT:
			len = pg_ltoa((int32) DatumGetInt16(v), buf);
			break;
		case F_INT4OUT:
			len = pg_ltoa(DatumGetInt32(v), buf);
			break;
		case F_INT8OUT:
			len = pg_lltoa(DatumGetInt64(v), buf);
			break;
		case F_FLOAT8OUT:
			/* the shortest form is what float8out writes with extra_float_digits above 0; NaN and the infinities are strings in json */
			if (extra_float_digits <= 0 || isnan(DatumGetFloat8(v)) || isinf(DatumGetFloat8(v)))
				return false;
			len = double_to_shortest_decimal_buf(DatumGetFloat8(v), buf);
			break;
		default:
			return false;
	}
	appendBinaryStringInfo(out, buf, len);
	return true;
}

static void
jb_value(StringInfo out, JbSlot *s, Datum v, bool isnull, bool key)
{
	if (isnull)
	{
		appendStringInfoString(out, "null");
		return;
	}
	if (s->raw)
	{
		escape_json(out, DatumGetCString(v));
		return;
	}
	switch (s->cat)
	{
		case JSONTYPE_BOOL:
			if (key)
				appendStringInfoChar(out, '"');
			appendStringInfoString(out, DatumGetBool(v) ? "true" : "false");
			if (key)
				appendStringInfoChar(out, '"');
			return;
		case JSONTYPE_OTHER:
			escape_json(out, OutputFunctionCall(&s->out, v));
			return;
		case JSONTYPE_JSON:
			if (!key)
			{
				if (s->outf == F_JSON_OUT)
				{
					/* json_out is textout: the bytes of the value */
					text	   *t = DatumGetTextPP(v);

					appendBinaryStringInfo(out, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
					return;
				}
				appendStringInfoString(out, OutputFunctionCall(&s->out, v));
				return;
			}
			break;
		case JSONTYPE_NUMERIC:
			if (key)
			{
				appendStringInfoChar(out, '"');
				appendStringInfoString(out, OutputFunctionCall(&s->out, v));
				appendStringInfoChar(out, '"');
				return;
			}
			if (jb_number(out, s, v))
				return;
			break;
		default:
			break;
	}
	if (key && (s->cat == JSONTYPE_ARRAY || s->cat == JSONTYPE_COMPOSITE || s->cat == JSONTYPE_JSON || s->cat == JSONTYPE_JSONB || s->cat == JSONTYPE_CAST))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("key value must be scalar, not array, composite, or json")));
	{
		text	   *t = DatumGetTextPP(datum_to_json(v, s->cat, s->outf));

		appendBinaryStringInfo(out, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
	}
}

Datum
sift_json_build_object(PG_FUNCTION_ARGS)
{
	Datum		own[FUNC_MAX_ARGS];
	bool		own_null[FUNC_MAX_ARGS];
	Datum	   *args;
	bool	   *nulls;
	Oid		   *types;
	int			nargs = PG_NARGS();
	JbCache    *c = fcinfo->flinfo->fn_extra;
	StringInfoData own_out;
	StringInfo	out;
	text	   *result;

	if (!jb_registered)
	{
		CacheRegisterSyscacheCallback(TYPEOID, jb_invalidate, (Datum) 0);
		CacheRegisterSyscacheCallback(CASTSOURCETARGET, jb_invalidate, (Datum) 0);
		jb_registered = true;
	}
	if (c == NULL)
		c = fcinfo->flinfo->fn_extra = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, sizeof(JbCache));
	if (!(c->direct && c->gen == jb_gen && c->n == nargs))
	{
		bool		direct = !get_fn_expr_variadic(fcinfo->flinfo);

		nargs = extract_variadic_args(fcinfo, 0, true, &args, &types, &nulls);
		if (nargs < 0)
			PG_RETURN_NULL();
		if (nargs % 2 != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("argument list must have even number of elements"),
			/* translator: %s is a SQL function name */
					 errhint("The arguments of %s must consist of alternating keys and values.", "json_build_object()")));
		if (c->gen != jb_gen || c->n != nargs || (nargs > 0 && memcmp(c->types, types, nargs * sizeof(Oid)) != 0))
			jb_fill(c, fcinfo->flinfo->fn_mcxt, nargs, types);
		for (int i = 0; i < nargs; i++)
		{
			c->slot[i].raw = direct && get_fn_expr_argtype(fcinfo->flinfo, i) == UNKNOWNOID;
			c->slot[i].stable = direct && i % 2 == 0 && get_fn_expr_arg_stable(fcinfo->flinfo, i);
		}
		c->direct = direct;
	}
	/* a call site types its arguments once, the plan fixes them: the arrays extract_variadic_args builds for every call are the call's own */
	if (c->direct)
	{
		for (int i = 0; i < nargs; i++)
		{
			own[i] = fcinfo->args[i].value;
			own_null[i] = fcinfo->args[i].isnull;
		}
		args = own;
		nulls = own_null;
	}
	if (c->direct && !c->busy)
	{
		if (c->buf.data == NULL)
		{
			MemoryContext old = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

			initStringInfo(&c->buf);
			MemoryContextSwitchTo(old);
		}
		resetStringInfo(&c->buf);
		out = &c->buf;
		c->busy = true;
	}
	else
	{
		initStringInfo(&own_out);
		out = &own_out;
	}
	appendStringInfoChar(out, '{');
	for (int i = 0; i < nargs; i += 2)
	{
		JbSlot	   *k = &c->slot[i];

		if (k->pre)
		{
			appendBinaryStringInfo(out, k->pre, k->prelen);
		}
		else
		{
			int			start = i > 0 ? out->len : out->len - 1;

			if (i > 0)
				appendStringInfoString(out, ", ");
			if (nulls[i])
				ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("null value not allowed for object key")));
			jb_value(out, k, args[i], false, true);
			appendStringInfoString(out, " : ");
			if (k->stable)
			{
				/* the first pair also holds the brace that opens the object, which every call writes before it */
				k->prelen = out->len - start;
				k->pre = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt, k->prelen);
				memcpy(k->pre, out->data + start, k->prelen);
				if (i == 0)
					memmove(k->pre, k->pre + 1, --k->prelen);
			}
		}
		jb_value(out, &c->slot[i + 1], args[i + 1], nulls[i + 1], false);
	}
	appendStringInfoChar(out, '}');
	result = cstring_to_text_with_len(out->data, out->len);
	if (out == &c->buf)
		c->busy = false;
	PG_RETURN_TEXT_P(result);
}

static Node *
swap_call(Node *n, void *context)
{
	if (n == NULL)
		return NULL;
	SIFT_SKIP_CONST_LIST(n, n);
	if (IsA(n, Query))
		return (Node *) query_tree_mutator((Query *) n, swap_call, context, QTW_DONT_COPY_QUERY);
	n = expression_tree_mutator(n, swap_call, context);
	if (IsA(n, FuncExpr) && ((FuncExpr *) n)->funcid == F_JSON_BUILD_OBJECT_ANY)
		((FuncExpr *) n)->funcid = *(Oid *) context;
	return n;
}

static bool
calls_json_build_object(Node *n, void *context)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Query))
		return query_tree_walker((Query *) n, calls_json_build_object, context, 0);
	return (IsA(n, FuncExpr) && ((FuncExpr *) n)->funcid == F_JSON_BUILD_OBJECT_ANY) || expression_tree_walker(n, calls_json_build_object, context);
}

/* does the query tree call json_build_object? Asked before the tree is copied for the swap: a walk that builds nothing */
bool
sift_json_build_object_calls(Query *q)
{
	return OidIsValid(sift_catalog()->json_build_object_func) && calls_json_build_object((Node *) q, NULL);
}

/* the same text, so a query that builds an object per row does not ask the type cache for every argument of every row: the call sites of the query take sift's function */
void
sift_json_build_object_swap(Query *q)
{
	Oid			swap = sift_catalog()->json_build_object_func;

	if (OidIsValid(swap) && object_aclcheck(ProcedureRelationId, swap, GetUserId(), ACL_EXECUTE) == ACLCHECK_OK)
		query_tree_mutator(q, swap_call, &swap, QTW_DONT_COPY_QUERY);
}
