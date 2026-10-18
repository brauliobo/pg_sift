/*
 * sift_aggplan.c - planner side of SiftAgg: proves that a single-table aggregate query (optionally unnesting one array or
 * object of the column source) is answered by the column source with the plain plan's result, and adds a
 * Custom Scan (SiftAgg) path for it in create_upper_paths_hook (UPPERREL_GROUP_AGG). Everything not proven keeps the
 * normal plan. Execution is in sift_agg.c; the plan travels as a SiftAggSpec (sift_aggspec.h).
 *
 * Matching works on the query tree (root->parse), not on the paths: the FROM list must be the table (and at most one lateral
 * set-returning function over one of its jsonb leaves), the quals are compiled to bitmap programs over leaves, and the group
 * keys and aggregates to accumulators. A leaf reference is an expression that equals what the column source stored: an
 * attribute or index expression, a jsonb path of it with the declared type (`(x->>'k')::float8`, `x #>> '{a,b}'`), a field
 * of the unnested function's rows. Any boolean or scalar expression of ONE leaf that is a dictionary (text) or small integer
 * leaf is evaluated by the executor with PostgreSQL's own expression evaluator on every distinct value of the leaf, so its
 * semantics (collations, NULL handling, casts, CASE) are PostgreSQL's by construction.
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "sift_agg.h"
#include "sift_aggspec.h"
#include "sift_colsrc.h"

typedef enum FuncKind
{
	FK_NONE,
	FK_RECORDSET,				/* jsonb_to_recordset(array) AS t(f type, ...) */
	FK_ELEMENTS,				/* jsonb_array_elements(array) */
	FK_EACH,					/* jsonb_each(object) */
	FK_EACH_TEXT				/* jsonb_each_text(object) */
} FuncKind;

/* a table joined by equality to leaves (SiftAggJoin) and the combos that evaluate expressions of leaves and its columns (SiftAggCombo) */
typedef struct JoinCtx
{
	Index		rti;
	bool		left;
	List	   *clauses;		/* the equalities `column = leaf` */
	SiftAggJoin j;
	int			combo;
} JoinCtx;

typedef struct SlotReq
{
	Node	   *expr;
	bool		is_key;
	int			key;
	int			agg;
} SlotReq;

typedef struct ComboCtx
{
	int			join;			/* index in AggCtx.joins, -1 none */
	int			nl;
	int			leaf[SIFT_AGG_MAX_CLEAVES];
	bool		typed[SIFT_AGG_MAX_CLEAVES];
	Oid			ptype[SIFT_AGG_MAX_CLEAVES];
	int32		ptypmod[SIFT_AGG_MAX_CLEAVES];
	Oid			pcoll[SIFT_AGG_MAX_CLEAVES];
	List	   *slots;			/* SlotReq */
} ComboCtx;

typedef struct AggCtx
{
	PlannerInfo *root;
	RelOptInfo *rel;
	Index		relid;
	Oid			heapoid;
	Oid			indexoid;
	SiftSrcLayout *lay;
	List	   *attexpr;		/* per attribute of the source: its expression with varno = relid (NULL: none) */
	Index		func_rti;
	FuncKind	func_kind;
	RangeTblFunction *func;
	int			group;			/* leaf of the unnested array/object, -1 */
	Bitmapset  *used;			/* leaves the spec reads */
	List	   *dpctx;			/* deparse context of expr_text */
	bool		other_group;	/* a leaf of an array the query does not unnest is read */
	int			npred;			/* ids handed out to predicate nodes */
	List	   *joins;			/* JoinCtx */
	List	   *combos;			/* ComboCtx */
	int			nvleaf;			/* SiftAggSpec.vleaf: leaves c->lay has after the source's */
	SiftAggVLeaf vleaf[SIFT_AGG_MAX_VLEAVES];
	List	   *rtable;			/* the range table expr_text resolves Vars in; NIL: the query's */
	Bitmapset  *elem_used;		/* leaves read inside an existential predicate (SAP_ANY_ELEM) */
	double		elem_work;		/* element predicate evaluations per row of those, for the cost */
	SiftSrcStats stats;
} AggCtx;

/* ---------- small helpers ---------- */

static SiftSrcLeaf *
leaf_of(AggCtx *c, int id)
{
	return &c->lay->leaf[id];
}

static int
leaf_level(AggCtx *c, int id)
{
	return leaf_of(c, id)->parent >= 0;
}

static char *expr_text(AggCtx *c, Node *n);

/* the name of an index attribute: its heap column, or the index expression */
static const char *
att_name(AggCtx *c, int attno)
{
	Node	   *x = list_nth(c->attexpr, attno - 1);

	return x == NULL ? "?" : IsA(x, Var) ? get_attname(c->heapoid, ((Var *) x)->varattno, false) : expr_text(c, x);
}

static const char *
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
	appendStringInfoString(&s, att);
	foreach_ptr(String, k, l->path)
		appendStringInfo(&s, ".%s", strVal(k));
	return s.data;
}

/*
 * An expression the executor may evaluate once per scan: no Vars, no volatile function, no executor parameter. A non-NULL context
 * lets the EXISTS sub-plans through: the clause was compiled, and compile_exists proved the ones it kept.
 */
static bool
has_exec_param_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubPlan) && context)
		return false;
	if (IsA(node, Param) && ((Param *) node)->paramkind == PARAM_EXEC)
		return true;
	if (IsA(node, SubPlan) || IsA(node, SubLink) || IsA(node, Aggref) || IsA(node, WindowFunc))
		return true;
	return expression_tree_walker(node, has_exec_param_walker, context);
}

static bool
pseudo_const(Node *n)
{
	return !contain_var_clause(n) && !contain_volatile_functions(n) && !has_exec_param_walker(n, NULL);
}

static Node *
strip_relabel(Node *n)
{
	while (n && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

static Oid
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

static List *
func_args(Node *n)
{
	return IsA(n, FuncExpr) ? ((FuncExpr *) n)->args : IsA(n, OpExpr) ? ((OpExpr *) n)->args : NIL;
}

static bool
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
static int
attr_of_expr(AggCtx *c, Node *e)
{
	for (int a = 0; a < list_length(c->attexpr); a++)
	{
		Node	   *x = list_nth(c->attexpr, a);

		if (x && equal(e, x))
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

		if (l->attno == att && l->parent < 0 && l->kind == SCL_SCALAR && !l->lossy && paths_equal(l->path, path) &&
			l->typid == typid)
			return i;
	}
	return -1;
}

static int
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
static int
key_leaf(AggCtx *c, int att, List *path, Oid typid)
{
	int			n = list_length(path),
				obj = -1,
				val;
	SiftSrcLayout *lay;
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
	lay = palloc(sizeof(SiftSrcLayout));
	*lay = *c->lay;
	lay->leaf = palloc((lay->nleaves + 1) * sizeof(SiftSrcLeaf));
	memcpy(lay->leaf, c->lay->leaf, lay->nleaves * sizeof(SiftSrcLeaf));
	v = &lay->leaf[lay->nleaves];
	memset(v, 0, sizeof(SiftSrcLeaf));
	v->id = lay->nleaves++;
	v->attno = att;
	v->parent = -1;
	v->kind = SCL_SCALAR;
	v->vtype = leaf_of(c, val)->vtype;
	v->typid = typid;
	v->path = path;
	c->lay = lay;
	c->vleaf[c->nvleaf].obj = obj;
	c->vleaf[c->nvleaf++].key = pstrdup(strVal(llast(path)));
	return v->id;
}

typedef struct JsonRef
{
	int			att;			/* attribute holding the jsonb root; 0: a column of the unnested function */
	int			fcol;			/* that column (1-based) */
	List	   *path;
	bool		as_text;
	Oid			cast_to;
} JsonRef;

/* the text keys of a `#>` / `#>>` path operand, false if not constant */
static bool
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
static bool
json_access(AggCtx *c, Node *e, JsonRef *r)
{
	Oid			fid;
	List	   *args;
	Node	   *a1;

	e = strip_relabel(e);
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

static Oid
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

static int match_leaf(AggCtx *c, Node *e);

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
			return child_leaf(c, c->group, strVal(list_nth(f->funccolnames, v->varattno - 1)), v->vartype);
		case FK_EACH:
		case FK_EACH_TEXT:
			if (v->varattno == 1)
				return child_leaf(c, c->group, "key", TEXTOID);
			return -1;
		default:
			return -1;
	}
}

/*
 * The leaf an expression reads, -1 if it is not exactly a stored value: an attribute or index expression, a Var of the
 * unnested function's row type, a typed jsonb path of either, or the length of an array leaf.
 */
static int
match_leaf(AggCtx *c, Node *e)
{
	JsonRef		r = {0};
	Oid			fid,
				cast_to = InvalidOid;
	Node	   *inner;
	int			att;

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

			if (l->kind == SCL_ARRAY && l->attno == a.att && paths_equal(l->path, a.path))
				return i;
		}
		return -1;
	}
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
		return -1;				/* the jsonb value itself */
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

static Node *
normalize_expr(AggCtx *c, Node *n)
{
#if PG_VERSION_NUM >= 180000
	/* the group expressions of grouping sets are marked as nulled by the grouping step (Vars with the RTE_GROUP in varnullingrels) */
	if (c->root->group_rtindex > 0)
		n = remove_nulling_relids(n, bms_make_singleton(c->root->group_rtindex), NULL);
#endif
	return nulltest_mutator(n, c);
}

/* ---------- expressions of one leaf ---------- */

typedef struct LeafWalk
{
	AggCtx	   *c;
	int			leaf;			/* -1: none yet */
	Oid			ptype;			/* type of the references */
	int32		ptypmod;
	Oid			pcoll;
	bool		bad;
} LeafWalk;

static bool
leaf_walker(Node *n, LeafWalk *w)
{
	int			id;

	if (n == NULL || w->bad)
		return false;
	if (IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType))
	{
		if ((id = match_leaf(w->c, n)) >= 0)
		{
			if (w->leaf >= 0 && (w->leaf != id || w->ptype != exprType(n)))
				w->bad = true;
			w->leaf = id;
			w->ptype = exprType(n);
			w->ptypmod = exprTypmod(n);
			w->pcoll = exprCollation(n);
			return false;
		}
		if (IsA(n, Var))
		{
			w->bad = true;		/* a column the source does not hold in this form */
			return false;
		}
	}
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
	{
		w->bad = true;
		return false;
	}
	if (IsA(n, SubPlan) || IsA(n, SubLink) || IsA(n, Aggref) || IsA(n, WindowFunc) || IsA(n, GroupingFunc) ||
		IsA(n, PlaceHolderVar) || IsA(n, CurrentOfExpr) || IsA(n, NextValueExpr) || IsA(n, SQLValueFunction) ||
		IsA(n, MinMaxExpr) || IsA(n, SetToDefault))
	{
		if (!IsA(n, MinMaxExpr) && !IsA(n, SQLValueFunction))
		{
			w->bad = true;
			return false;
		}
	}
	return expression_tree_walker(n, leaf_walker, w);
}

/* the one leaf an expression reads (and the type it reads it as); false if it reads none, several, or something else */
static bool
single_leaf(AggCtx *c, Node *e, int *leaf, Oid *ptype, int32 *ptypmod, Oid *pcoll)
{
	LeafWalk	w = {c, -1, InvalidOid, -1, InvalidOid, false};

	if (contain_volatile_functions(e))
		return false;
	leaf_walker(e, &w);
	if (w.bad || w.leaf < 0)
		return false;
	*leaf = w.leaf;
	*ptype = w.ptype;
	*ptypmod = w.ptypmod;
	*pcoll = w.pcoll;
	return true;
}

typedef struct SubstCtx
{
	AggCtx	   *c;
	Param	   *param;
} SubstCtx;

static Node *
subst_mutator(Node *n, SubstCtx *s)
{
	if (n == NULL)
		return NULL;
	if ((IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType)) &&
		match_leaf(s->c, n) >= 0)
		return (Node *) copyObject(s->param);
	return expression_tree_mutator(n, subst_mutator, s);
}

/* e with the leaf reference replaced by Param 0 (PARAM_EXEC) of the type it had */
static Node *
substitute_leaf(AggCtx *c, Node *e, Oid ptype, int32 ptypmod, Oid pcoll)
{
	Param	   *p = makeNode(Param);
	SubstCtx	s = {c, p};

	p->paramkind = PARAM_EXEC;
	p->paramid = 0;
	p->paramtype = ptype;
	p->paramtypmod = ptypmod;
	p->paramcollid = pcoll;
	p->location = -1;
	return subst_mutator(e, &s);
}

static bool
lut_leaf_ok(AggCtx *c, int leaf)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);

	return l->kind == SCL_SCALAR && (l->vtype == SCV_CODE || l->vtype == SCV_I32);
}

/* ---------- predicates ---------- */

typedef enum TypeClass
{
	TC_NONE,
	TC_INT,
	TC_DATE,
	TC_TS,
	TC_TSTZ,
	TC_FLOAT,
	TC_BOOL,
	TC_NUMERIC
} TypeClass;

static TypeClass
type_class(Oid t)
{
	switch (t)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
			return TC_INT;
		case DATEOID:
			return TC_DATE;
		case TIMESTAMPOID:
			return TC_TS;
		case TIMESTAMPTZOID:
			return TC_TSTZ;
		case FLOAT4OID:
		case FLOAT8OID:
			return TC_FLOAT;
		case BOOLOID:
			return TC_BOOL;
		case NUMERICOID:
			return TC_NUMERIC;
		default:
			return TC_NONE;
	}
}

/* the comparison a builtin operator is: SiftCmpOp, or -1 */
static int
builtin_cmp(Oid opno, Oid *lt, Oid *rt)
{
	char	   *name;
	int			op = -1;

	if (opno >= FirstGenbkiObjectId)
		return -1;
	name = get_opname(opno);
	if (name == NULL)
		return -1;
	if (strcmp(name, "=") == 0)
		op = SCMP_EQ;
	else if (strcmp(name, "<>") == 0)
		op = SCMP_NE;
	else if (strcmp(name, "<") == 0)
		op = SCMP_LT;
	else if (strcmp(name, "<=") == 0)
		op = SCMP_LE;
	else if (strcmp(name, ">") == 0)
		op = SCMP_GT;
	else if (strcmp(name, ">=") == 0)
		op = SCMP_GE;
	if (op >= 0)
		op_input_types(opno, lt, rt);
	return op;
}

static int
commute(int op)
{
	switch (op)
	{
		case SCMP_LT:
			return SCMP_GT;
		case SCMP_LE:
			return SCMP_GE;
		case SCMP_GT:
			return SCMP_LT;
		case SCMP_GE:
			return SCMP_LE;
		default:
			return op;
	}
}

static SiftAggPred *
new_pred(AggCtx *c, SiftAggPredKind kind)
{
	SiftAggPred *p = palloc0(sizeof(SiftAggPred));

	p->kind = kind;
	p->id = c->npred++;
	p->leaf = -1;
	p->desc = pstrdup("");
	return p;
}

/* the expression as EXPLAIN shows it, its Vars resolved through the query's range table */
static char *
expr_text(AggCtx *c, Node *n)
{
	if (c->dpctx == NIL)
	{
		PlannedStmt *stmt = makeNode(PlannedStmt);

		stmt->rtable = c->rtable ? c->rtable : c->root->parse->rtable;
		c->dpctx = deparse_context_for_plan_tree(stmt, select_rtable_names_for_explain(stmt->rtable, NULL));
	}
	return deparse_expression(n, c->dpctx, true, false);
}

static const char *
cmp_text(int op)
{
	static const char *const t[] = {"=", "<>", "<", "<=", ">", ">="};

	return t[op];
}

static void
use_leaf(AggCtx *c, int leaf)
{
	c->used = bms_add_member(c->used, leaf);
	if (leaf_of(c, leaf)->parent >= 0 && leaf_of(c, leaf)->parent != c->group)
		c->other_group = true;
}

static SiftAggPred *
make_leaf_pred(AggCtx *c, SiftAggPredKind kind, int leaf)
{
	SiftAggPred *p = new_pred(c, kind);

	p->leaf = leaf;
	p->level = leaf_level(c, leaf);
	use_leaf(c, leaf);
	return p;
}

/* ---------- lookup joins and combos ---------- */

static JoinCtx *
join_of_var(AggCtx *c, Node *n)
{
	Var		   *v;

	if (!IsA(n, Var))
		return NULL;
	v = (Var *) n;
	if (v->varlevelsup == 0 && v->varattno > 0)
		foreach_ptr(JoinCtx, jc, c->joins)
			if (jc->rti == v->varno)
			return jc;
	return NULL;
}

