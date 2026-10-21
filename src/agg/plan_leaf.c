/*
 * agg/plan_leaf.c - leaf references: what an expression of the query reads from the column source
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/pathnode.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/topn.h"
#include "agg/spec.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/plan_int.h"

/* ---------- small helpers ---------- */

SiftSrcLeaf *
leaf_of(AggCtx *c, int id)
{
	return &c->lay->leaf[id];
}

int
leaf_level(AggCtx *c, int id)
{
	return leaf_of(c, id)->parent >= 0;
}

/* the name of an index attribute: its heap column, or the index expression */
static const char *
att_name(AggCtx *c, int attno)
{
	Node	   *x = list_nth(c->attexpr, attno - 1);
	List	   *rtable = c->rtable,
			   *dpctx = c->dpctx;
	char	   *text;

	if (x == NULL || IsA(x, Var))
		return x == NULL ? "?" : get_attname(c->heapoid, ((Var *) x)->varattno, false);
	/* the Vars of an index expression are the table's of the query, not of the sub-query whose predicate is being compiled */
	c->rtable = NIL;
	c->dpctx = NIL;
	text = expr_text(c, x);
	c->rtable = rtable;
	c->dpctx = dpctx;
	return text;
}

const char *
leaf_name(AggCtx *c, int id)
{
	StringInfoData s;
	SiftSrcLeaf *l = leaf_of(c, id);
	const char *att = att_name(c, l->attno);

	initStringInfo(&s);
	if (l->parent >= 0)
	{
		const char *p = leaf_name(c, l->parent);

		appendStringInfo(&s, leaf_of(c, l->parent)->kind == SCL_OBJECT ? "%s.*.%s" : "%s[].%s", p, l->name);
		return s.data;
	}
	if (l->name[0] == '#')
		appendStringInfoString(&s, strcmp(l->name, "#typeof") == 0 ? "jsonb_typeof(" : "jsonb_array_length(");
	appendStringInfoString(&s, att);
	foreach_ptr(String, k, l->path)
		appendStringInfo(&s, ".%s", strVal(k));
	if (l->name[0] == '#')
		appendStringInfoChar(&s, ')');
	return s.data;
}

/* Local expression slots share parameter numbers with PostgreSQL, but not their identity. */
bool
null_input_param(Node *n)
{
	return IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC &&
		((Param *) n)->paramid == SIFT_AGG_NULL_INPUT && ((Param *) n)->location == SIFT_AGG_NULL_INPUT_LOC;
}

/*
 * An expression the executor may evaluate once per scan: no Vars, no volatile function, no executor parameter. A non-NULL context (the query's
 * AggCtx) lets the EXISTS sub-plans through, and the outputs of the ARRAY sub-selects a lookup table asks once: the clause was compiled, and
 * compile_exists and array_initplan_ok proved the ones it kept.
 */
bool
has_exec_param_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubPlan) && context)
		return false;
	if (IsA(node, Param) && ((Param *) node)->paramkind == PARAM_EXEC)
		return !context || !array_initplan_ok(context, (Param *) node);
	if (IsA(node, SubPlan) || IsA(node, SubLink) || IsA(node, Aggref) || IsA(node, WindowFunc))
		return true;
	return expression_tree_walker(node, has_exec_param_walker, context);
}

bool
pseudo_const(Node *n)
{
	return !contain_var_clause(n) && !contain_volatile_functions(n) && !has_exec_param_walker(n, NULL);
}

