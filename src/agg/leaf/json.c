/*
 * agg/leaf/json.c - jsonb access paths and the leaves read through them
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "utils/fmgroids.h"

#include "agg/leaf/int.h"

/* the text keys of a `#>` / `#>>` path operand, false if not constant */
bool
path_keys(Node *n, List **keys)
{
	n = strip_relabel(n);
	if (IsA(n, Const) && !((Const *) n)->constisnull)
	{
		ArrayType  *arr = DatumGetArrayTypeP(((Const *) n)->constvalue);
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;

		if (ARR_ELEMTYPE(arr) != TEXTOID)
			return false;
		deconstruct_array_builtin(arr, TEXTOID, &elems, &nulls, &nelems);
		for (int i = 0; i < nelems; i++)
		{
			if (nulls[i])
				return false;
			*keys = lappend(*keys, makeString(TextDatumGetCString(elems[i])));
		}
		return true;
	}
	if (IsA(n, ArrayExpr))
	{
		foreach_ptr(Node, e, ((ArrayExpr *) n)->elements)
		{
			e = strip_relabel(e);
			if (!IsA(e, Const) || ((Const *) e)->constisnull)
				return false;
			*keys = lappend(*keys, makeString(TextDatumGetCString(((Const *) e)->constvalue)));
		}
		return true;
	}
	return false;
}

/* x -> 'k', x ->> 'k', x #> '{..}', x #>> '{..}' chains down to the root; the root is an attribute or a column of the unnested function */
bool
json_access(AggCtx *c, Node *e, JsonRef *r)
{
	Oid			fid;
	List	   *args;
	Node	   *a1;

	e = strip_relabel(e);
	if (IsA(e, SubscriptingRef))
	{
		/* x['k']: the same as x -> 'k' */
		SubscriptingRef *sr = (SubscriptingRef *) e;

		a1 = sr->refupperindexpr == NIL ? NULL : strip_relabel(linitial(sr->refupperindexpr));
		if (sr->refcontainertype != JSONBOID || sr->refassgnexpr != NULL || sr->reflowerindexpr != NIL ||
			list_length(sr->refupperindexpr) != 1 || !IsA(a1, Const) || ((Const *) a1)->constisnull || exprType(a1) != TEXTOID ||
			!json_access(c, (Node *) sr->refexpr, r) || r->as_text)
			return false;
		r->path = lappend(r->path, makeString(TextDatumGetCString(((Const *) a1)->constvalue)));
		return true;
	}
	fid = func_of(e);
	args = func_args(e);
	if (fid == F_JSONB_OBJECT_FIELD || fid == F_JSONB_OBJECT_FIELD_TEXT || fid == F_JSONB_EXTRACT_PATH ||
		fid == F_JSONB_EXTRACT_PATH_TEXT)
	{
		List	   *keys = NIL;

		if (list_length(args) < 2 || !json_access(c, linitial(args), r) || r->as_text)
			return false;
		a1 = strip_relabel(lsecond(args));
		if (fid == F_JSONB_OBJECT_FIELD || fid == F_JSONB_OBJECT_FIELD_TEXT)
		{
			if (!IsA(a1, Const) || ((Const *) a1)->constisnull || exprType(a1) != TEXTOID)
				return false;
			keys = list_make1(makeString(TextDatumGetCString(((Const *) a1)->constvalue)));
		}
		else if (!path_keys(a1, &keys))
			return false;
		r->path = list_concat(r->path, keys);
		r->as_text = fid == F_JSONB_OBJECT_FIELD_TEXT || fid == F_JSONB_EXTRACT_PATH_TEXT;
		return true;
	}
	if (IsA(e, Var))
	{
		Var		   *v = (Var *) e;

		if (v->varlevelsup != 0 || v->varno != c->func_rti || exprType(e) != JSONBOID)
			goto attr;
		r->att = 0;
		r->fcol = v->varattno;
		r->path = NIL;
		r->as_text = false;
		return true;
	}
attr:
	{
		int			att = attr_of_expr(c, e);

		if (att == 0 || exprType(e) != JSONBOID)
			return false;
		r->att = att;
		r->fcol = 0;
		r->path = NIL;
		r->as_text = false;
		return true;
	}
}

Oid
json_cast_target(Oid fid)
{
	switch (fid)
	{
		case F_FLOAT8_JSONB:
			return FLOAT8OID;
		case F_FLOAT4_JSONB:
			return FLOAT4OID;
		case F_INT4_JSONB:
			return INT4OID;
		case F_INT8_JSONB:
			return INT8OID;
		case F_BOOL_JSONB:
			return BOOLOID;
		case F_NUMERIC_JSONB:
			return NUMERICOID;
		default:
			return InvalidOid;
	}
}

/* jsonb_array_length of a top level array leaf: its count leaf */
int
array_length_leaf(AggCtx *c, Node *e)
{
	JsonRef		a = {0};

	if (!json_access(c, linitial(func_args(e)), &a) || a.as_text || a.att == 0)
		return -1;
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->kind == SCL_ARRAY && l->attno == a.att && l->parent < 0 && !l->lossy && paths_equal(l->path, a.path))
			return count_leaf(c, i);
	}
	return -1;
}

/* the jsonb value itself, but for its cast to a number or boolean: the value of jsonb_each */
int
each_value_cast_leaf(AggCtx *c, Node *e, Oid fid, Oid cast_to, const JsonRef *r)
{
	int			leaf;

	if (cast_to == InvalidOid || json_cast_target(fid) == InvalidOid || cast_to != exprType(e) || r->att != 0 || c->func_kind != FK_EACH ||
		r->fcol != 2 || (c->group < 0 && c->func_key == NULL))
		return -1;
	leaf = c->func_key ? member_leaf(c, cast_to) : child_leaf(c, c->group, "value", cast_to);
	c->cast_pending = leaf + 1;
	return leaf;
}

/* a typed jsonb path, as text or through a cast, of a stored column or of the unnested function's row */
int
json_path_leaf(AggCtx *c, Node *e, Oid fid, Oid cast_to, const JsonRef *r)
{
	Oid			want = cast_to != InvalidOid ? cast_to : r->as_text ? TEXTOID : InvalidOid;

	if (want == InvalidOid || exprType(e) != want || (cast_to != InvalidOid && cast_to != want))
		return -1;
	if (cast_to != InvalidOid && !(r->as_text || json_cast_target(fid) != InvalidOid))
		return -1;
	if (r->att > 0)
	{
		int			l = scalar_leaf(c, r->att, r->path, want);

		if (l >= 0)
		{
			c->txt_pending = cast_to == InvalidOid && r->as_text ? l + 1 : 0;
			c->io_pending = cast_to != InvalidOid && r->as_text ? l + 1 : 0;
			c->cast_pending = cast_to != InvalidOid && !r->as_text ? l + 1 : 0;
		}
		return l >= 0 ? l : key_leaf(c, r->att, r->path, want);
	}
	if (c->group < 0)
		return -1;
	if (c->func_kind == FK_ELEMENTS && r->fcol == 1 && list_length(r->path) == 1)
	{
		int			l = child_leaf(c, c->group, strVal(linitial(r->path)), want);

		if (l >= 0)
		{
			c->io_pending = cast_to != InvalidOid && r->as_text ? l + 1 : 0;
			c->cast_pending = cast_to != InvalidOid && !r->as_text ? l + 1 : 0;
		}
		return l;
	}
	if (c->func_kind == FK_EACH && r->fcol == 2 && r->path == NIL)
		return child_leaf(c, c->group, "value", want);
	return -1;
}