/* the index of column v of the joined table among the columns the expressions read; -1 when there are too many */
static int
join_col(JoinCtx *jc, Var *v)
{
	SiftAggJoin *j = &jc->j;

	for (int i = 0; i < j->ncol; i++)
		if (j->colatt[i] == v->varattno)
			return i;
	if (j->ncol == SIFT_AGG_MAX_JCOLS)
		return -1;
	j->colatt[j->ncol] = v->varattno;
	j->coltype[j->ncol] = v->vartype;
	j->coltypmod[j->ncol] = v->vartypmod;
	j->colcoll[j->ncol] = v->varcollid;
	return j->ncol++;
}

/* the position of leaf `id` in the combo (added if new), its type fixed by the first reference that has one; -1 when it does not fit */
static int
combo_leaf(ComboCtx *cb, int id, bool typed, Oid ptype, int32 ptypmod, Oid pcoll)
{
	int			p = 0;

	while (p < cb->nl && cb->leaf[p] != id)
		p++;
	if (p == cb->nl)
	{
		if (p == SIFT_AGG_MAX_CLEAVES)
			return -1;
		cb->leaf[cb->nl++] = id;
		cb->ptype[p] = ptype;
		cb->ptypmod[p] = ptypmod;
		cb->pcoll[p] = pcoll;
	}
	else if (typed && cb->typed[p] && (cb->ptype[p] != ptype || cb->ptypmod[p] != ptypmod || cb->pcoll[p] != pcoll))
		return -1;
	if (typed && !cb->typed[p])
	{
		cb->typed[p] = true;
		cb->ptype[p] = ptype;
		cb->ptypmod[p] = ptypmod;
		cb->pcoll[p] = pcoll;
	}
	return p;
}

static ComboCtx *
new_combo(AggCtx *c, int join)
{
	ComboCtx   *cb = palloc0(sizeof(ComboCtx));

	cb->join = join;
	c->combos = lappend(c->combos, cb);
	return cb;
}

static int
combo_index(AggCtx *c, ComboCtx *cb)
{
	foreach_ptr(ComboCtx, x, c->combos)
		if (x == cb)
			return foreach_current_index(x);
	return -1;
}

static int
combo_level(AggCtx *c, ComboCtx *cb)
{
	int			lvl = 0;

	for (int p = 0; p < cb->nl; p++)
		lvl = Max(lvl, leaf_level(c, cb->leaf[p]));
	return lvl;
}

/* `column = leaf` of the joined table: the equality of a dictionary leaf with a text column, of an integer leaf with an integer column */
static bool
join_clause(AggCtx *c, JoinCtx *jc, Node *q)
{
	OpExpr	   *op;
	Oid			lt,
				rt;
	Node	   *l,
			   *r,
			   *m,
			   *e;
	SiftAggJoin *j = &jc->j;
	ComboCtx   *cb = list_nth(c->combos, jc->combo);
	SiftSrcLeaf *lf;
	int			leaf;
	Oid			mt;
	TypeClass	tc;

	if (!IsA(q, OpExpr) || j->nkey == SIFT_AGG_MAX_JKEYS)
		return false;
	op = (OpExpr *) q;
	if (list_length(op->args) != 2 || builtin_cmp(op->opno, &lt, &rt) != SCMP_EQ)
		return false;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if (join_of_var(c, l) == jc)
	{
		m = l;
		e = r;
	}
	else if (join_of_var(c, r) == jc)
	{
		m = r;
		e = l;
	}
	else
		return false;
	if (bms_is_member(jc->rti, pull_varnos(c->root, e)) || (leaf = match_leaf(c, e)) < 0)
		return false;
	lf = leaf_of(c, leaf);
	mt = ((Var *) m)->vartype;
	tc = type_class(lt);
	if (lf->kind != SCL_SCALAR || lf->lossy)
		return false;
	if (lf->vtype == SCV_CODE)
	{
		if (lt != TEXTOID || rt != TEXTOID || (mt != TEXTOID && mt != VARCHAROID) || (lf->typid != TEXTOID && lf->typid != VARCHAROID) ||
			!OidIsValid(op->inputcollid) || !get_collation_isdeterministic(op->inputcollid))
			return false;
	}
	else if (lf->vtype != SCV_I32 || (tc != TC_INT && tc != TC_DATE) || tc != type_class(rt) || tc != type_class(mt) ||
			 tc != type_class(lf->typid))
		return false;
	j->keyatt[j->nkey] = ((Var *) m)->varattno;
	j->keytype[j->nkey] = mt;
	if ((j->keypos[j->nkey] = combo_leaf(cb, leaf, false, lf->typid, -1, lf->vtype == SCV_CODE ? DEFAULT_COLLATION_OID : InvalidOid)) < 0)
		return false;
	use_leaf(c, leaf);
	j->nkey++;
	return true;
}

typedef struct ComboWalk
{
	AggCtx	   *c;
	JoinCtx    *jc;
	List	   *vars;			/* Vars of the joined table */
	int			nl;
	int			leaf[SIFT_AGG_MAX_CLEAVES];
	Oid			ptype[SIFT_AGG_MAX_CLEAVES];
	int32		ptypmod[SIFT_AGG_MAX_CLEAVES];
	Oid			pcoll[SIFT_AGG_MAX_CLEAVES];
	bool		bad;
} ComboWalk;

static bool
combo_walker(Node *n, ComboWalk *w)
{
	int			id;
	JoinCtx    *jc;

	if (n == NULL || w->bad)
		return false;
	if (IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType))
	{
		if ((jc = join_of_var(w->c, n)) != NULL)
		{
			if (w->jc != NULL && w->jc != jc)
				w->bad = true;
			w->jc = jc;
			w->vars = lappend(w->vars, n);
			return false;
		}
		if ((id = match_leaf(w->c, n)) >= 0)
		{
			int			p = 0;

			while (p < w->nl && w->leaf[p] != id)
				p++;
			if (p == w->nl && p == SIFT_AGG_MAX_CLEAVES)
				w->bad = true;
			else if (p < w->nl && (w->ptype[p] != exprType(n) || w->ptypmod[p] != exprTypmod(n) || w->pcoll[p] != exprCollation(n)))
				w->bad = true;
			else if (p == w->nl)
			{
				w->leaf[p] = id;
				w->ptype[p] = exprType(n);
				w->ptypmod[p] = exprTypmod(n);
				w->pcoll[p] = exprCollation(n);
				w->nl++;
			}
			w->bad |= !lut_leaf_ok(w->c, id);
			return false;
		}
		w->bad |= IsA(n, Var);
	}
	if ((IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC) || IsA(n, SubPlan) || IsA(n, SubLink) || IsA(n, Aggref) ||
		IsA(n, WindowFunc) || IsA(n, GroupingFunc) || IsA(n, PlaceHolderVar) || IsA(n, CurrentOfExpr) || IsA(n, NextValueExpr) ||
		IsA(n, SetToDefault))
		w->bad = true;
	return !w->bad && expression_tree_walker(n, combo_walker, w);
}

/* a slot for expr in the combo of its leaves (and of the table it reads columns of): its combo and slot numbers; false if it does not fit */
static bool
combo_slot(AggCtx *c, Node *expr, bool is_key, int *ci, int *si)
{
	ComboWalk	w = {c};
	ComboCtx   *cb = NULL;
	SlotReq    *rq;

	if (contain_volatile_functions(expr))
		return false;
	combo_walker(expr, &w);
	if (w.bad || (w.nl == 0 && w.jc == NULL))
		return false;
	if (w.jc)
		cb = list_nth(c->combos, w.jc->combo);
	else
		foreach_ptr(ComboCtx, x, c->combos)
		{
			bool		same = x->join < 0 && x->nl == w.nl;

			for (int p = 0; same && p < w.nl; p++)
			{
				int			q = 0;

				while (q < x->nl && x->leaf[q] != w.leaf[p])
					q++;
				same = q < x->nl;
			}
			if (same)
				cb = x;
		}
	if (cb == NULL)
		cb = new_combo(c, -1);
	for (int p = 0; p < w.nl; p++)
		if (combo_leaf(cb, w.leaf[p], true, w.ptype[p], w.ptypmod[p], w.pcoll[p]) < 0)
			return false;
	foreach_ptr(Var, v, w.vars)
		if (join_col(w.jc, v) < 0)
			return false;
	for (int p = 0; p < w.nl; p++)
		use_leaf(c, w.leaf[p]);
	rq = palloc0(sizeof(SlotReq));
	rq->expr = expr;
	rq->is_key = is_key;
	rq->agg = -1;
	cb->slots = lappend(cb->slots, rq);
	*ci = combo_index(c, cb);
	*si = list_length(cb->slots) - 1;
	return true;
}

typedef struct ComboSub
{
	AggCtx	   *c;
	ComboCtx   *cb;
	JoinCtx    *jc;
} ComboSub;

static Node *
combo_sub_mutator(Node *n, ComboSub *s)
{
	int			id;
	Param	   *p;

	if (n == NULL)
		return NULL;
	if (IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType))
	{
		p = makeNode(Param);
		p->paramkind = PARAM_EXEC;
		p->location = -1;
		if (s->jc != NULL && join_of_var(s->c, n) == s->jc)
		{
			Var		   *v = (Var *) n;

			p->paramid = s->cb->nl + join_col(s->jc, v);
			p->paramtype = v->vartype;
			p->paramtypmod = v->vartypmod;
			p->paramcollid = v->varcollid;
			return (Node *) p;
		}
		if ((id = match_leaf(s->c, n)) >= 0)
		{
			int			pos = combo_leaf(s->cb, id, false, 0, 0, 0);

			p->paramid = pos;
			p->paramtype = s->cb->ptype[pos];
			p->paramtypmod = s->cb->ptypmod[pos];
			p->paramcollid = s->cb->pcoll[pos];
			return (Node *) p;
		}
		pfree(p);
	}
	return expression_tree_mutator(n, combo_sub_mutator, s);
}

/* the joins and combos of the query as the spec holds them: the slots' expressions over Params */
static void
finish_combos(AggCtx *c, SiftAggSpec *s)
{
	s->njoin = list_length(c->joins);
	s->join = palloc0(Max(s->njoin, 1) * sizeof(SiftAggJoin));
	foreach_ptr(JoinCtx, jc, c->joins)
		s->join[foreach_current_index(jc)] = jc->j;
	s->ncombo = list_length(c->combos);
	s->combo = palloc0(Max(s->ncombo, 1) * sizeof(SiftAggCombo));
	foreach_ptr(ComboCtx, cb, c->combos)
	{
		SiftAggCombo *o = &s->combo[foreach_current_index(cb)];
		ComboSub	sub = {c, cb, cb->join >= 0 ? list_nth(c->joins, cb->join) : NULL};

		o->nleaves = cb->nl;
		o->join = cb->join;
		for (int p = 0; p < cb->nl; p++)
		{
			o->leaf[p] = cb->leaf[p];
			o->ptype[p] = cb->ptype[p];
			o->ptypmod[p] = cb->ptypmod[p];
			o->pcoll[p] = cb->pcoll[p];
			o->level = Max(o->level, leaf_level(c, cb->leaf[p]));
		}
		o->nslots = list_length(cb->slots);
		o->slot = palloc0(Max(o->nslots, 1) * sizeof(SiftAggSlot));
		foreach_ptr(SlotReq, rq, cb->slots)
		{
			SiftAggSlot *sl = &o->slot[foreach_current_index(rq)];

			sl->is_key = rq->is_key;
			sl->key = rq->key;
			sl->agg = rq->agg;
			sl->expr = combo_sub_mutator(rq->expr, &sub);
		}
	}
}

/* EXPLAIN: the leaves a combo reads */
static char *
combo_text(AggCtx *c, int ci, Node *expr)
{
	ComboCtx   *cb = list_nth(c->combos, ci);
	StringInfoData s;

	initStringInfo(&s);
	for (int p = 0; p < cb->nl; p++)
		appendStringInfo(&s, "%s%s", p ? ", " : "", leaf_name(c, cb->leaf[p]));
	return psprintf("%s: %s", s.data, expr_text(c, expr));
}

/* a boolean expression of several leaves, or of leaves and the columns of the matching row of a joined table */
static SiftAggPred *
compile_combo_pred(AggCtx *c, Node *n)
{
	int			ci,
				si;
	SiftAggPred *p;
	ComboCtx   *cb;

	if (exprType(n) != BOOLOID || !combo_slot(c, n, false, &ci, &si))
		return NULL;
	cb = list_nth(c->combos, ci);
	p = new_pred(c, SAP_COMBO);
	p->leaf = ci;
	p->op = si;
	p->level = cb->join >= 0 ? 2 : combo_level(c, cb);
	p->desc = combo_text(c, ci, n);
	return p;
}

/* ---------- numeric expressions ---------- */

/* digits an integer or numeric(p, s) column may hold: from the type modifier of its reference, else what the storage holds */
static int
leaf_digits(Oid typid, int32 typmod)
{
	if (typid == INT2OID)
		return 5;
	if (typid == INT4OID)
		return 10;
	return typmod >= 0 ? ((typmod - VARHDRSZ) >> 16) & 0xffff : 19;
}

static bool
nx_put(SiftAggNum *o, SiftAggNxOp op, int leaf, int a, int b, sift_i128 c)
{
	if (o->n >= SIFT_AGG_MAX_VX)
		return false;
	o->step[o->n++] = (SiftAggNx) {op, leaf, a, b, c};
	return true;
}

static bool
nx_leaf(AggCtx *c, Node *n, int leaf, SiftAggNum *o, int *scale, int *dig)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);

	if (l->kind != SCL_SCALAR || !((l->vtype == SCV_I64 && (l->typid == NUMERICOID || l->typid == INT8OID)) ||
								   (l->vtype == SCV_I32 && (l->typid == INT2OID || l->typid == INT4OID))))
		return false;
	use_leaf(c, leaf);
	*scale = l->typid == NUMERICOID ? l->scale : 0;
	*dig = leaf_digits(l->typid, exprTypmod(n));
	return nx_put(o, SNX_LEAF, leaf, 0, 0, 0);
}

/* a constant is its text: the digits and the dscale PostgreSQL prints; NaN and the infinities are not numbers of the program */
static bool
nx_const(Const *k, SiftAggNum *o, int *scale, int *dig)
{
	sift_i128	v,
				m;

	if (k->constisnull || k->consttype != NUMERICOID ||
		!sift_parse_decimal(DatumGetCString(DirectFunctionCall1(numeric_out, k->constvalue)), &v, scale))
		return false;
	for (*dig = 1, m = v < 0 ? -v : v; m >= 10; m /= 10)
		(*dig)++;
	return nx_put(o, SNX_CONST, -1, 0, 0, v);
}

/* left op right, the result scale the larger one (numeric_add, numeric_sub); *scale and *dig are the left operand's on entry */
static bool
nx_addsub(SiftAggNum *o, SiftAggNxOp op, int *scale, int *dig, int s2, int d2)
{
	int			s1 = *scale,
				sc = Max(s1, s2);

	*dig = Max(*dig + sc - s1, d2 + sc - s2) + 1;
	*scale = sc;
	return *dig <= SIFT_NUM_MAX_DIGITS && nx_put(o, op, -1, sc - s1, sc - s2, 0);
}

/* round, trunc, floor, ceil to n digits after the point: the result has scale n (0 for a negative n), as numeric_round and its kin give */
static bool
nx_rescale(SiftAggNum *o, SiftAggNxOp op, int n, int *scale, int *dig)
{
	int			a = Max(*scale - n, 0),
				b = n >= *scale ? n - *scale : Max(-n, 0);

	*dig = (a > 0 ? Max(*dig - a, 0) + 1 : *dig) + b;
	*scale = Max(n, 0);
	return *dig <= SIFT_NUM_MAX_DIGITS && nx_put(o, op, -1, a, b, 0);
}

/*
 * The program of a numeric expression: leaves, numeric constants, + - * and unary minus, abs, round / trunc / floor / ceil with a constant
 * number of digits, the casts of integers to numeric and of numeric to an integer (the digits it needs fit the integer: no overflow
 * error to reproduce). Anything else (division, other functions, parameters inside the arithmetic, a result of more than
 * SIFT_NUM_MAX_DIGITS digits) is false: its scale or its errors are not known here.
 */