Node *
strip_relabel(Node *n)
{
	while (n && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

Oid
func_of(Node *n)
{
	if (IsA(n, FuncExpr))
		return ((FuncExpr *) n)->funcid;
	if (IsA(n, OpExpr))
	{
		set_opfuncid((OpExpr *) n);
		return ((OpExpr *) n)->opfuncid;
	}
	return InvalidOid;
}

List *
func_args(Node *n)
{
	return IsA(n, FuncExpr) ? ((FuncExpr *) n)->args : IsA(n, OpExpr) ? ((OpExpr *) n)->args : NIL;
}

bool
paths_equal(List *a, List *b)
{
	if (list_length(a) != list_length(b))
		return false;
	for (int i = 0; i < list_length(a); i++)
		if (strcmp(strVal(list_nth(a, i)), strVal(list_nth(b, i))) != 0)
			return false;
	return true;
}

/* ---------- leaf references ---------- */

/* the attribute whose index (table) expression is e, 0 if none */
int
attr_of_expr(AggCtx *c, Node *e)
{
	for (int a = 0; a < list_length(c->attexpr); a++)
	{
		Node	   *x = list_nth(c->attexpr, a);
		Node	   *y = c->attalias ? list_nth(c->attalias, a) : NULL;

		if ((x && equal(e, x)) || (y && equal(e, y)))
			return a + 1;
	}
	return 0;
}

static int
scalar_leaf(AggCtx *c, int att, List *path, Oid typid)
{
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == att && l->parent < 0 && l->kind == SCL_SCALAR && !l->lossy && l->name[0] != '#' && paths_equal(l->path, path) &&
			l->typid == typid)
			return i;
	}
	return -1;
}

int
child_leaf(AggCtx *c, int group, const char *name, Oid typid)
{
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->parent == group && !l->lossy && strcmp(l->name, name) == 0 && l->typid == typid)
			return i;
	}
	return -1;
}

/*
 * `(x->>'k')::float8` of an object leaf with free keys (`metrics.*`) at the row level: a scalar leaf after the source's, which the
 * executor builds from the "value" of the element of each row whose "key" is k.
 */
static SiftSrcLeaf *
vleaf_add(AggCtx *c, int att, List *path, SiftSrcVecType vtype, Oid typid)
{
	SiftSrcLayout *lay = palloc(sizeof(SiftSrcLayout));
	SiftSrcLeaf *v;

	*lay = *c->lay;
	lay->leaf = palloc((lay->nleaves + 1) * sizeof(SiftSrcLeaf));
	memcpy(lay->leaf, c->lay->leaf, lay->nleaves * sizeof(SiftSrcLeaf));
	v = &lay->leaf[lay->nleaves];
	memset(v, 0, sizeof(SiftSrcLeaf));
	v->id = lay->nleaves++;
	v->attno = att;
	v->parent = -1;
	v->kind = SCL_SCALAR;
	v->vtype = vtype;
	v->typid = typid;
	v->path = path;
	c->lay = lay;
	return v;
}

static int
key_leaf(AggCtx *c, int att, List *path, Oid typid)
{
	int			n = list_length(path),
				obj = -1,
				val;
	SiftSrcLeaf *v;

	if (n == 0 || c->nvleaf >= SIFT_AGG_MAX_VLEAVES || c->lay->nleaves >= SIFT_COLSRC_MAX_LEAVES)
		return -1;
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->kind == SCL_OBJECT && l->attno == att && l->parent < 0 && !l->lossy && list_length(l->path) == n - 1 &&
			paths_equal(l->path, list_copy_head(path, n - 1)))
			obj = i;
	}
	if (obj < 0 || (val = child_leaf(c, obj, "value", typid)) < 0)
		return -1;
	/* an expression is matched more than once (when its inputs are collected and when they are replaced): the same member is one leaf */
	for (int j = 0; j < c->nvleaf; j++)
		if (!c->vleaf[j].count && !c->vleaf[j].types && c->vleaf[j].obj == obj && strcmp(c->vleaf[j].key, strVal(llast(path))) == 0 &&
			leaf_of(c, c->lay->nleaves - c->nvleaf + j)->typid == typid)
			return c->lay->nleaves - c->nvleaf + j;
	v = vleaf_add(c, att, path, leaf_of(c, val)->vtype, typid);
	c->vleaf[c->nvleaf].obj = obj;
	c->vleaf[c->nvleaf].count = false;
	c->vleaf[c->nvleaf].types = false;
	c->vleaf[c->nvleaf++].key = pstrdup(strVal(llast(path)));
	return v->id;
}

