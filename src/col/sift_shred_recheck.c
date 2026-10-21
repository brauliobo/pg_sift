/*
 * sift_shred_recheck.c - the walk of a row that holds a value a typed leaf cannot store, read as the SQL of the query reads it
 * (docs/COL-DIRTY-UNITS.md). The shredder keeps such a value as an overflow slot; here it is turned into the value the expression
 * gives (the text of a scalar through the input function of the leaf's type), into NULL (a key of an array or scalar is NULL), or into
 * the error SQL raises for it, which is kept to be raised when the row reaches the expression.
 */
#include "sift_host.h"

#include <math.h>

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/numeric.h"

#include "sift_shred_pg.h"

static Oid
input_function(SiftShredType t)
{
	switch (t)
	{
		case SIFT_SHRED_BOOL:
			return F_BOOLIN;
		case SIFT_SHRED_INT2:
			return F_INT2IN;
		case SIFT_SHRED_INT4:
			return F_INT4IN;
		case SIFT_SHRED_INT8:
			return F_INT8IN;
		case SIFT_SHRED_FLOAT4:
			return F_FLOAT4IN;
		case SIFT_SHRED_FLOAT8:
			return F_FLOAT8IN;
		default:
			return InvalidOid;
	}
}

SiftShredRecheck *
sift_shred_recheck_create(MemoryContext mcxt, const SiftShredSpec *spec)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftShredRecheck *rc = palloc0(sizeof(SiftShredRecheck));

	rc->spec = spec;
	rc->mcxt = mcxt;
	rc->col = palloc0(Max(spec->ncols, 1));
	rc->grp = palloc0(Max(spec->ngroups, 1));
	rc->in = palloc0(Max(spec->ncols, 1) * sizeof(FmgrInfo));
	for (int c = 0; c < spec->ncols; c++)
		if (OidIsValid(input_function(spec->cols[c].type)))
			fmgr_info(input_function(spec->cols[c].type), &rc->in[c]);
	MemoryContextSwitchTo(old);
	return rc;
}

void
sift_shred_recheck_begin(SiftShredRecheck *rc)
{
	rc->err = NULL;
	rc->poison_grp = -1;
}

/* the first error of a document is the one SQL raises (the walk is in document order) */
static void
keep_error(SiftShredRecheck *rc, ErrorData *err)
{
	if (rc->err == NULL)
		rc->err = err;
}

/* an error raised by the function the query calls, as SQL raises it */
static void
function_error(SiftShredRecheck *rc, int sqlstate, const char *fmt, const char *fn)
{
	ErrorSaveContext esc = {T_ErrorSaveContext};
	MemoryContext old = MemoryContextSwitchTo(rc->mcxt);

	esc.details_wanted = true;
	errsave((Node *) &esc, (errcode(sqlstate), errmsg(fmt, fn)));
	keep_error(rc, esc.error_data);
	MemoryContextSwitchTo(old);
}

/* the text of a scalar or container the way `->>` and a record column give it; NULL for a JSON null */
static char *
value_text(const JsonbValue *v)
{
	switch (v->type)
	{
		case jbvString:
			return pnstrdup(v->val.string.val, v->val.string.len);
		case jbvNumeric:
			return DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(v->val.numeric)));
		case jbvBool:
			return pstrdup(v->val.boolean ? "true" : "false");
		case jbvBinary:
			return JsonbToCString(NULL, v->val.binary.data, v->val.binary.len);
		default:
			return NULL;
	}
}

/* the value of the leaf's type the text gives; false when the input function raises (kept) or the value is one the leaf cannot store (NaN, an infinity, -0) */
static bool
convert(SiftShred *sh, SiftShredRecheck *rc, int col, const char *text)
{
	SiftShredType type = rc->spec->cols[col].type;
	ErrorSaveContext esc = {T_ErrorSaveContext};
	MemoryContext old = MemoryContextSwitchTo(rc->mcxt);
	bool		ok;
	Datum		d;

	esc.details_wanted = true;
	ok = InputFunctionCallSafe(&rc->in[col], (char *) text, InvalidOid, -1, (Node *) &esc, &d);
	MemoryContextSwitchTo(old);
	if (!ok)
	{
		keep_error(rc, esc.error_data);
		if (rc->grp[rc->spec->cols[col].group] == SRG_RECORDSET)
			rc->poison_grp = rc->spec->cols[col].group;
		return false;
	}
	switch (type)
	{
		case SIFT_SHRED_BOOL:
			sift_shred_value_i64(sh, DatumGetBool(d));
			return true;
		case SIFT_SHRED_INT2:
			sift_shred_value_i64(sh, DatumGetInt16(d));
			return true;
		case SIFT_SHRED_INT4:
			sift_shred_value_i64(sh, DatumGetInt32(d));
			return true;
		case SIFT_SHRED_INT8:
			sift_shred_value_i64(sh, DatumGetInt64(d));
			return true;
		default:
			{
				double		f = type == SIFT_SHRED_FLOAT4 ? (double) DatumGetFloat4(d) : DatumGetFloat8(d);

				/* the shredder stores neither NaN, an infinity nor -0: the slot stays an overflow one and the scan raises its generic error */
				if (isnan(f) || isinf(f) || (f == 0 && signbit(f)))
					return false;
				sift_shred_value_f64(sh, f);
				return true;
			}
	}
}

/* a value where a leaf is declared, read as the query's expression reads it */
static bool
recheck_leaf(SiftShred *sh, const JsonbValue *v, SiftShredRecheck *rc, int col)
{
	char	   *text;

	if (rc->col[col] != SRC_TEXTIO || v->type == jbvNull)
		return false;
	text = value_text(v);
	if (!SIFT_SHRED_IS_TEXTUAL(rc->spec->cols[col].type))
		return convert(sh, rc, col, text);
	sh->any_length = true;
	sift_shred_string(sh, text, strlen(text));
	sh->any_length = false;
	return true;
}

bool
sift_shred_recheck_value(SiftShred *sh, const JsonbValue *v, SiftShredRecheck *rc)
{
	bool		arr = v->type == jbvBinary && JsonContainerIsArray(v->val.binary.data);
	bool		obj = v->type == jbvBinary && !arr;
	int			id = 0;

	switch (sift_shred_target(sh, &id))
	{
		case SIFT_SHRED_T_LEAF:
			return recheck_leaf(sh, v, rc, id);
		case SIFT_SHRED_T_OBJECT:
			/* a key of an array is NULL, as for `->` and a record column */
			if (!arr)
				return false;
			sift_shred_null(sh);
			return true;
		case SIFT_SHRED_T_ELEM:
			if (obj)
				return false;
			if (rc->grp[id] == SRG_RECORDSET)
			{
				function_error(rc, ERRCODE_INVALID_PARAMETER_VALUE, "argument of %s must be an array of objects", "jsonb_to_recordset");
				rc->poison_grp = id;
			}
			sift_shred_null(sh);
			return true;
		case SIFT_SHRED_T_LIST:
			if (arr || rc->grp[id] != SRG_RECORDSET)
				return false;
			function_error(rc, ERRCODE_INVALID_PARAMETER_VALUE, "cannot call %s on a non-array", "jsonb_to_recordset");
			if (v->type != jbvNull)
				return false;
			sift_shred_bool(sh, false);
			return true;
		default:
			return false;
	}
}

void
sift_shred_recheck_settle(SiftShred *sh, SiftShredRecheck *rc)
{
	if (rc->poison_grp >= 0)
	{
		sift_shred_poison(sh);
		rc->poison_grp = -1;
	}
}