static bool
nx_walk(AggCtx *c, Node *n, SiftAggNum *o, int *scale, int *dig)
{
	int			leaf,
				s2,
				d2,
				nd = 0,
				maxd = SIFT_NUM_MAX_DIGITS;
	Oid			fid;
	List	   *args;
	SiftAggNxOp op;

	n = strip_relabel(n);
	if (IsA(n, Const))
		return nx_const((Const *) n, o, scale, dig);
	if ((leaf = match_leaf(c, n)) >= 0)
		return nx_leaf(c, n, leaf, o, scale, dig);
	fid = func_of(n);
	args = func_args(n);
	switch (fid)
	{
		case F_NUMERIC_ADD:
		case F_NUMERIC_SUB:
		case F_NUMERIC_MUL:
			if (!nx_walk(c, linitial(args), o, scale, dig) || !nx_walk(c, lsecond(args), o, &s2, &d2))
				return false;
			if (fid != F_NUMERIC_MUL)
				return nx_addsub(o, fid == F_NUMERIC_ADD ? SNX_ADD : SNX_SUB, scale, dig, s2, d2);
			*scale += s2;
			*dig += d2;
			return *dig <= SIFT_NUM_MAX_DIGITS && nx_put(o, SNX_MUL, -1, 0, 0, 0);
		case F_NUMERIC_UMINUS:
		case F_ABS_NUMERIC:
		case F_NUMERIC_ABS:
			return nx_walk(c, linitial(args), o, scale, dig) && nx_put(o, fid == F_NUMERIC_UMINUS ? SNX_NEG : SNX_ABS, -1, 0, 0, 0);
		case F_NUMERIC_INT2:
		case F_NUMERIC_INT4:
		case F_NUMERIC_INT8:
			return nx_walk(c, linitial(args), o, scale, dig);
		case F_INT2_NUMERIC:
		case F_INT4_NUMERIC:
		case F_INT8_NUMERIC:
			maxd = fid == F_INT2_NUMERIC ? 4 : fid == F_INT4_NUMERIC ? 9 : 18;
			return nx_walk(c, linitial(args), o, scale, dig) && nx_rescale(o, SNX_ROUND, 0, scale, dig) && *dig <= maxd;
		case F_ROUND_NUMERIC:
		case F_ROUND_NUMERIC_INT4:
		case F_TRUNC_NUMERIC:
		case F_TRUNC_NUMERIC_INT4:
		case F_FLOOR_NUMERIC:
		case F_CEIL_NUMERIC:
		case F_CEILING_NUMERIC:
			if (list_length(args) == 2)
			{
				Const	   *k = (Const *) strip_relabel(lsecond(args));

				if (!IsA(k, Const) || k->constisnull || abs(nd = DatumGetInt32(k->constvalue)) > SIFT_NUM_MAX_DIGITS)
					return false;
			}
			op = (fid == F_ROUND_NUMERIC || fid == F_ROUND_NUMERIC_INT4) ? SNX_ROUND :
				(fid == F_TRUNC_NUMERIC || fid == F_TRUNC_NUMERIC_INT4) ? SNX_TRUNC : fid == F_FLOOR_NUMERIC ? SNX_FLOOR : SNX_CEIL;
			return nx_walk(c, linitial(args), o, scale, dig) && nx_rescale(o, op, nd, scale, dig);
		default:
			return false;
	}
}

/* the program of e, at most maxdig digits wide */
static bool
compile_nx(AggCtx *c, Node *e, int maxdig, SiftAggNum *o)
{
	int			dig;

	o->n = 0;
	o->step = palloc(SIFT_AGG_MAX_VX * sizeof(SiftAggNx));
	return nx_walk(c, e, o, &o->scale, &dig) && dig <= maxdig;
}

/* the level (0 row, 1 element) of the leaves of a program, -1 when it mixes them */
static int
nx_level(AggCtx *c, const SiftAggNum *o)
{
	int			lvl = -1;

	for (int k = 0; k < o->n; k++)
		if (o->step[k].op == SNX_LEAF)
		{
			int			l = leaf_level(c, o->step[k].leaf);

			if (lvl >= 0 && l != lvl)
				return -1;
			lvl = l;
		}
	return lvl;
}

/* numeric expression op operand; two expressions are compared as their difference against 0 */
static SiftAggPred *
compile_ncmp(AggCtx *c, int cmp, Node *l, Node *r)
{
	SiftAggPred *p = new_pred(c, SAP_NCMP);
	Node	   *lhs = l,
			   *rhs = r,
			   *operand;
	int			dig,
				s2,
				d2;

	if (!pseudo_const(r) && pseudo_const(l))
	{
		lhs = r;
		rhs = l;
		cmp = commute(cmp);
	}
	operand = rhs;
	p->num.step = palloc(SIFT_AGG_MAX_VX * sizeof(SiftAggNx));
	if (!nx_walk(c, lhs, &p->num, &p->num.scale, &dig))
		return NULL;
	if (!pseudo_const(rhs))
	{
		if (!nx_walk(c, rhs, &p->num, &s2, &d2) || !nx_addsub(&p->num, SNX_SUB, &p->num.scale, &dig, s2, d2))
			return NULL;
		operand = (Node *) makeConst(NUMERICOID, -1, InvalidOid, -1, NumericGetDatum(int64_to_numeric(0)), false, false);
	}
	if ((p->level = nx_level(c, &p->num)) < 0)
		return NULL;
	p->op = cmp;
	p->expr = operand;
	p->argtype = NUMERICOID;
	p->desc = psprintf("%s %s %s", expr_text(c, lhs), cmp_text(cmp), expr_text(c, rhs));
	return p;
}

/* leaf op operand, for a leaf the comparison kernels read */
static SiftAggPred *
compile_cmp(AggCtx *c, OpExpr *op)
{
	Oid			lt,
				rt;
	int			cmp = builtin_cmp(op->opno, &lt, &rt),
				leaf;
	Node	   *l,
			   *r;
	SiftAggPred *p;
	SiftSrcLeaf *lf;

	if (cmp < 0 || list_length(op->args) != 2)
		return NULL;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if ((leaf = match_leaf(c, l)) >= 0 && pseudo_const(r))
		;
	else if ((leaf = match_leaf(c, r)) >= 0 && pseudo_const(l))
	{
		Node	   *t = l;

		l = r;
		r = t;
		cmp = commute(cmp);
		lt = rt;
	}
	else
		return lt == NUMERICOID && rt == NUMERICOID ? compile_ncmp(c, cmp, l, r) : NULL;
	lf = leaf_of(c, leaf);
	if (lf->kind != SCL_SCALAR || lf->vtype == SCV_CODE)
		return NULL;
	if (type_class(lf->typid) == TC_NONE || type_class(lf->typid) != type_class(exprType(r)) ||
		type_class(exprType(l)) != type_class(lf->typid))
		return NULL;
	p = make_leaf_pred(c, SAP_CMP, leaf);
	p->op = cmp;
	p->expr = r;
	p->argtype = exprType(r);
	p->desc = psprintf("%s %s %s", leaf_name(c, leaf), cmp_text(cmp), expr_text(c, r));
	p->sel = clause_selectivity(c->root, (Node *) op, c->relid, JOIN_INNER, NULL);
	return p;
}

#define AGG_MAX_NUM_IN		64		/* constants of a numeric IN list: one comparison each */

/* numeric leaf IN (constants) is the OR of its equalities, NOT IN the AND of its inequalities: every operand is scaled like a comparison's */
static SiftAggPred *
compile_num_in(AggCtx *c, ScalarArrayOpExpr *sa, int leaf, Node *arr)
{
	List	   *elems = NIL;
	SiftAggPred *p;
	int			i = 0;

	if (IsA(arr, Const))
	{
		Const	   *k = (Const *) arr;
		Datum	   *el;
		bool	   *nl;
		int			ne;

		if (k->constisnull)
			return new_pred(c, SAP_FALSE);
		deconstruct_array(DatumGetArrayTypeP(k->constvalue), NUMERICOID, -1, false, TYPALIGN_INT, &el, &nl, &ne);
		for (int j = 0; j < ne; j++)
			elems = lappend(elems, makeConst(NUMERICOID, -1, InvalidOid, -1, el[j], nl[j], false));
	}
	else if (IsA(arr, ArrayExpr))
		elems = ((ArrayExpr *) arr)->elements;
	else
		return NULL;
	if (elems == NIL)
		return new_pred(c, sa->useOr ? SAP_FALSE : SAP_TRUE);
	if (list_length(elems) > AGG_MAX_NUM_IN)
		return NULL;
	p = new_pred(c, sa->useOr ? SAP_OR : SAP_AND);
	p->level = leaf_level(c, leaf);
	p->nchild = list_length(elems);
	p->child = palloc(p->nchild * sizeof(SiftAggPred *));
	foreach_ptr(Node, e, elems)
	{
		SiftAggPred *q = make_leaf_pred(c, SAP_CMP, leaf);

		q->op = sa->useOr ? SCMP_EQ : SCMP_NE;
		q->expr = e;
		q->argtype = NUMERICOID;
		q->desc = psprintf("%s %s %s", leaf_name(c, leaf), cmp_text(q->op), expr_text(c, e));
		p->child[i++] = q;
	}
	p->desc = psprintf("%s %s %s", leaf_name(c, leaf), sa->useOr ? "IN" : "NOT IN", expr_text(c, arr));
	return p;
}

/* leaf IN (constants), leaf NOT IN (constants), on integer-valued leaves */
static SiftAggPred *
compile_in(AggCtx *c, ScalarArrayOpExpr *sa)
{
	Oid			lt,
				rt;
	int			cmp = builtin_cmp(sa->opno, &lt, &rt),
				leaf;
	Node	   *l,
			   *r;
	SiftAggPred *p;
	SiftSrcLeaf *lf;

	if (cmp < 0 || list_length(sa->args) != 2 || !((sa->useOr && cmp == SCMP_EQ) || (!sa->useOr && cmp == SCMP_NE)))
		return NULL;
	l = strip_relabel(linitial(sa->args));
	r = strip_relabel(lsecond(sa->args));
	if ((leaf = match_leaf(c, l)) < 0 || !pseudo_const(r))
		return NULL;
	lf = leaf_of(c, leaf);
	if (lf->kind == SCL_SCALAR && lf->typid == NUMERICOID && type_class(lt) == TC_NUMERIC)
		return compile_num_in(c, sa, leaf, r);
	if (lf->kind != SCL_SCALAR || lf->vtype != SCV_I32 || type_class(lf->typid) == TC_NONE ||
		type_class(lf->typid) != type_class(lt) || type_class(lf->typid) != type_class(rt) ||
		type_class(lf->typid) == TC_FLOAT)
		return NULL;
	p = make_leaf_pred(c, SAP_IN, leaf);
	p->op = !sa->useOr;
	p->expr = r;
	p->argtype = rt;
	p->desc = psprintf("%s %s %s", leaf_name(c, leaf), sa->useOr ? "IN" : "NOT IN", expr_text(c, r));
	return p;
}

/* any boolean expression of one dictionary or integer leaf */
static SiftAggPred *
compile_lut(AggCtx *c, Node *n)
{
	int			leaf;
	Oid			ptype,
				pcoll;
	int32		ptypmod;
	SiftAggPred *p;

	if (exprType(n) != BOOLOID)
		return NULL;
	if (!single_leaf(c, n, &leaf, &ptype, &ptypmod, &pcoll) || !lut_leaf_ok(c, leaf))
		return compile_combo_pred(c, n);
	p = make_leaf_pred(c, SAP_LUT, leaf);
	p->expr = substitute_leaf(c, n, ptype, ptypmod, pcoll);
	p->desc = psprintf("%s: %s", leaf_name(c, leaf), expr_text(c, n));
	return p;
}

static SiftAggPred *compile_pred(AggCtx *c, Node *n);
static SiftAggPred *compile_existential(AggCtx *c, Node *n, Node **nullable_target);
static bool target_not_null(AggCtx *c, Node *target);

/* an AND / OR node of nchild children, the children to be set (junction_done after) */
static SiftAggPred *
new_junction(AggCtx *c, SiftAggPredKind kind, int nchild)
{
	SiftAggPred *p = new_pred(c, kind);

	p->nchild = nchild;
	p->child = palloc(nchild * sizeof(SiftAggPred *));
	return p;
}

static void
junction_done(SiftAggPred *p)
{
	StringInfoData s;

	initStringInfo(&s);
	for (int i = 0; i < p->nchild; i++)
	{
		p->level = Max(p->level, p->child[i]->level);
		appendStringInfo(&s, "%s%s", i ? (p->kind == SAP_AND ? " AND " : " OR ") : "", p->child[i]->desc);
	}
	p->desc = psprintf("(%s)", s.data);
}

static SiftAggPred *
compile_junction(AggCtx *c, SiftAggPredKind kind, List *args)
{
	SiftAggPred *p = new_junction(c, kind, list_length(args));
	int			i = 0;

	foreach_ptr(Node, a, args)
	{
		if ((p->child[i] = compile_pred(c, a)) == NULL || p->child[i]->level > 1)
			return NULL;
		i++;
	}
	junction_done(p);
	return p;
}

static SiftAggPred *
compile_bool(AggCtx *c, BoolExpr *b)
{
	SiftAggPred *p;
	Node	   *target;

	if (b->boolop != NOT_EXPR)
		return compile_junction(c, b->boolop == AND_EXPR ? SAP_AND : SAP_OR, b->args);
	/* NOT of an existential test is its negation unless the test can be NULL (a NULL document) */
	if ((p = compile_existential(c, strip_relabel(linitial(b->args)), &target)) == NULL)
		return compile_lut(c, (Node *) b);
	if (target && !target_not_null(c, target))
		return NULL;
	p->op = 1;
	p->desc = psprintf("NOT %s", p->desc);
	return p;
}

static SiftAggPred *
compile_pred(AggCtx *c, Node *n)
{
	SiftAggPred *p;
	Node	   *leaf_target;
	int			leaf;

	n = strip_relabel(n);
	if (IsA(n, Const))
	{
		Const	   *k = (Const *) n;

		p = new_pred(c, !k->constisnull && DatumGetBool(k->constvalue) ? SAP_TRUE : SAP_FALSE);
		return p;
	}
	if (IsA(n, BoolExpr))
		return compile_bool(c, (BoolExpr *) n);
	if (IsA(n, NullTest) && !((NullTest *) n)->argisrow && (leaf = match_leaf(c, (Node *) ((NullTest *) n)->arg)) >= 0)
	{
		p = make_leaf_pred(c, SAP_NULL, leaf);
		p->op = ((NullTest *) n)->nulltesttype == IS_NOT_NULL;
		p->desc = psprintf("%s IS %sNULL", leaf_name(c, leaf), p->op ? "NOT " : "");
		return p;
	}
	if (IsA(n, OpExpr) && (p = compile_cmp(c, (OpExpr *) n)) != NULL)
		return p;
	if (IsA(n, ScalarArrayOpExpr) && (p = compile_in(c, (ScalarArrayOpExpr *) n)) != NULL)
		return p;
	if ((p = compile_existential(c, n, &leaf_target)) != NULL)
		return p;
	return compile_lut(c, n);
}

/* ---------- keys and aggregates ---------- */

static SlotReq *
slot_req(AggCtx *c, int combo, int slot)
{
	return list_nth(((ComboCtx *) list_nth(c->combos, combo))->slots, slot);
}

/* the output table of a combo's key slot is the one of key `key` */
static void
combo_set_key(AggCtx *c, SiftAggKey *k, int key)
{
	if (k->kind == SAK_COMBO)
		slot_req(c, k->leaf, k->slot)->key = key;
}

/* a key that is an expression of several leaves, or reads the matching row of a joined table */
static bool
compile_combo_key(AggCtx *c, Node *expr, SiftAggKey *k)
{
	int			ci,
				si;

	if (!combo_slot(c, expr, true, &ci, &si))
		return false;
	k->kind = SAK_COMBO;
	k->leaf = ci;
	k->slot = si;
	k->desc = combo_text(c, ci, expr);
	return true;
}

static bool
compile_key(AggCtx *c, SortGroupClause *sgc, Node *expr, SiftAggKey *k)
{
	int			leaf;
	Oid			ptype,
				pcoll;
	int32		ptypmod;
	RegProcedure lhs,
				rhs;

	if (!get_op_hash_functions(sgc->eqop, &lhs, &rhs))
		return false;
	expr = normalize_expr(c, expr);
	k->typid = exprType(expr);
	k->typmod = exprTypmod(expr);
	k->collid = exprCollation(expr);
	k->eqop = sgc->eqop;
	/* a float leaf groups by float_key's bits; an expression of one is not a lookup table (too many values) */
	if ((leaf = match_leaf(c, expr)) >= 0 && (lut_leaf_ok(c, leaf) || (leaf_of(c, leaf)->kind == SCL_SCALAR && leaf_of(c, leaf)->vtype == SCV_F64)))
	{
		k->kind = SAK_LEAF;
		k->leaf = leaf;
		k->desc = pstrdup(leaf_name(c, leaf));
	}
	else if (leaf < 0 && single_leaf(c, expr, &leaf, &ptype, &ptypmod, &pcoll) && lut_leaf_ok(c, leaf))
	{
		k->kind = SAK_LUT;
		k->leaf = leaf;
		k->expr = substitute_leaf(c, expr, ptype, ptypmod, pcoll);
		k->desc = psprintf("%s: %s", leaf_name(c, leaf), expr_text(c, expr));
	}
	else if ((k->typid == NUMERICOID || k->typid == INT2OID || k->typid == INT4OID || k->typid == INT8OID) &&
			 compile_nx(c, expr, SIFT_NUM_MAX_DIGITS, &k->num))
	{
		k->kind = SAK_NUM;
		k->leaf = -1;
		k->desc = expr_text(c, expr);
		return true;
	}
	else
		return compile_combo_key(c, expr, k);
	use_leaf(c, leaf);
	return true;
}