/*
 * `jsonb_typeof(x->'k')` of a typed scalar leaf: a scalar leaf after the source's, text, whose values the executor takes from the JSON types of the
 * values of the leaf (SiftAggVLeaf.types): the name of the type, NULL where the path has no value.
 */
static int
types_leaf(AggCtx *c, Node *call)
{
	int			base = typeof_leaf(c, linitial(func_args(call)));
	SiftSrcLeaf *v;

	if (base < 0 || !source_can_types(c) || c->stats.nokinds[base])
		return -1;
	for (int j = 0; j < c->nvleaf; j++)
		if (c->vleaf[j].types && c->vleaf[j].obj == base)
			return c->lay->nleaves - c->nvleaf + j;
	if (c->nvleaf >= SIFT_AGG_MAX_VLEAVES || c->lay->nleaves >= SIFT_COLSRC_MAX_LEAVES)
		return -1;
	v = vleaf_add(c, leaf_of(c, base)->attno, leaf_of(c, base)->path, SCV_CODE, TEXTOID);
	strlcpy(v->name, "#typeof", NAMEDATALEN);	/* not the scalar of the path: scalar_leaf skips it */
	c->vleaf[c->nvleaf].obj = base;
	c->vleaf[c->nvleaf].count = false;
	c->vleaf[c->nvleaf].types = true;
	c->vleaf[c->nvleaf++].key = pstrdup("");
	return v->id;
}

/* `jsonb_array_length(x->'k')` of an array leaf: a scalar leaf after the source's, whose values the executor takes from the element counts of the array */
static int
count_leaf(AggCtx *c, int grp)
{
	SiftSrcLeaf *v;
	int			nbase = c->lay->nleaves - c->nvleaf;

	for (int j = 0; j < c->nvleaf; j++)
		if (c->vleaf[j].count && c->vleaf[j].obj == grp)
			return nbase + j;
	if (c->nvleaf >= SIFT_AGG_MAX_VLEAVES || c->lay->nleaves >= SIFT_COLSRC_MAX_LEAVES)
		return -1;
	v = vleaf_add(c, leaf_of(c, grp)->attno, leaf_of(c, grp)->path, SCV_I32, INT4OID);
	strlcpy(v->name, "#count", NAMEDATALEN);	/* not the scalar of the path: scalar_leaf skips it */
	c->vleaf[c->nvleaf].obj = grp;
	c->vleaf[c->nvleaf].count = true;
	c->vleaf[c->nvleaf].types = false;
	c->vleaf[c->nvleaf++].key = pstrdup("");
	return v->id;
}

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

/* the member of the row's object that `LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'` picks (NULL where the row has none), as a row level leaf */
static int
member_leaf(AggCtx *c, Oid typid)
{
	const SiftSrcLeaf *obj = leaf_of(c, c->func_obj);

	if (child_leaf(c, c->func_obj, "value", typid) < 0)
		return -1;
	c->func_ovf = bms_add_member(c->func_ovf, c->func_obj);
	return key_leaf(c, obj->attno, lappend(list_copy(obj->path), makeString(pstrdup(c->func_key))), typid);
}

/* a column of the unnested function, as a leaf */
static int
func_var_leaf(AggCtx *c, Var *v)
{
	RangeTblFunction *f = c->func;

	if (v->varlevelsup != 0 || v->varno != c->func_rti || c->group < 0)
		return -1;
	switch (c->func_kind)
	{
		case FK_RECORDSET:
			if (v->varattno < 1 || v->varattno > list_length(f->funccolnames) ||
				v->vartype != list_nth_oid(f->funccoltypes, v->varattno - 1))
				return -1;
			{
				int			leaf = child_leaf(c, c->group, strVal(list_nth(f->funccolnames, v->varattno - 1)), v->vartype);

				c->rec_pending = leaf + 1;
				return leaf;
			}
		case FK_EACH:
		case FK_EACH_TEXT:
			if (v->varattno == 1)
				return child_leaf(c, c->group, "key", TEXTOID);
			/* jsonb_each_text: the text of the value is the text of the member; a number has none in a text leaf, only in an astext one */
			if (c->func_kind == FK_EACH_TEXT && v->varattno == 2 && v->vartype == TEXTOID)
				return child_leaf(c, c->group, "value", TEXTOID);
			return -1;
		case FK_ELEMENTS_TEXT:		/* the element of an array of strings is the one unnamed child of the array */
			return v->varattno == 1 && v->vartype == TEXTOID ? child_leaf(c, c->group, "", TEXTOID) : -1;
		default:
			return -1;
	}
}