/* count(DISTINCT x) of a column that is unique per row: a unique single-column index on a NOT NULL attribute */
static bool
unique_row_column(AggCtx *c, Node *arg)
{
	Var		   *v;

	arg = strip_relabel(arg);
	if (!IsA(arg, Var))
		return false;
	v = (Var *) arg;
	if (v->varno != c->relid || v->varlevelsup != 0 || v->varattno <= 0 || !bms_is_member(v->varattno, c->rel->notnullattnums))
		return false;
	foreach_node(IndexOptInfo, ii, c->rel->indexlist)
		if (ii->unique && ii->ncolumns == 1 && ii->indexkeys[0] == v->varattno && ii->indpred == NIL && !ii->hypothetical)
			return true;
	return false;
}

static bool
leaf_numeric(AggCtx *c, int leaf, SiftSrcVecType vt, Oid typ1, Oid typ2)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);

	return l->kind == SCL_SCALAR && l->vtype == vt && (l->typid == typ1 || (typ2 != InvalidOid && l->typid == typ2));
}

/*
 * The argument of sum/avg(float8) built of float8 + - * and unary minus over leaves (an int2, int4, float4 or float8 leaf widened by
 * the cast the query has) and constants: the postfix program. Evaluated in double in the order of the tree, as the float8 operators
 * are (their overflow errors are not reproduced).
 */
static bool
compile_vx(AggCtx *c, Node *n, SiftAggAgg *out)
{
	int			leaf;

	n = strip_relabel(n);
	if (out->nvx >= SIFT_AGG_MAX_VX)
		return false;
	if (IsA(n, Const))
	{
		Const	   *k = (Const *) n;

		if (k->constisnull || k->consttype != FLOAT8OID)
			return false;
		out->vx[out->nvx++] = (SiftAggVx) {SVX_CONST, -1, DatumGetFloat8(k->constvalue)};
		return true;
	}
	if (IsA(n, FuncExpr) && (((FuncExpr *) n)->funcid == F_FLOAT8_INT4 || ((FuncExpr *) n)->funcid == F_FLOAT8_INT2 ||
							 ((FuncExpr *) n)->funcid == F_FLOAT8_FLOAT4 || ((FuncExpr *) n)->funcid == F_FLOAT8_NUMERIC))
		n = strip_relabel(linitial(((FuncExpr *) n)->args));
	if ((leaf = match_leaf(c, n)) >= 0)
	{
		SiftSrcLeaf *l = leaf_of(c, leaf);

		/* numeric to float8 is a division of two doubles, correctly rounded like numeric_float8, while the integer is below 2^53 */
		if (l->kind != SCL_SCALAR || (l->vtype != SCV_I32 && l->vtype != SCV_F64 && l->vtype != SCV_I64) ||
			(l->vtype == SCV_I32 && l->typid != INT4OID && l->typid != INT2OID) ||
			(l->vtype == SCV_F64 && l->typid != FLOAT8OID && l->typid != FLOAT4OID) ||
			(l->vtype == SCV_I64 && (l->typid != NUMERICOID || leaf_digits(l->typid, exprTypmod(n)) > 15)))
			return false;
		use_leaf(c, leaf);
		out->vx[out->nvx++] = (SiftAggVx) {SVX_LEAF, leaf, l->vtype == SCV_I64 ? (double) sift_pow10(l->scale) : 0};
		return true;
	}
	if (IsA(n, OpExpr))
	{
		OpExpr	   *o = (OpExpr *) n;
		SiftAggVxOp op;

		switch (o->opfuncid)
		{
			case F_FLOAT8PL:
				op = SVX_ADD;
				break;
			case F_FLOAT8MI:
				op = SVX_SUB;
				break;
			case F_FLOAT8MUL:
				op = SVX_MUL;
				break;
			case F_FLOAT8UM:
				op = SVX_NEG;
				break;
			default:
				return false;
		}
		foreach_ptr(Node, arg, o->args)
			if (!compile_vx(c, arg, out))
				return false;
		if (out->nvx >= SIFT_AGG_MAX_VX)
			return false;
		out->vx[out->nvx++] = (SiftAggVx) {op, -1, 0};
		return true;
	}
	return false;
}

/*
 * percentile_cont / percentile_disc (frac) WITHIN GROUP (ORDER BY x) with a constant fraction in [0, 1] (others raise their
 * errors in the normal plan) and x a float8 expression, or for percentile_disc also a float4, int2, int4 or date leaf: the
 * executor keeps the values of a group exactly and picks the ranks as PostgreSQL's final functions do.
 */
static bool
compile_ordered(AggCtx *c, Aggref *a, SiftAggAgg *out)
{
	bool		is_cont = a->aggfnoid == F_PERCENTILE_CONT_FLOAT8_FLOAT8;
	Const	   *frac;
	Node	   *arg;
	TypeCacheEntry *tc;
	SortGroupClause *sgc;
	int			leaf;

	if ((!is_cont && a->aggfnoid != F_PERCENTILE_DISC_FLOAT8_ANYELEMENT) || list_length(a->aggdirectargs) != 1 ||
		list_length(a->args) != 1 || list_length(a->aggorder) != 1 || a->aggdistinct != NIL)
		return false;
	frac = (Const *) strip_relabel(linitial(a->aggdirectargs));
	if (!IsA(frac, Const) || frac->constisnull || frac->consttype != FLOAT8OID)
		return false;
	out->frac = DatumGetFloat8(frac->constvalue);
	if (!(out->frac >= 0 && out->frac <= 1))
		return false;
	arg = strip_relabel((Node *) linitial_node(TargetEntry, a->args)->expr);
	sgc = linitial_node(SortGroupClause, a->aggorder);
	tc = lookup_type_cache(exprType(arg), TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);
	if (sgc->sortop != tc->lt_opr && sgc->sortop != tc->gt_opr)
		return false;
	out->desc_order = sgc->sortop == tc->gt_opr;
	out->kind = is_cont ? SAA_PCT_CONT : SAA_PCT_DISC;
	out->argtype = exprType(arg);
	if ((leaf = match_leaf(c, arg)) >= 0)
	{
		SiftSrcLeaf *l = leaf_of(c, leaf);

		if (l->kind != SCL_SCALAR ||
			!((l->vtype == SCV_F64 && (l->typid == FLOAT8OID || l->typid == FLOAT4OID)) ||
			  (l->vtype == SCV_I32 && (l->typid == INT4OID || l->typid == INT2OID || l->typid == DATEOID))))
			return false;
		out->leaf = leaf;
		use_leaf(c, leaf);
	}
	else
	{
		out->vx = palloc(SIFT_AGG_MAX_VX * sizeof(SiftAggVx));
		if (out->argtype != FLOAT8OID || !compile_vx(c, arg, out))
			return false;
	}
	out->share = -1;
	out->desc = psprintf("%s(%g) WITHIN GROUP (ORDER BY %s%s)", get_func_name(a->aggfnoid), out->frac, expr_text(c, arg),
						 out->desc_order ? " DESC" : "");
	return true;
}

/* the aggregates on the same argument sort it once: share = the first of them (no FILTER on either) */
static bool
same_sorted_input(const SiftAggAgg *a, const SiftAggAgg *b)
{
	if (a->filter || b->filter || a->leaf != b->leaf || a->nvx != b->nvx)
		return false;
	for (int k = 0; k < a->nvx; k++)
		if (a->vx[k].op != b->vx[k].op || a->vx[k].leaf != b->vx[k].leaf || a->vx[k].c != b->vx[k].c)
			return false;
	return true;
}

static void
share_ordered(SiftAggSpec *s)
{
	for (int i = 0; i < s->naggs; i++)
	{
		if (!is_ordered_agg(&s->agg[i]))
			continue;
		s->agg[i].share = i;
		for (int j = 0; j < i; j++)
			if (is_ordered_agg(&s->agg[j]) && s->agg[j].share == j && same_sorted_input(&s->agg[j], &s->agg[i]))
			{
				s->agg[i].share = j;
				break;
			}
	}
}

#if PG_VERSION_NUM >= 180000
typedef CompareType SortStrategy;
#define SORT_LT		COMPARE_LT
#else
typedef int16 SortStrategy;
#define SORT_LT		BTLessStrategyNumber
#endif

/* the kind of min() or max(): its sort operator is the less (greater) than of the default btree order of the argument's type */
static int
minmax_kind(Aggref *a, Oid argtype)
{
	HeapTuple	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(a->aggfnoid));
	TypeCacheEntry *tc = lookup_type_cache(argtype, TYPECACHE_BTREE_OPFAMILY | TYPECACHE_CMP_PROC_FINFO);
	Oid			sortop,
				opfamily,
				opcintype;
	SortStrategy strategy;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for aggregate %u", a->aggfnoid);
	sortop = ((Form_pg_aggregate) GETSTRUCT(tup))->aggsortop;
	ReleaseSysCache(tup);
	if (!OidIsValid(sortop) || !get_ordering_op_properties(sortop, &opfamily, &opcintype, &strategy) ||
		opfamily != tc->btree_opf || !OidIsValid(tc->cmp_proc))
		return -1;
	return strategy == SORT_LT ? SAA_M_MIN : SAA_M_MAX;
}

/*
 * count, count(DISTINCT), min, max, and sum, avg of numeric and float8 of an expression that reads a column of a joined table: the
 * expression is a slot of the join's combo, which gives the id of the value of each joined row (none: NULL, for the rows a LEFT JOIN
 * does not match). ai: the number of the aggregate in the spec, the owner of the table of the distinct values; -1: not allowed here.
 */
static bool
compile_mapped(AggCtx *c, Aggref *a, Node *arg, SiftAggAgg *out, int ai)
{
	Oid			fid = a->aggfnoid,
				typ = exprType(arg);
	TypeCacheEntry *tc = lookup_type_cache(typ, TYPECACHE_EQ_OPR);
	RegProcedure lhs,
				rhs;
	bool		joined = false;
	int			kind,
				ci,
				si;

	foreach_ptr(JoinCtx, jc, c->joins)
		joined |= bms_is_member(jc->rti, pull_varnos(c->root, arg));
	if (ai < 0 || !joined || !OidIsValid(tc->eq_opr) || !get_op_hash_functions(tc->eq_opr, &lhs, &rhs))
		return false;
	if (fid == F_COUNT_ANY)
		kind = a->aggdistinct != NIL ? SAA_M_DISTINCT : SAA_M_COUNT;
	else if ((fid == F_SUM_NUMERIC || fid == F_AVG_NUMERIC) && typ == NUMERICOID)
		kind = fid == F_SUM_NUMERIC ? SAA_M_SUM : SAA_M_AVG;
	else if ((fid == F_SUM_FLOAT8 || fid == F_AVG_FLOAT8) && typ == FLOAT8OID)
		kind = fid == F_SUM_FLOAT8 ? SAA_M_SUM : SAA_M_AVG;
	else if ((kind = minmax_kind(a, typ)) < 0)
		return false;
	if (!combo_slot(c, normalize_expr(c, arg), true, &ci, &si))
		return false;
	slot_req(c, ci, si)->agg = ai;
	out->kind = kind;
	out->nvx = 0;
	out->argtype = typ;
	out->combo = ci;
	out->slot = si;
	out->desc = psprintf("%s(%s%s)", get_func_name(fid), a->aggdistinct ? "DISTINCT " : "", expr_text(c, arg));
	return true;
}

static bool
compile_agg(AggCtx *c, Aggref *a, SiftAggAgg *out, int ai)
{
	int			leaf = -1;
	Node	   *arg = NULL;
	Oid			fid = a->aggfnoid;
	bool		ordered = a->aggkind == AGGKIND_ORDERED_SET;

	if ((a->aggkind != AGGKIND_NORMAL && !ordered) || (a->aggorder != NIL && !ordered) || a->agglevelsup != 0 ||
		a->aggvariadic || contain_volatile_functions((Node *) a))
		return false;
	if (a->aggdistinct != NIL && fid != F_COUNT_ANY && fid != F_SUM_NUMERIC && fid != F_AVG_NUMERIC)
		return false;
	out->leaf = -1;
	out->restype = a->aggtype;
	out->collid = a->inputcollid;
	if (a->aggfilter)
	{
		if ((out->filter = compile_pred(c, normalize_expr(c, (Node *) a->aggfilter))) == NULL || out->filter->level > 1)
			return false;
	}
	if (ordered)
		return compile_ordered(c, a, out);
	if (fid == F_COUNT_)
		out->kind = SAA_COUNT_STAR;
	else
	{
		if (list_length(a->args) != 1)
			return false;
		arg = (Node *) linitial_node(TargetEntry, a->args)->expr;
		if (fid == F_COUNT_ANY && IsA(strip_relabel(arg), Const) && !((Const *) strip_relabel(arg))->constisnull &&
			a->aggdistinct == NIL)
			out->kind = SAA_COUNT_STAR;
		else if (fid == F_COUNT_ANY && a->aggdistinct != NIL && unique_row_column(c, arg))
			out->kind = SAA_COUNT_ROWS;
		else if (exprType(arg) == NUMERICOID && (fid == F_SUM_NUMERIC || fid == F_AVG_NUMERIC || fid == F_MIN_NUMERIC ||
												 fid == F_MAX_NUMERIC || fid == F_COUNT_ANY))
		{
			bool		sum = fid == F_SUM_NUMERIC || fid == F_AVG_NUMERIC;

			/* a sum of n values of D digits has D + log10(n) digits: 26 leave room for 10^12 rows in 128 bits */
			if (!compile_nx(c, arg, sum ? 26 : SIFT_NUM_MAX_DIGITS, &out->num))
				return compile_mapped(c, a, arg, out, ai);
			out->kind = fid == F_SUM_NUMERIC ? SAA_SUM_NUM : fid == F_AVG_NUMERIC ? SAA_AVG_NUM :
				fid == F_MIN_NUMERIC ? SAA_MIN : fid == F_MAX_NUMERIC ? SAA_MAX : a->aggdistinct ? SAA_COUNT_DISTINCT : SAA_COUNT;
			out->argtype = NUMERICOID;
			out->distinct = sum && a->aggdistinct != NIL;
			out->desc = psprintf("%s(%s%s)", get_func_name(fid), a->aggdistinct ? "DISTINCT " : "", expr_text(c, arg));
			return true;
		}
		else if ((fid == F_SUM_FLOAT8 || fid == F_AVG_FLOAT8) && match_leaf(c, arg) < 0)
		{
			out->vx = palloc(SIFT_AGG_MAX_VX * sizeof(SiftAggVx));
			if (!compile_vx(c, arg, out) || (out->nvx < 2 && out->vx[0].op != SVX_LEAF))
				return compile_mapped(c, a, arg, out, ai);
			out->kind = fid == F_SUM_FLOAT8 ? SAA_SUM_F64 : SAA_AVG_F64;
			out->argtype = FLOAT8OID;
			out->desc = psprintf("%s(%s)", get_func_name(fid), expr_text(c, arg));
			return true;
		}
		else
		{
			leaf = match_leaf(c, arg);
			if (leaf < 0 || leaf_of(c, leaf)->kind == SCL_OBJECT)
				return compile_mapped(c, a, arg, out, ai);
			out->leaf = leaf;
			out->argtype = leaf_of(c, leaf)->typid;
			switch (fid)
			{
				case F_COUNT_ANY:
					if (a->aggdistinct != NIL)
					{
						if (!lut_leaf_ok(c, leaf))
							return false;
						out->kind = SAA_COUNT_DISTINCT;
					}
					else
						out->kind = SAA_COUNT;
					break;
				case F_SUM_INT4:
				case F_SUM_INT2:
					if (!leaf_numeric(c, leaf, SCV_I32, fid == F_SUM_INT4 ? INT4OID : INT2OID, InvalidOid) ||
						leaf_of(c, leaf)->kind != SCL_SCALAR)
						return false;
					out->kind = SAA_SUM_INT;
					break;
				case F_SUM_FLOAT8:				/* sum(float4) adds in float4: left to the normal plan */
					if (!leaf_numeric(c, leaf, SCV_F64, FLOAT8OID, InvalidOid))
						return false;
					out->kind = SAA_SUM_F64;
					break;
				case F_AVG_INT4:
				case F_AVG_INT2:
					if (!leaf_numeric(c, leaf, SCV_I32, fid == F_AVG_INT4 ? INT4OID : INT2OID, InvalidOid))
						return false;
					out->kind = SAA_AVG_INT;
					break;
				case F_AVG_FLOAT8:
				case F_AVG_FLOAT4:
					if (!leaf_numeric(c, leaf, SCV_F64, fid == F_AVG_FLOAT8 ? FLOAT8OID : FLOAT4OID, InvalidOid))
						return false;
					out->kind = SAA_AVG_F64;
					break;
				case F_MIN_INT2:
				case F_MIN_INT4:
				case F_MIN_INT8:
				case F_MIN_DATE:
				case F_MIN_TIMESTAMP:
				case F_MIN_TIMESTAMPTZ:
				case F_MIN_FLOAT4:
				case F_MIN_FLOAT8:
				case F_MAX_INT2:
				case F_MAX_INT4:
				case F_MAX_INT8:
				case F_MAX_DATE:
				case F_MAX_TIMESTAMP:
				case F_MAX_TIMESTAMPTZ:
				case F_MAX_FLOAT4:
				case F_MAX_FLOAT8:
					if (leaf_of(c, leaf)->vtype == SCV_CODE || leaf_of(c, leaf)->kind != SCL_SCALAR ||
						leaf_of(c, leaf)->typid != a->aggtype)
						return false;
					out->kind = (fid == F_MIN_INT2 || fid == F_MIN_INT4 || fid == F_MIN_INT8 || fid == F_MIN_DATE ||
								 fid == F_MIN_TIMESTAMP || fid == F_MIN_TIMESTAMPTZ || fid == F_MIN_FLOAT4 ||
								 fid == F_MIN_FLOAT8) ? SAA_MIN : SAA_MAX;
					break;
				default:
					return false;
			}
			use_leaf(c, leaf);
		}
	}
	out->desc = psprintf("%s%s", fid == F_COUNT_ ? "count(*)" : get_func_name(fid),
						 leaf >= 0 ? psprintf("(%s%s)", a->aggdistinct ? "DISTINCT " : "", leaf_name(c, leaf)) : "");
	return true;
}

/* ---------- the query ---------- */

typedef struct LeftJoin
{
	Index		rti;			/* the right side, a table */
	List	   *quals;			/* its ON clause */
} LeftJoin;

typedef struct FromScan
{
	List	   *rtis;			/* range table indexes, in FROM order */
	List	   *quals;
	List	   *lefts;			/* LeftJoin */
	bool		ok;
} FromScan;

static void
scan_from(Node *n, FromScan *f)
{
	if (n == NULL)
		return;
	if (IsA(n, RangeTblRef))
		f->rtis = lappend_int(f->rtis, ((RangeTblRef *) n)->rtindex);
	else if (IsA(n, FromExpr))
	{
		foreach_ptr(Node, x, ((FromExpr *) n)->fromlist)
			scan_from(x, f);
		if (((FromExpr *) n)->quals)
			f->quals = list_concat(f->quals, list_copy((List *) ((FromExpr *) n)->quals));
	}
	else if (IsA(n, JoinExpr) && ((JoinExpr *) n)->jointype == JOIN_LEFT && IsA(((JoinExpr *) n)->rarg, RangeTblRef))
	{
		JoinExpr   *j = (JoinExpr *) n;
		LeftJoin   *lj = palloc(sizeof(LeftJoin));

		scan_from(j->larg, f);
		lj->rti = ((RangeTblRef *) j->rarg)->rtindex;
		lj->quals = j->quals == NULL ? NIL : IsA(j->quals, List) ? list_copy((List *) j->quals) : list_make1(j->quals);
		f->rtis = lappend_int(f->rtis, lj->rti);
		f->lefts = lappend(f->lefts, lj);
	}
	else if (IsA(n, JoinExpr) && ((JoinExpr *) n)->jointype == JOIN_INNER)
	{
		JoinExpr   *j = (JoinExpr *) n;

		scan_from(j->larg, f);
		scan_from(j->rarg, f);
		if (j->quals)
			f->quals = list_concat(f->quals, list_copy(IsA(j->quals, List) ? (List *) j->quals : list_make1(j->quals)));
	}
	else
		f->ok = false;
}

static FuncKind
func_kind_of(RangeTblFunction *f)
{
	FuncExpr   *fe;

	if (!IsA(f->funcexpr, FuncExpr))
		return FK_NONE;
	fe = (FuncExpr *) f->funcexpr;
	switch (fe->funcid)
	{
		case F_JSONB_TO_RECORDSET:
			return FK_RECORDSET;
		case F_JSONB_ARRAY_ELEMENTS:
			return FK_ELEMENTS;
		case F_JSONB_EACH:
			return FK_EACH;
		case F_JSONB_EACH_TEXT:
			return FK_EACH_TEXT;
		default:
			return FK_NONE;
	}
}

/* the kind of group leaf a set-returning function reads */
static SiftSrcLeafKind
func_leaf_kind(FuncKind k)
{
	return k == FK_EACH || k == FK_EACH_TEXT ? SCL_OBJECT : SCL_ARRAY;
}

/* the group leaf of the given kind an expression is: the jsonb array (object) leaf an unnested function reads */
static int
group_leaf_of(AggCtx *c, Node *arg, SiftSrcLeafKind kind)
{
	JsonRef		r = {0};

	if (!json_access(c, arg, &r) || r.as_text || r.att == 0)
		return -1;
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == r.att && l->parent < 0 && paths_equal(l->path, r.path) && l->kind == kind)
			return i;
	}
	return -1;
}

/* ---------- existential predicates: some element of one array passes (SAP_ANY_ELEM) ---------- */

static double elems_of(AggCtx *c, int grp);

/*
 * The element predicate is compiled with the array in scope as the query's group (so its leaves are element leaves of that array,
 * not a second unnest) and with its own set of leaves; whatever it changes but the counters is undone.
 */
static AggCtx
scope_enter(AggCtx *c, int grp)
{
	AggCtx		saved = *c;

	c->group = grp;
	c->used = NULL;
	return saved;
}

static void
scope_leave(AggCtx *c, const AggCtx *saved, int grp)
{
	c->elem_used = bms_join(c->elem_used, c->used);
	c->used = bms_add_member(saved->used, grp);
	c->group = saved->group;
	c->func_rti = saved->func_rti;
	c->func = saved->func;
	c->func_kind = saved->func_kind;
	c->rtable = saved->rtable;
	c->dpctx = saved->dpctx;
}

typedef SiftAggPred *(*ElemBuild) (AggCtx *c, int grp, void *arg);

/* some element of array leaf grp passes the predicate that build compiles over the leaves of its elements */
static SiftAggPred *
any_elem(AggCtx *c, int grp, ElemBuild build, void *arg)
{
	SiftAggPred *p = new_pred(c, SAP_ANY_ELEM),
			   *e;
	int			n0 = c->npred;
	AggCtx		saved = scope_enter(c, grp);

	e = build(c, grp, arg);
	scope_leave(c, &saved, grp);
	if (e == NULL)
		return NULL;
	p->leaf = grp;
	p->nchild = 1;
	p->child = palloc(sizeof(SiftAggPred *));
	p->child[0] = e;
	c->elem_work += (c->npred - n0) * elems_of(c, grp);
	p->desc = psprintf("EXISTS %s[] (%s)", leaf_name(c, grp), e->desc);
	return p;
}

/* a value the element leaf is compared with: a JSON string, number or boolean */
typedef struct Lit
{
	char		kind;			/* 's', 'n', 'b' */
	const char *s;
	int			len;
	Numeric		num;
	bool		b;
} Lit;

static int
child_named(AggCtx *c, int group, const char *name)
{
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->parent == group && !l->lossy && l->kind == SCL_SCALAR && strcmp(l->name, name) == 0)
			return i;
	}
	return -1;
}

/* element leaf (JSON member `name`) op lit with the semantics of a jsonpath / containment comparison of JSON values, or NULL when not provable */
static SiftAggPred *
lit_cmp(AggCtx *c, int grp, const char *name, int op, const Lit *lit)
{
	int			leaf = child_named(c, grp, name);
	SiftAggPred *p;
	Const	   *k;
	Oid			typid;

	if (leaf < 0)
		return NULL;
	typid = leaf_of(c, leaf)->typid;
	if (lit->kind == 's' && typid == TEXTOID && op == SCMP_EQ && lut_leaf_ok(c, leaf))
	{
		/* bytewise equality: the C collation is deterministic whatever the database collation is */
		Param	   *pm = makeNode(Param);
		char	   *str = pnstrdup(lit->s, lit->len);
		OpExpr	   *e;

		pm->paramkind = PARAM_EXEC;
		pm->paramtype = TEXTOID;
		pm->paramtypmod = -1;
		pm->paramcollid = C_COLLATION_OID;
		pm->location = -1;
		k = makeConst(TEXTOID, -1, C_COLLATION_OID, -1, PointerGetDatum(cstring_to_text_with_len(lit->s, lit->len)), false, false);
		e = (OpExpr *) make_opclause(get_opfamily_member(TEXT_BTREE_FAM_OID, TEXTOID, TEXTOID, BTEqualStrategyNumber), BOOLOID,
									 false, (Expr *) pm, (Expr *) k, InvalidOid, C_COLLATION_OID);
		set_opfuncid(e);
		p = make_leaf_pred(c, SAP_LUT, leaf);
		p->expr = (Node *) e;
		p->desc = psprintf("%s = %s", leaf_name(c, leaf), quote_literal_cstr(str));
		return p;
	}
	/* jsonb numbers are numerics: only an integer leaf and an integral literal compare exactly (a float8 leaf holds the rounded value) */
	if (lit->kind == 'n' && (typid == INT2OID || typid == INT4OID || typid == INT8OID))
	{
		bool		err;
		int64		v = numeric_int8_opt_error(lit->num, &err);

		if (err || !DatumGetBool(DirectFunctionCall2(numeric_eq, NumericGetDatum(lit->num),
													 DirectFunctionCall1(int8_numeric, Int64GetDatum(v)))))
			return NULL;
		k = makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(v), false, FLOAT8PASSBYVAL);
	}
	else if (lit->kind == 'b' && typid == BOOLOID)
		k = makeConst(BOOLOID, -1, InvalidOid, 1, BoolGetDatum(lit->b), false, true);
	else
		return NULL;
	p = make_leaf_pred(c, SAP_CMP, leaf);
	p->op = op;
	p->expr = (Node *) k;
	p->argtype = k->consttype;
	p->desc = psprintf("%s %s %s", leaf_name(c, leaf), cmp_text(op), expr_text(c, (Node *) k));
	return p;
}

/* ---- EXISTS (SELECT 1 FROM <unnest of the array> AS t(..) WHERE <element conjunction>) ---- */

typedef struct ExistsArg
{
	Query	   *q;
	RangeTblFunction *f;
	FuncKind	kind;
} ExistsArg;