/*
 * The leaf an expression reads, -1 if it is not exactly a stored value: an attribute or index expression, a Var of the
 * unnested function's row type, a typed jsonb path of either, or the length of an array leaf.
 */
int
match_leaf(AggCtx *c, Node *e)
{
	JsonRef		r = {0};
	Oid			fid,
				cast_to = InvalidOid;
	Node	   *inner;
	int			att;

	c->rec_pending = 0;
	e = strip_relabel(e);
	if (IsA(e, Var) && ((Var *) e)->varno == c->func_rti && c->func_rti != 0)
		return func_var_leaf(c, (Var *) e);
	if ((att = attr_of_expr(c, e)) > 0)
		return scalar_leaf(c, att, NIL, exprType(e));
	fid = func_of(e);
	if (fid == F_JSONB_ARRAY_LENGTH)
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
	if (fid == F_JSONB_TYPEOF)
		return types_leaf(c, e);
	inner = e;
	if (IsA(e, CoerceViaIO))
	{
		cast_to = exprType(e);
		inner = strip_relabel((Node *) ((CoerceViaIO *) e)->arg);
	}
	else if (json_cast_target(fid) != InvalidOid)
	{
		cast_to = json_cast_target(fid);
		inner = strip_relabel(linitial(func_args(e)));
	}
	if (cast_to != InvalidOid && cast_to == exprType(e) && IsA(inner, Var) && ((Var *) inner)->varno == c->func_rti &&
		c->func_rti != 0 && c->func_kind == FK_EACH_TEXT && ((Var *) inner)->varattno == 2 && IsA(e, CoerceViaIO))
		return c->group < 0 ? -1 : child_leaf(c, c->group, "value", cast_to);
	if (!json_access(c, inner, &r))
		return -1;
	if (IsA(inner, Var) || attr_of_expr(c, inner) > 0)
	{
		/* the jsonb value itself, but for its cast to a number or boolean: the value of jsonb_each */
		if (cast_to != InvalidOid && json_cast_target(fid) != InvalidOid && cast_to == exprType(e) && r.att == 0 && c->func_kind == FK_EACH &&
			r.fcol == 2 && (c->group >= 0 || c->func_key != NULL))
			return c->func_key ? member_leaf(c, cast_to) : child_leaf(c, c->group, "value", cast_to);
		return -1;
	}
	{
		Oid			want = cast_to != InvalidOid ? cast_to : r.as_text ? TEXTOID : InvalidOid;

		if (want == InvalidOid || exprType(e) != want || (cast_to != InvalidOid && cast_to != want))
			return -1;
		if (cast_to != InvalidOid && !(r.as_text || json_cast_target(fid) != InvalidOid))
			return -1;
		if (r.att > 0)
		{
			int			l = scalar_leaf(c, r.att, r.path, want);

			return l >= 0 ? l : key_leaf(c, r.att, r.path, want);
		}
		if (c->group < 0)
			return -1;
		if (c->func_kind == FK_ELEMENTS && r.fcol == 1 && list_length(r.path) == 1)
			return child_leaf(c, c->group, strVal(linitial(r.path)), want);
		if (c->func_kind == FK_EACH && r.fcol == 2 && r.path == NIL)
			return child_leaf(c, c->group, "value", want);
	}
	return -1;
}