/* the query of a correlated EXISTS sub-plan, if it is the plain shape: one unnesting function and a WHERE */
static Query *
exists_query(AggCtx *c, SubPlan *sp)
{
	Query	   *q;
	RangeTblEntry *rte;

	if (sp->subLinkType != EXISTS_SUBLINK || sp->testexpr || sp->useHashTable || sp->setParam || sp->parParam == NIL ||
		list_length(sp->parParam) != list_length(sp->args) || sp->plan_id < 1 ||
		sp->plan_id > list_length(c->root->glob->subroots))
		return NULL;
	q = ((PlannerInfo *) list_nth(c->root->glob->subroots, sp->plan_id - 1))->parse;
	if (q->commandType != CMD_SELECT || q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->groupClause || q->havingQual ||
		q->distinctClause || q->limitCount || q->limitOffset || q->setOperations || q->cteList || q->rowMarks ||
		list_length(q->rtable) != 1 || list_length(q->jointree->fromlist) != 1 ||
		!IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return NULL;
	rte = linitial_node(RangeTblEntry, q->rtable);
	if (rte->rtekind != RTE_FUNCTION || rte->funcordinality || list_length(rte->functions) != 1 ||
		contain_volatile_functions(q->jointree->quals))
		return NULL;
	return q;
}

/* the outer expressions the sub-plan's parameters stand for */
static Node *
outer_mutator(Node *n, SubPlan *sp)
{
	if (n && IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
	{
		ListCell   *lp,
				   *la;

		forboth(lp, sp->parParam, la, sp->args)
			if (lfirst_int(lp) == ((Param *) n)->paramid)
				return copyObject(lfirst(la));
	}
	return expression_tree_mutator(n, outer_mutator, sp);
}

/* the conjunction as one predicate */
static SiftAggPred *
compile_conj(AggCtx *c, List *quals)
{
	SiftAggPred *p;

	if (quals == NIL)
	{
		p = new_pred(c, SAP_TRUE);
		p->desc = pstrdup("true");
		return p;
	}
	return list_length(quals) == 1 ? compile_pred(c, linitial(quals)) : compile_junction(c, SAP_AND, quals);
}

static SiftAggPred *
build_exists(AggCtx *c, int grp, void *arg)
{
	ExistsArg  *x = arg;

	c->func_rti = 1;
	c->func = x->f;
	c->func_kind = x->kind;
	c->rtable = x->q->rtable;
	c->dpctx = NIL;
	return compile_conj(c, (List *) x->q->jointree->quals);
}

static SiftAggPred *
compile_exists(AggCtx *c, SubPlan *sp)
{
	ExistsArg	x;
	Query	   *q = exists_query(c, sp);
	int			grp;

	if (q == NULL)
		return NULL;
	x.q = q;
	x.f = linitial_node(RangeTblFunction, linitial_node(RangeTblEntry, q->rtable)->functions);
	x.kind = func_kind_of(x.f);
	if (x.kind == FK_NONE)
		return NULL;
	grp = group_leaf_of(c, outer_mutator(linitial(((FuncExpr *) x.f->funcexpr)->args), sp), func_leaf_kind(x.kind));
	return grp < 0 ? NULL : any_elem(c, grp, build_exists, &x);
}

/* ---- jsonb_path_exists(col, '$[*] ? (<comparisons of @.key with literals, && ||>)') ---- */

static int
path_cmp_op(JsonPathItemType t)
{
	switch (t)
	{
		case jpiEqual:
			return SCMP_EQ;
		case jpiLess:
			return SCMP_LT;
		case jpiLessOrEqual:
			return SCMP_LE;
		case jpiGreater:
			return SCMP_GT;
		case jpiGreaterOrEqual:
			return SCMP_GE;
		default:					/* <>: JSON null is "not equal" to a literal but looks like an absent key in the index */
			return -1;
	}
}

static bool
path_lit(JsonPathItem *v, Lit *lit)
{
	if (jspHasNext(v))
		return false;
	switch (v->type)
	{
		case jpiString:
			lit->kind = 's';
			lit->s = jspGetString(v, &lit->len);
			return true;
		case jpiNumeric:
			lit->kind = 'n';
			lit->num = jspGetNumeric(v);
			return true;
		case jpiBool:
			lit->kind = 'b';
			lit->b = jspGetBool(v);
			return true;
		default:
			return false;
	}
}

/* @.key, as the name of the member */
static char *
path_key(JsonPathItem *v)
{
	JsonPathItem k;
	int32		len;
	char	   *name;

	if (v->type != jpiCurrent || !jspGetNext(v, &k) || k.type != jpiKey || jspHasNext(&k))
		return NULL;
	name = jspGetString(&k, &len);
	return pnstrdup(name, len);
}

static SiftAggPred *
path_pred(AggCtx *c, int grp, JsonPathItem *v)
{
	JsonPathItem l,
				r;
	SiftAggPred *p;
	int			op;
	char	   *key;
	Lit			lit;

	op = path_cmp_op(v->type);
	if (jspHasNext(v) || (op < 0 && v->type != jpiAnd && v->type != jpiOr))
		return NULL;
	jspGetLeftArg(v, &l);
	jspGetRightArg(v, &r);
	if (op < 0)
	{
		p = new_junction(c, v->type == jpiAnd ? SAP_AND : SAP_OR, 2);
		if ((p->child[0] = path_pred(c, grp, &l)) == NULL || (p->child[1] = path_pred(c, grp, &r)) == NULL)
			return NULL;
		junction_done(p);
		return p;
	}
	if ((key = path_key(&l)) != NULL && path_lit(&r, &lit))
		;
	else if ((key = path_key(&r)) != NULL && path_lit(&l, &lit))
		op = commute(op);
	else
		return NULL;
	return lit_cmp(c, grp, key, op, &lit);
}

static SiftAggPred *
build_path(AggCtx *c, int grp, void *arg)
{
	return path_pred(c, grp, arg);
}

static bool
empty_jsonb_object(Node *n)
{
	n = strip_relabel(n);
	return IsA(n, Const) && !((Const *) n)->constisnull && JB_ROOT_IS_OBJECT(DatumGetJsonbP(((Const *) n)->constvalue)) &&
		JB_ROOT_COUNT(DatumGetJsonbP(((Const *) n)->constvalue)) == 0;
}

/* lax mode `$[*] ? (filter)` on the array column; no variables */
static SiftAggPred *
compile_path_exists(AggCtx *c, Node *n)
{
	List	   *args = func_args(n);
	Node	   *path = strip_relabel(lsecond(args));
	JsonPathItem root,
				any,
				filt,
				expr;
	JsonPath   *jp;
	int			grp;

	if ((func_of(n) == F_JSONB_PATH_EXISTS && (list_length(args) != 4 || !empty_jsonb_object(lthird(args)))) || !IsA(path, Const) ||
		((Const *) path)->constisnull || (grp = group_leaf_of(c, linitial(args), SCL_ARRAY)) < 0)
		return NULL;
	jp = DatumGetJsonPathP(((Const *) path)->constvalue);
	jspInit(&root, jp);
	if (!(jp->header & JSONPATH_LAX) || root.type != jpiRoot || !jspGetNext(&root, &any) || any.type != jpiAnyArray ||
		!jspGetNext(&any, &filt) || filt.type != jpiFilter || jspHasNext(&filt))
		return NULL;
	jspGetArg(&filt, &expr);
	return any_elem(c, grp, build_path, &expr);
}

/* ---- col @> '[{"k": v, ...}, ...]': every object of the right side is contained by some element ---- */

static SiftAggPred *
build_contains(AggCtx *c, int grp, void *arg)
{
	JsonbContainer *obj = arg;
	int			n = JsonContainerSize(obj);
	JsonbIterator *it = JsonbIteratorInit(obj);
	JsonbValue	k,
				v;
	SiftAggPred *p = n > 1 ? new_junction(c, SAP_AND, n) : NULL;

	if (n == 0)
		return NULL;
	(void) JsonbIteratorNext(&it, &k, true);	/* the start of the object */
	for (int i = 0; i < n; i++)
	{
		Lit			lit;
		SiftAggPred *e;

		(void) JsonbIteratorNext(&it, &k, true);
		(void) JsonbIteratorNext(&it, &v, true);
		/* a JSON null equals an absent member in the index but only a JSON null contains it */
		if (v.type == jbvString)
			lit = (Lit) {'s', v.val.string.val, v.val.string.len};
		else if (v.type == jbvNumeric)
			lit = (Lit) {'n', NULL, 0, v.val.numeric};
		else if (v.type == jbvBool)
			lit = (Lit) {'b', NULL, 0, NULL, v.val.boolean};
		else
			return NULL;
		if ((e = lit_cmp(c, grp, pnstrdup(k.val.string.val, k.val.string.len), SCMP_EQ, &lit)) == NULL)
			return NULL;
		if (p)
			p->child[i] = e;
		else
			p = e;
	}
	if (n > 1)
		junction_done(p);
	return p;
}

static SiftAggPred *
compile_contains(AggCtx *c, Node *target, Node *rhs)
{
	Jsonb	   *jb;
	JsonbIterator *it;
	JsonbValue	v;
	SiftAggPred *p = NULL;
	int			grp,
				n;

	rhs = strip_relabel(rhs);
	if (!IsA(rhs, Const) || ((Const *) rhs)->constisnull || (grp = group_leaf_of(c, target, SCL_ARRAY)) < 0)
		return NULL;
	jb = DatumGetJsonbP(((Const *) rhs)->constvalue);
	if (!JB_ROOT_IS_ARRAY(jb) || JB_ROOT_IS_SCALAR(jb) || (n = JB_ROOT_COUNT(jb)) == 0)
		return NULL;
	if (n > 1)
		p = new_junction(c, SAP_AND, n);
	it = JsonbIteratorInit(&jb->root);
	(void) JsonbIteratorNext(&it, &v, true);	/* the start of the array */
	for (int i = 0; i < n; i++)
	{
		SiftAggPred *e;

		(void) JsonbIteratorNext(&it, &v, true);
		if (v.type != jbvBinary || !JsonContainerIsObject(v.val.binary.data) ||
			(e = any_elem(c, grp, build_contains, v.val.binary.data)) == NULL)
			return NULL;
		if (p)
			p->child[i] = e;
		else
			p = e;
	}
	if (n > 1)
		junction_done(p);
	return p;
}

/*
 * The existential tests: EXISTS over the elements of an array, jsonb_path_exists / @? and @> on an array of objects. The result is
 * never NULL for EXISTS; the others are NULL for a NULL document, which the executor (like WHERE) treats as false: *target is that
 * document's expression, for the negation to prove it NOT NULL.
 */
static SiftAggPred *
compile_existential(AggCtx *c, Node *n, Node **target)
{
	Oid			fid;

	*target = NULL;
	if (IsA(n, SubPlan))
		return compile_exists(c, (SubPlan *) n);
	fid = func_of(n);
	if (fid == F_JSONB_PATH_EXISTS || fid == F_JSONB_PATH_EXISTS_OPR)
	{
		*target = linitial(func_args(n));
		return compile_path_exists(c, n);
	}
	if (fid == F_JSONB_CONTAINS || fid == F_JSONB_CONTAINED)
	{
		bool		fwd = fid == F_JSONB_CONTAINS;

		*target = fwd ? linitial(func_args(n)) : lsecond(func_args(n));
		return compile_contains(c, *target, fwd ? lsecond(func_args(n)) : linitial(func_args(n)));
	}
	return NULL;
}

/* the document is a column with a NOT NULL constraint */
static bool
target_not_null(AggCtx *c, Node *target)
{
	Var		   *v = (Var *) strip_relabel(target);

	return IsA(v, Var) && v->varno == c->relid && v->varlevelsup == 0 && v->varattno > 0 &&
		bms_is_member(v->varattno, c->rel->notnullattnums);
}

static void
record_deps(PlannerInfo *root, Node *n);

static bool
dep_walker(Node *n, PlannerInfo *root)
{
	if (n == NULL)
		return false;
	if (IsA(n, FuncExpr))
		record_plan_function_dependency(root, ((FuncExpr *) n)->funcid);
	else if (IsA(n, OpExpr))
	{
		set_opfuncid((OpExpr *) n);
		record_plan_function_dependency(root, ((OpExpr *) n)->opfuncid);
	}
	else if (IsA(n, ScalarArrayOpExpr))
	{
		set_sa_opfuncid((ScalarArrayOpExpr *) n);
		record_plan_function_dependency(root, ((ScalarArrayOpExpr *) n)->opfuncid);
	}
	else if (IsA(n, CoerceViaIO) || IsA(n, ArrayCoerceExpr) || IsA(n, Aggref))
		;
	return expression_tree_walker(n, dep_walker, root);
}

static void
record_deps(PlannerInfo *root, Node *n)
{
	dep_walker(n, root);
}

/* the tree of preds as one flat array, ids ascending, to count them */
static void
pred_deps(PlannerInfo *root, SiftAggPred *p)
{
	if (p == NULL)
		return;
	record_deps(root, p->expr);
	for (int i = 0; i < p->nchild; i++)
		pred_deps(root, p->child[i]);
}

/* Aggrefs of the output, each once; false if a column is used outside the keys and aggregates */
typedef struct OutWalk
{
	List	   *keys;			/* group expressions */
	List	   *aggs;
	bool		bad;
} OutWalk;

static bool
out_walker(Node *n, OutWalk *w)
{
	if (n == NULL || w->bad)
		return false;
	foreach_ptr(Node, k, w->keys)
		if (equal(n, k))
			return false;
	if (IsA(n, Aggref) || IsA(n, GroupingFunc))
	{
		if (!list_member(w->aggs, n))
			w->aggs = lappend(w->aggs, n);
		return false;
	}
	if (IsA(n, Var) || IsA(n, PlaceHolderVar) || IsA(n, WindowFunc) || IsA(n, SubPlan) || IsA(n, SubLink))
	{
		w->bad = true;
		return false;
	}
	return expression_tree_walker(n, out_walker, w);
}

/* ---------- cost ---------- */

/*
 * The executor's cost per value in units of cpu_operator_cost (about 3 ns: docs/COL-PLAN.md, "Cost", fitted on whole table runs of
 * one to three keys and aggregates), and the elements per row of an array whose statistics are missing.
 */
#define AGG_SCAN_LEAF		0.7		/* a leaf read (decode, null flags) */
#define AGG_FILTER_VALUE	0.5		/* a predicate on a value */
#define AGG_TUPLE			2.5		/* a tuple that passes the filters: selection, ordinals, group ids */
#define AGG_AGG_ROW			1.2		/* an aggregate update */
#define AGG_KEY_ROW			1.0		/* a group key id */
#define AGG_COMBO_ROW		3.0		/* a tuple's entry in a combo: gathers and a hash probe */
#define AGG_JOIN_ROW		4.0		/* a row of a joined table, read when the scan starts */
#define AGG_ORDERED_ROW		6.0		/* a value kept for an ordered-set aggregate (histogram probe) */
#define AGG_NUM_STEP		0.5		/* an int128 add, subtract, multiply on a value */
#define AGG_NUM_DIV			8.0		/* round, trunc, floor, ceil: an int128 division */
#define AGG_UNIT_COST		200.0
#define AGG_START_COST		400.0
#define AGG_MERGE_GROUP		8.0		/* a group of a partial table of a worker: dumped, sent, merged */
#define AGG_DEFAULT_ELEMS	3.0

static double
elems_of(AggCtx *c, int grp)
{
	return c->stats.elems_per_row[grp] > 0 ? c->stats.elems_per_row[grp] : AGG_DEFAULT_ELEMS;
}

static double
num_cost(const SiftAggNum *o)
{
	double		cost = 0;

	for (int k = 0; k < o->n; k++)
		cost += o->step[k].op >= SNX_ROUND ? AGG_NUM_DIV : o->step[k].op > SNX_CONST ? AGG_NUM_STEP : 0;
	return cost;
}

/* the numeric programs of a predicate tree, per row that reaches them */
static double
pred_num_cost(const SiftAggPred *p)
{
	double		cost = p == NULL ? 0 : num_cost(&p->num);

	for (int i = 0; p && i < p->nchild; i++)
		cost += pred_num_cost(p->child[i]);
	return cost;
}

/* what PostgreSQL divides the cost of a parallel scan by: the workers, and the share of the leader that scans too */
static double
parallel_divisor(int workers)
{
	double		d = workers;

	if (parallel_leader_participation)
		d += Max(1.0 - 0.3 * workers, 0.0);
	return d;
}

/* The correlation of the heap column a leaf is read from with the heap order (0: unknown, or the leaf is a jsonb path or an expression) */
static double
leaf_correlation(AggCtx *c, int leaf)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
	HeapTuple	tp;
	AttStatsSlot sslot;
	double		corr = 0.0;

	if (e == NULL || !IsA(e, Var))
		return 0.0;
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid),
						 Int16GetDatum(((Var *) e)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tp))
		return 0.0;
	if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		corr = sslot.numbers[0];
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	return corr;
}

/*
 * The share of the units a zone test leaves: a comparison keeps the fraction f of the rows (its selectivity, which for a
 * comparison is also the fraction of the leaf's range); the units are consecutive in the heap, so their zones are narrow in the
 * share the column is correlated with the heap order (corr^2) and as wide as the range otherwise: kept = f + (1 - f) * (1 - corr^2).
 * The most selective comparison counts, and the scan reads at least one unit (docs/COL-PLAN.md, "Zone pruning").
 */
static double
zone_kept(AggCtx *c, SiftAggSpec *s)
{
	double		kept = 1.0;

	foreach_ptr(SiftAggPred, p, sift_aggspec_zone_cmps(s, c->lay, c->lay->nleaves))
	{
		double		f = Min(Max(p->sel, 0.0), 1.0),
					corr = leaf_correlation(c, p->leaf);

		kept = Min(kept, f + (1.0 - f) * (1.0 - corr * corr));
	}
	return kept;
}

static void
agg_cost(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, Path *p, double rows_out)
{
	double		rows = Max(c->stats.nrows, Max(c->rel->tuples, 1.0)),
				elems = 1.0,
				sel_rows = Min(Max(c->rel->rows, 1.0), rows),
				units = Max(c->stats.nunits, ceil(rows / 2000.0)),
				vis = 1.0 - Min(Max(c->stats.allvisible_frac, 0.0), 1.0),
				scan_cost,
				agg_rows;
	Cost		startup;

	if (s->group >= 0)
		elems = elems_of(c, s->group);
	scan_cost = 0;
	for (int i = 0; i < s->nleaves; i++)
		scan_cost += rows * (leaf_level(c, s->leaves[i]) ? elems_of(c, leaf_of(c, s->leaves[i])->parent) : 1.0) * AGG_SCAN_LEAF;
	scan_cost += rows * (1 + (s->group >= 0 ? elems : 0)) * AGG_FILTER_VALUE * Max(s->npred, 1);
	scan_cost += rows * c->elem_work * AGG_FILTER_VALUE;
	scan_cost = (scan_cost + units * AGG_UNIT_COST) * Max(zone_kept(c, s), 1.0 / units);
	agg_rows = sel_rows * (s->group >= 0 ? elems : 1.0);
	scan_cost += agg_rows * (Max(s->nsets, 1) * (AGG_TUPLE + s->nkeys * AGG_KEY_ROW + s->naggs * AGG_AGG_ROW) + s->ncombo * AGG_COMBO_ROW);
	foreach_ptr(JoinCtx, jc, c->joins)
		scan_cost += Max(c->root->simple_rel_array[jc->rti]->tuples, 1.0) * AGG_JOIN_ROW;
	for (int i = 0; i < s->naggs; i++)
	{
		if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
			scan_cost += agg_rows * AGG_ORDERED_ROW;
		scan_cost += agg_rows * num_cost(&s->agg[i].num) + rows * pred_num_cost(s->agg[i].filter);
	}
	for (int i = 0; i < s->nkeys; i++)
		scan_cost += agg_rows * num_cost(&s->key[i].num);
	for (int i = 0; i < s->npred; i++)
		scan_cost += rows * pred_num_cost(s->pred[i]);
	scan_cost *= cpu_operator_cost;
	/* rows on pages that are not all-visible are checked in the heap, a page at a time */
	scan_cost += vis * Min(sel_rows, Max(c->rel->pages, 1.0)) * random_page_cost * 0.25;
	startup = AGG_START_COST * cpu_operator_cost;
	if (s->nworkers > 0)
	{
		/* each process aggregates its share into a table of its own (one per lane for float sums); the leader merges them */
		double		tables = sift_aggspec_ordered(s) ? SIFT_AGG_LANES : s->nworkers + 1;

		scan_cost = scan_cost / parallel_divisor(s->nworkers) +
			tables * Min(rows_out, Max(agg_rows / tables, 1.0)) * AGG_MERGE_GROUP * cpu_operator_cost;
		startup += parallel_setup_cost;
	}
	p->startup_cost = startup + scan_cost;
	p->total_cost = p->startup_cost + rows_out * cpu_tuple_cost;
	p->rows = rows_out;
}

/* ---------- the path ---------- */

static const CustomPathMethods agg_path_methods;

static bool
unsupported_clause(Node *n)
{
	return n != NULL && (contain_volatile_functions(n) || has_exec_param_walker(n, n));
}

/* a table in the FROM list besides the scanned one: it joins by equality of its columns to leaves */
static bool
add_join(AggCtx *c, FromScan *fs, Index rti)
{
	RangeTblEntry *e = rt_fetch(rti, c->root->parse->rtable);
	JoinCtx    *jc = palloc0(sizeof(JoinCtx));

	if (e->relkind != RELKIND_RELATION)
		return false;
	jc->rti = rti;
	jc->j.relid = e->relid;
	foreach_ptr(LeftJoin, lj, fs->lefts)
		if (lj->rti == rti)
		{
			jc->left = true;
			jc->clauses = lj->quals;
		}
	jc->j.left = jc->left;
	jc->j.desc = psprintf("%s%s", get_rel_name(e->relid), jc->left ? " (left)" : "");
	jc->combo = combo_index(c, new_combo(c, list_length(c->joins)));
	c->joins = lappend(c->joins, jc);
	return true;
}

/* the join clauses of every joined table (the ON clause of a LEFT JOIN, the equalities that read the table in the quals of the others);
 * *quals is left with the other conjuncts */
static bool
setup_joins(AggCtx *c, List **quals)
{
	List	   *rest = NIL;

	foreach_ptr(JoinCtx, jc, c->joins)
		foreach_ptr(Node, q, *quals)
			if (!jc->left && bms_is_member(jc->rti, pull_varnos(c->root, q)))
				jc->clauses = lappend(jc->clauses, q);
	foreach_ptr(Node, q, *quals)
	{
		Relids		v = pull_varnos(c->root, q);
		bool		used = false;

		foreach_ptr(JoinCtx, jc, c->joins)
			if (bms_is_member(jc->rti, v))
			{
				if (jc->left)
					return false;
				used = true;
			}
		if (!used)
			rest = lappend(rest, q);
	}
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		if (jc->clauses == NIL)
			return false;
		foreach_ptr(Node, q, jc->clauses)
			if (!join_clause(c, jc, q))
				return false;
	}
	*quals = rest;
	return true;
}

/* Opens the source of the rel and its layout; false if none */
static bool
find_source(AggCtx *c, IndexOptInfo **ixout)
{
	RangeTblEntry *rte = planner_rt_fetch(c->relid, c->root);
	Relation	rel;

	*ixout = NULL;
	if (sift_colsrc_is_table(get_rel_relam(rte->relid)))
	{
		rel = table_open(rte->relid, NoLock);
		c->lay = sift_colsrc_layout(rel);
		sift_colsrc_stats(rel, &c->stats);
		table_close(rel, NoLock);
		c->indexoid = InvalidOid;
		return true;
	}
	foreach_node(IndexOptInfo, ii, c->rel->indexlist)
	{
		if (!sift_colsrc_is_index(ii->relam) || ii->hypothetical || ii->indpred != NIL)
			continue;
		rel = index_open(ii->indexoid, AccessShareLock);
		c->lay = sift_colsrc_layout(rel);
		sift_colsrc_stats(rel, &c->stats);
		index_close(rel, AccessShareLock);
		c->indexoid = ii->indexoid;
		*ixout = ii;
		return true;
	}
	return false;
}

static void
build_attexprs(AggCtx *c)
{
	foreach_ptr(Node, e, c->lay->attexprs)
	{
		Node	   *x = e ? copyObject(e) : NULL;

		if (x)
			ChangeVarNodes(x, 1, c->relid, 0);
		c->attexpr = lappend(c->attexpr, x);
	}
}

static int
cmp_int(const void *a, const void *b)
{
	return *(const int *) a - *(const int *) b;
}

static void
finish_leaves(AggCtx *c, SiftAggSpec *s)
{
	int			n = 0,
				m = -1;

	s->leaves = palloc(Max(bms_num_members(c->used), 1) * sizeof(int));
	while ((m = bms_next_member(c->used, m)) >= 0)
		s->leaves[n++] = m;
	s->nleaves = n;
	qsort(s->leaves, n, sizeof(int), cmp_int);
}

/*
 * The FROM list of root's query as AggCtx: the scanned table with a column source, at most one lateral unnest of one of its leaves,
 * the tables that join to it by equality. input_rel (NULL: not the query being planned) is the join the planner made of it.
 */
static bool
agg_base(AggCtx *c, PlannerInfo *root, RelOptInfo *input_rel, FromScan *fs)
{
	Query	   *parse = root->parse;
	IndexOptInfo *ii;
	Bitmapset  *allrtis = NULL;
	int			nin = 0;

	scan_from((Node *) parse->jointree, fs);
	if (!fs->ok)
		return false;
	c->root = root;
	c->group = -1;
	foreach_int(rti, fs->rtis)
	{
		RangeTblEntry *e = rt_fetch(rti, parse->rtable);

		allrtis = bms_add_member(allrtis, rti);
		if (e->rtekind == RTE_RELATION)
		{
			if (e->inh || e->tablesample || e->securityQuals != NIL ||
				(e->relkind != RELKIND_RELATION && e->relkind != RELKIND_MATVIEW))
				return false;
			if (c->relid == 0)
				c->relid = rti;
			else if (!add_join(c, fs, rti))
				return false;
		}
		else if (e->rtekind == RTE_FUNCTION)
		{
			if (c->func_rti != 0 || e->funcordinality || list_length(e->functions) != 1 || !e->lateral)
				return false;
			c->func_rti = rti;
			c->func = linitial_node(RangeTblFunction, e->functions);
			c->func_kind = func_kind_of(c->func);
			if (c->func_kind == FK_NONE)
				return false;
		}
		else
			return false;
	}
	if (c->relid == 0)
		return false;
	if (input_rel != NULL)
	{
		/* the planner may have dropped a join to a table that no expression reads (unique key): the lookup is exact for it too */
		if (!bms_is_member(c->relid, input_rel->relids))
			return false;
		for (int m = -1; (m = bms_next_member(input_rel->relids, m)) >= 0;)
			if (rt_fetch(m, parse->rtable)->rtekind != RTE_JOIN)
			{
				if (!bms_is_member(m, allrtis))
					return false;
				nin++;
			}
		if (nin + list_length(c->joins) < list_length(fs->rtis))
			return false;
	}
	c->rel = root->simple_rel_array[c->relid];
	c->heapoid = planner_rt_fetch(c->relid, root)->relid;
	if (c->rel == NULL || !find_source(c, &ii))
		return false;
	build_attexprs(c);
	if (c->func_rti)
	{
		c->group = group_leaf_of(c, linitial(((FuncExpr *) c->func->funcexpr)->args), func_leaf_kind(c->func_kind));
		if (c->group < 0)
			return false;
		c->used = bms_add_member(c->used, c->group);
	}
	return true;
}

static SiftAggSpec *
agg_spec_new(AggCtx *c)
{
	SiftAggSpec *s = palloc0(sizeof(SiftAggSpec));

	s->indexoid = c->indexoid;
	s->heapoid = c->heapoid;
	s->group = c->group;
	return s;
}

/* the WHERE of the scan: bitmap predicates, and the conjuncts over the joined rows */
static bool
agg_quals(AggCtx *c, SiftAggSpec *s, FromScan *fs)
{
	List	   *quals = fs->quals;
	StringInfoData txt;

	if (!setup_joins(c, &quals))
		return false;
	s->pred = palloc(Max(list_length(quals), 1) * sizeof(SiftAggPred *));
	s->post = palloc(Max(list_length(quals), 1) * sizeof(SiftAggPred *));
	initStringInfo(&txt);
	foreach_ptr(Node, q, quals)
	{
		SiftAggPred *p = compile_pred(c, normalize_expr(c, q));

		if (p == NULL || unsupported_clause(q))
			return false;
		if (p->level > 1)
			s->post[s->npost++] = p;
		else
			s->pred[s->npred++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	s->desc_quals = txt.data;
	return true;
}

static void
spec_deps(PlannerInfo *root, SiftAggSpec *s)
{
	for (int i = 0; i < s->npred; i++)
		pred_deps(root, s->pred[i]);
	for (int i = 0; i < s->nkeys; i++)
		record_deps(root, s->key[i].expr);
	for (int i = 0; i < s->naggs; i++)
		pred_deps(root, s->agg[i].filter);
	for (int i = 0; i < s->ncombo; i++)
		for (int k = 0; k < s->combo[i].nslots; k++)
			record_deps(root, s->combo[i].slot[k].expr);
	for (int a = 0; a < s->narms; a++)
	{
		for (int i = 0; i < s->arm[a].nfilter; i++)
			pred_deps(root, s->arm[a].filter[i]);
		for (int i = 0; i < s->arm[a].ninner; i++)
			pred_deps(root, s->arm[a].inner[i].filter);
	}
}

/* the groups the scan makes: those of the keys, or of each grouping set */
static double
set_groups(PlannerInfo *root, SiftAggSpec *s, List *keys, double rows)
{
	double		n = 0;

	for (int set = 0; set < Max(s->nsets, 1); set++)
	{
		List	   *sub = NIL;

		for (int j = 0; j < list_length(keys); j++)
			if (s->nsets == 0 || (s->setmask[set] >> j & 1))
				sub = lappend(sub, list_nth(keys, j));
		n += sub == NIL ? 1.0 : Max(estimate_num_groups(root, sub, rows, NULL, NULL), 1.0);
	}
	return n;
}

/* A path with the given number of workers: the spec travels with it, and the cheaper of the paths wins */
static void
add_agg_path(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, RelOptInfo *output_rel, List *scan_tlist, int nworkers)
{
	CustomPath *cp = makeNode(CustomPath);

	s->nworkers = nworkers;
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = output_rel->reltarget;
	cp->path.parallel_safe = false;
	cp->methods = &agg_path_methods;
	agg_cost(c, s, input_rel, &cp->path, s->est_groups);
	cp->custom_private = list_make2(sift_aggspec_pack(s), scan_tlist);
	add_path(output_rel, &cp->path);
}

/*
 * Workers for the scan, as PostgreSQL decides them for a parallel seq scan of the table (size, reloption, the GUCs), when the whole
 * query is safe to run in a worker (the workers evaluate the spec's expressions) and the spec's tables can be merged
 * (sift_aggspec_parallel_ok).
 */
static int
parallel_workers(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel)
{
	PlannerGlobal *glob = c->root->glob;

	if (!sift_aggspec_parallel_ok(s) || !glob->parallelModeOK || glob->maxParallelHazard != PROPARALLEL_SAFE ||
		!input_rel->consider_parallel || !c->rel->consider_parallel || max_parallel_workers_per_gather <= 0)
		return 0;
	return compute_parallel_worker(c->rel, c->rel->pages, -1, max_parallel_workers_per_gather);
}

/* the leaves the spec reads, the combos, the cost; adds the paths to output_rel. keys: the group expressions of root's query */
static void
agg_finish(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, List *keys, List *scan_tlist)
{
	int			nworkers;

	if (c->other_group)
		return;
	share_ordered(s);
	if (c->group < 0)
		for (int i = 0; i < c->lay->nleaves; i++)
			if (bms_is_member(i, c->used) && leaf_of(c, i)->parent >= 0)
				return;
	if (c->group >= 0 && !bms_is_member(c->group, c->used))
		return;
	if (list_length(c->combos) > SIFT_AGG_MAX_COMBOS || list_length(c->joins) > SIFT_AGG_MAX_JOINS)
		return;
	s->npreds_total = c->npred;
	s->nvleaf = c->nvleaf;
	memcpy(s->vleaf, c->vleaf, sizeof(c->vleaf));
	c->used = bms_union(c->used, c->elem_used);
	finish_leaves(c, s);
	finish_combos(c, s);
	s->est_groups = set_groups(root, s, keys, input_rel->rows);

	spec_deps(root, s);
	add_agg_path(c, s, input_rel, output_rel, scan_tlist, 0);
	nworkers = parallel_workers(c, s, input_rel);
	if (nworkers > 0)
		add_agg_path(c, s, input_rel, output_rel, scan_tlist, nworkers);
}

/* the index among the group columns of the one with the sortgroupref ref */
static int
key_of_ref(Query *parse, int ref)
{
	foreach_node(SortGroupClause, sgc, parse->groupClause)
		if (sgc->tleSortGroupRef == ref)
			return foreach_current_index(sgc);
	elog(ERROR, "sift: grouping column %d not found", ref);
}

/* GROUPING(columns): the keys of its arguments; the executor makes its bitmask of the set of each group */
static bool
compile_grouping(Query *parse, GroupingFunc *g, SiftAggAgg *out)
{
	if (g->agglevelsup != 0 || parse->groupingSets == NIL)
		return false;
	out->kind = SAA_GROUPING;
	out->leaf = -1;
	out->restype = INT4OID;
	out->nref = list_length(g->refs);
	out->ref = palloc(out->nref * sizeof(int));
	foreach_int(ref, g->refs)
		out->ref[foreach_current_index(ref)] = key_of_ref(parse, ref);
	out->desc = "GROUPING";
	return true;
}

/*
 * The grouping sets as PostgreSQL expanded them (parse->groupingSets: a list of lists of sortgrouprefs, ROLLUP and CUBE included,
 * duplicates as written): a mask over the group columns for each, and the number of the set as one more key, so that the groups of
 * two sets never meet. The scan emits it as a constant column, which nothing reads.
 */
static void
add_sets(Query *parse, SiftAggSpec *s, List **scan_tlist)
{
	SiftAggKey *k = &s->key[s->nkeys];

	s->nsets = list_length(parse->groupingSets);
	s->setmask = palloc(s->nsets * sizeof(uint32));
	foreach_ptr(List, set, parse->groupingSets)
	{
		uint32		mask = (uint32) 1 << s->nkeys;

		foreach_int(ref, set)
			mask |= (uint32) 1 << key_of_ref(parse, ref);
		s->setmask[foreach_current_index(set)] = mask;
	}
	k->kind = SAK_SET;
	k->leaf = -1;
	k->typid = INT4OID;
	k->typmod = -1;
	k->eqop = Int4EqualOperator;
	k->desc = "grouping set";
	*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(0), false, true),
													   list_length(*scan_tlist) + 1, NULL, false));
	s->nkeys++;
}

static void sift_agg_two_level(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *gextra);

void
sift_agg_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel,
					 void *extra)
{
	Query	   *parse = root->parse;
	GroupPathExtraData *gextra = (GroupPathExtraData *) extra;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {NIL, NIL, NIL, true};
	SiftAggSpec *s;
	OutWalk		ow = {NIL, NIL, false};
	List	   *scan_tlist = NIL;

	if (stage != UPPERREL_GROUP_AGG || !sift_enable_agg || !(parse->hasAggs || parse->groupClause || parse->groupingSets) ||
		parse->commandType != CMD_SELECT || parse->hasWindowFuncs || parse->hasTargetSRFs || parse->hasDistinctOn ||
		root->rowMarks != NIL || parse->hasRecursive)
		return;
	if (list_length(parse->rtable) > 0 && parse->jointree->fromlist != NIL && list_length(parse->jointree->fromlist) == 1 &&
		IsA(linitial(parse->jointree->fromlist), RangeTblRef) &&
		rt_fetch(((RangeTblRef *) linitial(parse->jointree->fromlist))->rtindex, parse->rtable)->rtekind == RTE_SUBQUERY)
	{
		sift_agg_two_level(root, input_rel, output_rel, gextra);
		return;
	}
	if (!agg_base(c, root, input_rel, &fs))
		return;
	s = agg_spec_new(c);
	if (!agg_quals(c, s, &fs))
		return;

	/* keys */
	if (list_length(parse->groupClause) + (parse->groupingSets != NIL) > SIFT_AGG_MAX_KEYS)
		return;
	s->key = palloc0((list_length(parse->groupClause) + 1) * sizeof(SiftAggKey));
	foreach_node(SortGroupClause, sgc, parse->groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);

		if (!compile_key(c, sgc, e, &s->key[s->nkeys]))
			return;
		combo_set_key(c, &s->key[s->nkeys], s->nkeys);
		ow.keys = lappend(ow.keys, e);
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) e, list_length(scan_tlist) + 1, NULL, false));
		s->nkeys++;
	}
	if (parse->groupingSets != NIL)
		add_sets(parse, s, &scan_tlist);

	/* aggregates, in the output and the HAVING clause */
	foreach_ptr(Node, e, output_rel->reltarget->exprs)
		out_walker(e, &ow);
	if (gextra && gextra->havingQual)
		out_walker((Node *) gextra->havingQual, &ow);
	if (ow.bad)
		return;
	s->agg = palloc0(Max(list_length(ow.aggs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(Node, a, ow.aggs)
	{
		if (!(IsA(a, GroupingFunc) ? compile_grouping(parse, (GroupingFunc *) a, &s->agg[s->naggs]) :
			  compile_agg(c, (Aggref *) a, &s->agg[s->naggs], s->naggs)))
			return;
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) a, list_length(scan_tlist) + 1, NULL, false));
		s->naggs++;
	}
	agg_finish(c, s, root, input_rel, output_rel, ow.keys, scan_tlist);
}

/* ---------- two level: an outer aggregate over aggregates grouped by row ---------- */

#define AGG_MAX_ARMS	4

typedef struct CteSub
{
	Index		rti;			/* 0: the arm reads the table itself */
	List	   *tlist;			/* the target list of the common table expression */
} CteSub;

static Node *
cte_sub_mutator(Node *n, CteSub *s)
{
	if (n == NULL)
		return NULL;
	if (s->rti != 0 && IsA(n, Var) && ((Var *) n)->varno == s->rti && ((Var *) n)->varlevelsup == 0)
	{
		TargetEntry *te = get_tle_by_resno(s->tlist, ((Var *) n)->varattno);

		return te ? (Node *) copyObject(te->expr) : n;
	}
	return expression_tree_mutator(n, cte_sub_mutator, s);
}

/* what an arm (a subquery of the outer query) is, in the terms of the table it reads */
typedef struct ArmIn
{
	CteSub		sub;
	List	   *cols;			/* the expressions of its output columns */
	List	   *group;			/* its group expressions */
	List	   *quals;			/* its WHERE, when it reads a common table expression */
	Node	   *id;				/* the group expression that is unique per row */
	SiftAggArm *out;
} ArmIn;

/* the root of the common table expression a CTE scan of `root` reads; NULL if it is not one the scan can look through */
static PlannerInfo *
cte_root(PlannerInfo *root, RangeTblEntry *rte)
{
	PlannerInfo *cteroot = root;
	int			ndx = 0;
	CommonTableExpr *cte = NULL;

	for (Index up = rte->ctelevelsup; up > 0 && cteroot != NULL; up--)
		cteroot = cteroot->parent_root;
	if (cteroot == NULL)
		return NULL;
	foreach_node(CommonTableExpr, x, cteroot->parse->cteList)
	{
		if (strcmp(x->ctename, rte->ctename) == 0)
		{
			cte = x;
			break;
		}
		ndx++;
	}
	if (cte == NULL || cte->cterecursive || cte->search_clause || cte->cycle_clause || ndx >= list_length(cteroot->cte_plan_ids))
		return NULL;
	return list_nth(root->glob->subroots, list_nth_int(cteroot->cte_plan_ids, ndx) - 1);
}