/*
 * `x->>'k' IS [NOT] NULL` on a path whose leaf has another type than text (`attributes->normalized bool`) is the null test of that
 * leaf (`->>` turns a JSON null into NULL, so both are NULL for a missing key and a JSON null, both not NULL for a value). The
 * argument becomes the typed access `(x->>'k')::bool`, which match_leaf knows; a value the leaf could not hold makes the scan raise
 * the overflow error instead of being a non-NULL text.
 */
static Node *
nulltest_mutator(Node *n, AggCtx *c)
{
	NullTest   *nt;
	JsonRef		r = {0};
	Node	   *arg;

	if (n == NULL)
		return NULL;
	if (IsA(n, NullTest) && !((NullTest *) n)->argisrow && exprType(strip_relabel((Node *) ((NullTest *) n)->arg)) == TEXTOID)
	{
		nt = (NullTest *) n;
		arg = strip_relabel((Node *) nt->arg);
		if ((func_of(arg) == F_JSONB_OBJECT_FIELD_TEXT || func_of(arg) == F_JSONB_EXTRACT_PATH_TEXT) && json_access(c, arg, &r) &&
			r.as_text && r.att > 0 && scalar_leaf(c, r.att, r.path, TEXTOID) < 0)
			for (int i = 0; i < c->lay->nleaves; i++)
			{
				SiftSrcLeaf *l = leaf_of(c, i);

				if (l->attno == r.att && l->parent < 0 && l->kind == SCL_SCALAR && !l->lossy && paths_equal(l->path, r.path))
				{
					CoerceViaIO *io = makeNode(CoerceViaIO);
					NullTest   *out = makeNode(NullTest);

					io->arg = (Expr *) arg;
					io->resulttype = l->typid;
					io->coerceformat = COERCE_EXPLICIT_CAST;
					io->location = -1;
					*out = *nt;
					out->arg = (Expr *) io;
					return (Node *) out;
				}
			}
	}
	return expression_tree_mutator(n, nulltest_mutator, c);
}

/* A placeholder computed inside the nullable input becomes NULL when that whole input is absent. */
static Node *
nullable_expr_mutator(Node *n, AggCtx *c)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, PlaceHolderVar))
	{
		PlaceHolderVar *ph = (PlaceHolderVar *) n;

		if (ph->phlevelsup == 0 && !bms_is_empty(ph->phnullingrels) && bms_is_subset(ph->phnullingrels, c->nulling))
		{
			CaseExpr   *out = makeNode(CaseExpr);
			CaseWhen   *when = makeNode(CaseWhen);
			Param	   *missing = makeNode(Param);

			missing->paramkind = PARAM_EXEC;
			missing->paramid = SIFT_AGG_NULL_INPUT;
			missing->paramtype = BOOLOID;
			missing->paramtypmod = -1;
			missing->location = SIFT_AGG_NULL_INPUT_LOC;
			when->expr = (Expr *) missing;
			when->result = (Expr *) makeNullConst(exprType(n), exprTypmod(n), exprCollation(n));
			when->location = -1;
			out->casetype = exprType(n);
			out->casecollid = exprCollation(n);
			out->args = list_make1(when);
			out->defresult = (Expr *) nullable_expr_mutator((Node *) ph->phexpr, c);
			out->location = -1;
			return (Node *) out;
		}
	}
	return expression_tree_mutator(n, nullable_expr_mutator, c);
}

Node *
normalize_expr(AggCtx *c, Node *n)
{
	if (c->nulling)
	{
		n = nullable_expr_mutator(n, c);
		n = remove_nulling_relids(n, c->nulling, NULL);
	}
#if PG_VERSION_NUM >= 180000
	/* the group expressions of grouping sets are marked as nulled by the grouping step (Vars with the RTE_GROUP in varnullingrels) */
	if (c->root->group_rtindex > 0)
		n = remove_nulling_relids(n, bms_make_singleton(c->root->group_rtindex), NULL);
#endif
	return nulltest_mutator(n, c);
}