/*
 * The arms of the outer query's one FROM item: the subqueries of a UNION ALL (appendrel) or the subquery itself. The quals of the
 * outer query that PostgreSQL pushed into the arms (to their WHERE, or HAVING) are theirs now: an arm whose rel still has a qual
 * (one it did not push: volatile, a one-time filter) is not one.
 */
static List *
outer_arms(PlannerInfo *root, Index srti)
{
	List	   *arms = NIL;

	if (!rt_fetch(srti, root->parse->rtable)->inh)
		return root->simple_rel_array[srti] && root->simple_rel_array[srti]->subroot &&
			root->simple_rel_array[srti]->baserestrictinfo == NIL ? list_make1(root->simple_rel_array[srti]->subroot) : NIL;
	foreach_node(RestrictInfo, rinfo, root->simple_rel_array[srti]->baserestrictinfo)
		if (rinfo->pseudoconstant)
			return NIL;
	foreach_node(AppendRelInfo, ai, root->append_rel_list)
	{
		RelOptInfo *rel;
		int			i = 0;

		if (ai->parent_relid != srti)
			continue;
		rel = root->simple_rel_array[ai->child_relid];
		if (rel == NULL || rel->subroot == NULL || rel->baserestrictinfo != NIL)
			return NIL;
		foreach_node(Var, v, ai->translated_vars)
			if (!IsA(v, Var) || v->varno != ai->child_relid || v->varattno != ++i)
				return NIL;
		arms = lappend(arms, rel->subroot);
	}
	return arms;
}

/* the table an arm reads: through its CTE scan to the common table expression's root, or the arm's own root; NULL if the arm is not a simple aggregate */
static PlannerInfo *
arm_base(PlannerInfo *arm, ArmIn *in)
{
	Query	   *q = arm->parse;
	RangeTblEntry *rte;

	if (!q->hasAggs || q->groupClause == NIL || q->groupingSets || q->hasWindowFuncs || q->hasTargetSRFs ||
		q->distinctClause || q->sortClause || q->limitCount || q->limitOffset || q->groupDistinct || q->hasForUpdate ||
		q->setOperations || q->cteList != NIL || list_length(q->jointree->fromlist) != 1 ||
		!IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return NULL;
	rte = rt_fetch(((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex, q->rtable);
	in->sub.rti = 0;
	if (rte->rtekind != RTE_CTE)
		return arm;
	in->sub.rti = ((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex;
	return cte_root(arm, rte);
}

static bool
null_const(Node *e)
{
	return IsA(e, Const) && ((Const *) e)->constisnull;
}

/* `operand op constant` (either side) on an int8 or float8 operand: the operand, the comparison and the constant in *out */
static bool
cond_split(Node *q, Node **operand, SiftAggCond *out)
{
	Oid			lt,
				rt,
				vtype;
	OpExpr	   *op;
	Node	   *l,
			   *r;
	Const	   *k;
	int			cmp;

	if (!IsA(q, OpExpr) || list_length(((OpExpr *) q)->args) != 2)
		return false;
	op = (OpExpr *) q;
	if ((cmp = builtin_cmp(op->opno, &lt, &rt)) < 0)
		return false;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if (IsA(r, Const))
	{
		*operand = l;
		k = (Const *) r;
	}
	else if (IsA(l, Const))
	{
		*operand = r;
		k = (Const *) l;
		cmp = commute(cmp);
	}
	else
		return false;
	vtype = exprType(*operand);
	out->op = cmp;
	if (k->constisnull)
		return false;
	if (vtype == FLOAT8OID && k->consttype == FLOAT8OID)
	{
		out->isint = false;
		out->cd = DatumGetFloat8(k->constvalue);
	}
	else if (vtype == INT8OID && (k->consttype == INT8OID || k->consttype == INT4OID || k->consttype == INT2OID))
	{
		out->isint = true;
		out->ci = k->consttype == INT8OID ? DatumGetInt64(k->constvalue) : k->consttype == INT4OID ?
			DatumGetInt32(k->constvalue) : DatumGetInt16(k->constvalue);
	}
	else
		return false;
	return true;
}

/* `column op constant` on an output column of the arms: the column in out->col */
static bool
outer_cond(Node *q, Index srti, SiftAggCond *out)
{
	Node	   *x;

	if (!cond_split(q, &x, out) || !IsA(x, Var) || ((Var *) x)->varno != srti || ((Var *) x)->varlevelsup != 0)
		return false;
	out->col = ((Var *) x)->varattno - 1;
	return true;
}

/* the inner aggregate e of the arm, added to its inner aggregates: its index; -1 if the arm cannot give it */
static int
arm_inner_agg(AggCtx *c, ArmIn *in, Node *e, Oid want_type)
{
	SiftAggArm *a = in->out;
	SiftAggAgg *ia = &a->inner[a->ninner];

	if (!IsA(e, Aggref) || exprType(e) != want_type || !compile_agg(c, (Aggref *) e, ia, -1))
		return -1;
	switch (ia->kind)
	{
		case SAA_COUNT_STAR:
		case SAA_COUNT:
		case SAA_SUM_INT:
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			break;
		default:
			return -1;
	}
	return a->ninner++;
}

/* the inner aggregate that is the arm's column `col`, compiled once: its index in the arm; -1 if the arm cannot give it */
static int
arm_inner(AggCtx *c, ArmIn *in, int col, Oid want_type)
{
	SiftAggArm *a = in->out;

	if (a->colsrc[col] < 0)
		a->colsrc[col] = arm_inner_agg(c, in, list_nth(in->cols, col), want_type);
	return a->colsrc[col];
}

/* a conjunct of the arm's HAVING: `inner aggregate op constant` (in terms of the scanned table), with the aggregate's index as cond.col */
static bool
arm_having(AggCtx *c, ArmIn *in, Node *q, SiftAggCond *out)
{
	Node	   *x;

	return cond_split(q, &x, out) && (out->col = arm_inner_agg(c, in, x, exprType(x))) >= 0;
}

/* The outer aggregate: avg/sum of a float8 or int8 column of the arms, count(DISTINCT id), count(*), with a FILTER of comparisons of columns */
static bool
outer_agg(AggCtx *c, SiftAggSpec *s, Aggref *oa, Index srti, ArmIn *arms, int narms, SiftAggAgg *out)
{
	Oid			fid = oa->aggfnoid;
	Node	   *arg = NULL;
	Var		   *v = NULL;
	List	   *conds = oa->aggfilter ? make_ands_implicit(oa->aggfilter) : NIL;
	int			nc = 0;

	if (oa->aggkind != AGGKIND_NORMAL || oa->aggorder != NIL || oa->agglevelsup != 0 || oa->aggvariadic || contain_volatile_functions((Node *) oa))
		return false;
	out->leaf = -1;
	out->col = -1;
	out->restype = oa->aggtype;
	out->cond = palloc0(Max(list_length(conds), 1) * sizeof(SiftAggCond));
	foreach_ptr(Node, q, conds)
	{
		if (!outer_cond(q, srti, &out->cond[nc++]))
			return false;
	}
	out->ncond = nc;
	for (int i = 0; i < nc; i++)
		if (out->cond[i].col < 0 || out->cond[i].col >= list_length(arms[0].cols))
			return false;
	if (fid != F_COUNT_)
	{
		if (list_length(oa->args) != 1)
			return false;
		arg = strip_relabel((Node *) linitial_node(TargetEntry, oa->args)->expr);
		if (!IsA(arg, Var) || ((Var *) arg)->varno != srti || ((Var *) arg)->varlevelsup != 0)
			return false;
		v = (Var *) arg;
		out->col = v->varattno - 1;
		if (out->col < 0 || out->col >= list_length(arms[0].cols))
			return false;
	}
	if (fid == F_COUNT_ && oa->aggdistinct == NIL)
		out->kind = SAA_O_COUNT_STAR;
	else if (fid == F_COUNT_ANY && oa->aggdistinct != NIL)
	{
		out->kind = SAA_O_COUNT_ROWS;
		for (int k = 0; k < narms; k++)
			if (!equal(list_nth(arms[k].cols, out->col), arms[k].id))
				return false;
	}
	else if (oa->aggdistinct == NIL && (fid == F_AVG_FLOAT8 || fid == F_SUM_FLOAT8) && v && v->vartype == FLOAT8OID)
		out->kind = fid == F_AVG_FLOAT8 ? SAA_O_AVG_F64 : SAA_O_SUM_F64;
	else if (oa->aggdistinct == NIL && fid == F_SUM_INT8 && v && v->vartype == INT8OID)
		out->kind = SAA_O_SUM_I64;
	else
		return false;
	for (int k = 0; k < narms; k++)
	{
		if (out->kind == SAA_O_AVG_F64 || out->kind == SAA_O_SUM_F64 || out->kind == SAA_O_SUM_I64)
			if (arm_inner(c, &arms[k], out->col, v->vartype) < 0)
				return false;
		for (int i = 0; i < out->ncond; i++)
			if (arm_inner(c, &arms[k], out->cond[i].col, out->cond[i].isint ? INT8OID : FLOAT8OID) < 0)
				return false;
	}
	out->desc = psprintf("%s", out->kind == SAA_O_COUNT_STAR ? "count(*)" : out->kind == SAA_O_COUNT_ROWS ? "count(DISTINCT row)" :
						 out->kind == SAA_O_AVG_F64 ? "avg(inner)" : out->kind == SAA_O_SUM_F64 ? "sum(inner)" : "sum(inner int8)");
	return true;
}

/*
 * The outer query aggregates the rows of its FROM item, the arms: a UNION ALL of aggregates (or one) of the same scan, each grouped by
 * a unique row id and by outer keys (a NULL constant where an arm has none of them): see SiftAggArm. The arms are read from their
 * planned roots, so that group expressions and constants are in the planner's final form; an arm that reads a common table
 * expression is looked through to it.
 */
static void
sift_agg_two_level(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *gextra)
{
	Query	   *parse = root->parse;
	Index		srti = ((RangeTblRef *) linitial(parse->jointree->fromlist))->rtindex;
	List	   *roots;
	ArmIn		arms[AGG_MAX_ARMS] = {0};
	int			narms = 0;
	PlannerInfo *base = NULL;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {NIL, NIL, NIL, true};
	SiftAggSpec *s;
	OutWalk		ow = {NIL, NIL, false};
	List	   *scan_tlist = NIL;
	int			okey[SIFT_AGG_MAX_KEYS];
	int			ncols = 0;

	if (parse->groupingSets != NIL || list_length(parse->groupClause) > SIFT_AGG_MAX_KEYS - 1)
		return;
	roots = outer_arms(root, srti);
	if (roots == NIL || list_length(roots) > AGG_MAX_ARMS)
		return;
	foreach_ptr(PlannerInfo, ar, roots)
	{
		PlannerInfo *b = arm_base(ar, &arms[narms]);

		if (b == NULL || (base != NULL && b != base) || (b == ar && list_length(roots) != 1))
			return;
		base = b;
		arms[narms].sub.tlist = b->parse->targetList;
		narms++;
	}
	if (!agg_base(c, base, NULL, &fs))
		return;
	s = agg_spec_new(c);
	if (!agg_quals(c, s, &fs))
		return;

	/* the arms in the terms of the scanned table */
	for (int k = 0; k < narms; k++)
	{
		Query	   *aq = ((PlannerInfo *) list_nth(roots, k))->parse;
		ArmIn	   *in = &arms[k];
		int			nid = 0;

		foreach_node(TargetEntry, te, aq->targetList)
			if (!te->resjunk)
				in->cols = lappend(in->cols, normalize_expr(c, cte_sub_mutator((Node *) te->expr, &in->sub)));
		if (k == 0)
			ncols = list_length(in->cols);
		else if (ncols != list_length(in->cols))
			return;
		foreach_node(SortGroupClause, sgc, aq->groupClause)
		{
			Node	   *g = normalize_expr(c, cte_sub_mutator((Node *) get_sortgroupclause_expr(sgc, aq->targetList), &in->sub));

			in->group = lappend(in->group, g);
			if (unique_row_column(c, g))
			{
				in->id = g;
				nid++;
			}
		}
		if (nid != 1)
			return;
		if (in->sub.rti != 0 && aq->jointree->quals != NULL)
			foreach_ptr(Node, q, (List *) aq->jointree->quals)
				in->quals = lappend(in->quals, normalize_expr(c, cte_sub_mutator(q, &in->sub)));
		in->out = palloc0(sizeof(SiftAggArm));
		in->out->colsrc = palloc(Max(ncols, 1) * sizeof(int));
		for (int i = 0; i < ncols; i++)
			in->out->colsrc[i] = -1;
		in->out->inner = palloc0((ncols + list_length((List *) aq->havingQual)) * sizeof(SiftAggAgg) + 1);
		in->out->having = palloc0(Max(list_length((List *) aq->havingQual), 1) * sizeof(SiftAggCond));
		foreach_ptr(Node, q, (List *) aq->havingQual)
			if (!arm_having(c, in, normalize_expr(c, cte_sub_mutator(q, &in->sub)), &in->out->having[in->out->nhaving++]))
				return;
		in->out->filter = palloc(Max(list_length(in->quals), 1) * sizeof(SiftAggPred *));
		foreach_ptr(Node, q, in->quals)
		{
			SiftAggPred *p = compile_combo_pred(c, q);

			if (p == NULL || unsupported_clause(q))
				return;
			in->out->filter[in->out->nfilter++] = p;
		}
	}

	/* the outer keys: columns that every arm groups by or has as NULL */
	s->key = palloc0(Max(list_length(parse->groupClause), 1) * sizeof(SiftAggKey));
	foreach_node(SortGroupClause, sgc, parse->groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);
		Node	   *def = NULL;

		if (!IsA(e, Var) || ((Var *) e)->varno != srti || ((Var *) e)->varlevelsup != 0 || ((Var *) e)->varattno > ncols)
			return;
		okey[s->nkeys] = ((Var *) e)->varattno - 1;
		for (int k = 0; k < narms; k++)
		{
			Node	   *x = list_nth(arms[k].cols, okey[s->nkeys]);

			if (null_const(x))
				continue;
			if (!list_member(arms[k].group, x) || equal(x, arms[k].id) || (def != NULL && !equal(x, def)))
				return;
			def = x;
			arms[k].out->keymask |= 1 << s->nkeys;
		}
		if (def == NULL || !compile_key(c, sgc, def, &s->key[s->nkeys]))
			return;
		combo_set_key(c, &s->key[s->nkeys], s->nkeys);
		ow.keys = lappend(ow.keys, e);
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) e, list_length(scan_tlist) + 1, NULL, false));
		s->nkeys++;
	}
	/* an inner group is the share of one row in an outer group: every group expression is an outer key (but the id) */
	for (int k = 0; k < narms; k++)
		if (list_length(arms[k].group) != 1 + pg_popcount32(arms[k].out->keymask))
			return;

	/* the outer aggregates */
	foreach_ptr(Node, e, output_rel->reltarget->exprs)
		out_walker(e, &ow);
	if (gextra && gextra->havingQual)
		out_walker((Node *) gextra->havingQual, &ow);
	if (ow.bad)
		return;
	s->agg = palloc0(Max(list_length(ow.aggs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(Node, a, ow.aggs)
	{
		if (!IsA(a, Aggref) || !outer_agg(c, s, (Aggref *) a, srti, arms, narms, &s->agg[s->naggs]))
			return;
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) a, list_length(scan_tlist) + 1, NULL, false));
		s->naggs++;
	}
	s->narms = narms;
	s->arm = palloc0(narms * sizeof(SiftAggArm));
	for (int k = 0; k < narms; k++)
	{
		s->arm[k] = *arms[k].out;
		s->arm[k].ncol = ncols;
	}
	agg_finish(c, s, root, input_rel, output_rel, ow.keys, scan_tlist);
}

/* ---------- plan ---------- */

static Plan *
agg_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	List	   *pack = linitial(best->custom_private);
	List	   *scan_tlist = lsecond(best->custom_private);
	SiftAggSpec *spec = sift_aggspec_unpack(pack);
	GroupPathExtraData *gextra = NULL;

	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->custom_scan_tlist = copyObject(scan_tlist);
	cs->custom_private = pack;
	cs->methods = &sift_agg_plan_methods;
	if (root->hasHavingQual && root->parse->havingQual)
		cs->scan.plan.qual = (List *) copyObject(root->parse->havingQual);
	(void) gextra;
	if (OidIsValid(spec->indexoid))
		root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->indexoid);
	for (int i = 0; i < spec->njoin; i++)
		root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->join[i].relid);
	return &cs->scan.plan;
}

static const CustomPathMethods agg_path_methods = {"SiftAgg", agg_plan_path, NULL};

const CustomScanMethods sift_agg_plan_methods = {"SiftAgg", sift_agg_create_state};
