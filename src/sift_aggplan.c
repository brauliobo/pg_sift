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
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_operator.h"
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
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"

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

static const char *
leaf_name(AggCtx *c, int id)
{
	StringInfoData s;
	SiftSrcLeaf *l = leaf_of(c, id);
	char	   *att = get_attname(c->heapoid, l->attno, true);

	initStringInfo(&s);
	if (l->parent >= 0)
	{
		const char *p = leaf_name(c, l->parent);

		appendStringInfo(&s, leaf_of(c, l->parent)->kind == SCL_OBJECT ? "%s.*.%s" : "%s[].%s", p, l->name);
		return s.data;
	}
	appendStringInfoString(&s, att ? att : "?");
	foreach_ptr(String, k, l->path)
		appendStringInfo(&s, ".%s", strVal(k));
	return s.data;
}

/* an expression the executor may evaluate once per scan: no Vars, no volatile function, no executor parameter */
static bool
has_exec_param_walker(Node *node, void *context)
{
	if (node == NULL)
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
			return scalar_leaf(c, r.att, r.path, want);
		if (c->group < 0)
			return -1;
		if (c->func_kind == FK_ELEMENTS && r.fcol == 1 && list_length(r.path) == 1)
			return child_leaf(c, c->group, strVal(linitial(r.path)), want);
		if (c->func_kind == FK_EACH && r.fcol == 2 && r.path == NIL)
			return child_leaf(c, c->group, "value", want);
	}
	return -1;
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
	TC_BOOL
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

		stmt->rtable = c->root->parse->rtable;
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
		return NULL;
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

	if (exprType(n) != BOOLOID || !single_leaf(c, n, &leaf, &ptype, &ptypmod, &pcoll) || !lut_leaf_ok(c, leaf))
		return NULL;
	p = make_leaf_pred(c, SAP_LUT, leaf);
	p->expr = substitute_leaf(c, n, ptype, ptypmod, pcoll);
	p->desc = psprintf("%s: %s", leaf_name(c, leaf), expr_text(c, n));
	return p;
}

static SiftAggPred *compile_pred(AggCtx *c, Node *n);

static SiftAggPred *
compile_bool(AggCtx *c, BoolExpr *b)
{
	SiftAggPred *p;
	int			i = 0;

	if (b->boolop == NOT_EXPR)
		return compile_lut(c, (Node *) b);
	p = new_pred(c, b->boolop == AND_EXPR ? SAP_AND : SAP_OR);
	p->nchild = list_length(b->args);
	p->child = palloc(p->nchild * sizeof(SiftAggPred *));
	foreach_ptr(Node, a, b->args)
	{
		if ((p->child[i] = compile_pred(c, a)) == NULL)
			return NULL;
		p->level = Max(p->level, p->child[i]->level);
		i++;
	}
	p->desc = psprintf("%s of %d", b->boolop == AND_EXPR ? "AND" : "OR", p->nchild);
	return p;
}

static SiftAggPred *
compile_pred(AggCtx *c, Node *n)
{
	SiftAggPred *p;
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
	return compile_lut(c, n);
}

/* ---------- keys and aggregates ---------- */

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
	k->typid = exprType(expr);
	k->typmod = exprTypmod(expr);
	k->collid = exprCollation(expr);
	k->eqop = sgc->eqop;
	if ((leaf = match_leaf(c, expr)) >= 0)
	{
		if (!lut_leaf_ok(c, leaf))
			return false;
		k->kind = SAK_LEAF;
		k->leaf = leaf;
		k->desc = pstrdup(leaf_name(c, leaf));
	}
	else if (single_leaf(c, expr, &leaf, &ptype, &ptypmod, &pcoll) && lut_leaf_ok(c, leaf))
	{
		k->kind = SAK_LUT;
		k->leaf = leaf;
		k->expr = substitute_leaf(c, expr, ptype, ptypmod, pcoll);
		k->desc = psprintf("%s: %s", leaf_name(c, leaf), expr_text(c, expr));
	}
	else
		return false;
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
							 ((FuncExpr *) n)->funcid == F_FLOAT8_FLOAT4))
		n = strip_relabel(linitial(((FuncExpr *) n)->args));
	if ((leaf = match_leaf(c, n)) >= 0)
	{
		SiftSrcLeaf *l = leaf_of(c, leaf);

		if (l->kind != SCL_SCALAR || (l->vtype != SCV_I32 && l->vtype != SCV_F64) ||
			(l->vtype == SCV_I32 && l->typid != INT4OID && l->typid != INT2OID) ||
			(l->vtype == SCV_F64 && l->typid != FLOAT8OID && l->typid != FLOAT4OID))
			return false;
		use_leaf(c, leaf);
		out->vx[out->nvx++] = (SiftAggVx) {SVX_LEAF, leaf, 0};
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

static bool
compile_agg(AggCtx *c, Aggref *a, SiftAggAgg *out)
{
	int			leaf = -1;
	Node	   *arg = NULL;
	Oid			fid = a->aggfnoid;

	if (a->aggkind != AGGKIND_NORMAL || a->aggorder != NIL || a->agglevelsup != 0 || a->aggvariadic ||
		contain_volatile_functions((Node *) a))
		return false;
	if (a->aggdistinct != NIL && fid != F_COUNT_ANY)
		return false;
	out->leaf = -1;
	out->restype = a->aggtype;
	out->collid = a->inputcollid;
	if (a->aggfilter)
	{
		if ((out->filter = compile_pred(c, (Node *) a->aggfilter)) == NULL)
			return false;
	}
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
		else if ((fid == F_SUM_FLOAT8 || fid == F_AVG_FLOAT8) && match_leaf(c, arg) < 0)
		{
			out->vx = palloc(SIFT_AGG_MAX_VX * sizeof(SiftAggVx));
			if (!compile_vx(c, arg, out) || out->nvx < 2)
				return false;
			out->kind = fid == F_SUM_FLOAT8 ? SAA_SUM_F64 : SAA_AVG_F64;
			out->argtype = FLOAT8OID;
			out->desc = psprintf("%s(%s)", get_func_name(fid), expr_text(c, arg));
			return true;
		}
		else
		{
			leaf = match_leaf(c, arg);
			if (leaf < 0 || leaf_of(c, leaf)->kind == SCL_OBJECT)
				return false;
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

typedef struct FromScan
{
	List	   *rtis;			/* range table indexes, in FROM order */
	List	   *quals;
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

/* the group leaf an unnested function reads: its argument is the jsonb array (object) leaf */
static int
group_leaf_of(AggCtx *c, Node *arg)
{
	JsonRef		r = {0};

	if (!json_access(c, arg, &r) || r.as_text || r.att == 0)
		return -1;
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == r.att && l->parent < 0 && paths_equal(l->path, r.path) &&
			((l->kind == SCL_ARRAY && (c->func_kind == FK_RECORDSET || c->func_kind == FK_ELEMENTS)) ||
			 (l->kind == SCL_OBJECT && (c->func_kind == FK_EACH || c->func_kind == FK_EACH_TEXT))))
			return i;
	}
	return -1;
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
	if (IsA(n, Aggref))
	{
		if (!list_member(w->aggs, n))
			w->aggs = lappend(w->aggs, n);
		return false;
	}
	if (IsA(n, Var) || IsA(n, PlaceHolderVar) || IsA(n, GroupingFunc) || IsA(n, WindowFunc) || IsA(n, SubPlan) ||
		IsA(n, SubLink))
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
#define AGG_UNIT_COST		200.0
#define AGG_START_COST		400.0
#define AGG_DEFAULT_ELEMS	3.0

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
		elems = c->stats.elems_per_row[s->group] > 0 ? c->stats.elems_per_row[s->group] : AGG_DEFAULT_ELEMS;
	scan_cost = 0;
	for (int i = 0; i < s->nleaves; i++)
	{
		bool		lvl = leaf_level(c, s->leaves[i]);

		scan_cost += rows * (lvl ? elems : 1.0) * AGG_SCAN_LEAF;
	}
	scan_cost += rows * (1 + (s->group >= 0 ? elems : 0)) * AGG_FILTER_VALUE * Max(s->npred, 1);
	agg_rows = sel_rows * (s->group >= 0 ? elems : 1.0);
	scan_cost += agg_rows * (AGG_TUPLE + s->nkeys * AGG_KEY_ROW + s->naggs * AGG_AGG_ROW);
	scan_cost *= cpu_operator_cost;
	/* rows on pages that are not all-visible are checked in the heap, a page at a time */
	scan_cost += vis * Min(sel_rows, Max(c->rel->pages, 1.0)) * random_page_cost * 0.25;
	startup = (AGG_START_COST + units * AGG_UNIT_COST) * cpu_operator_cost;
	p->startup_cost = startup + scan_cost;
	p->total_cost = p->startup_cost + rows_out * cpu_tuple_cost;
	p->rows = rows_out;
}

/* ---------- the path ---------- */

static const CustomPathMethods agg_path_methods;

static bool
unsupported_clause(Node *n)
{
	return n != NULL && (contain_volatile_functions(n) || has_exec_param_walker(n, NULL));
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

void
sift_agg_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel,
					 void *extra)
{
	Query	   *parse = root->parse;
	GroupPathExtraData *gextra = (GroupPathExtraData *) extra;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {NIL, NIL, true};
	IndexOptInfo *ii;
	SiftAggSpec *s;
	OutWalk		ow = {NIL, NIL, false};
	List	   *quals,
			   *scan_tlist = NIL;
	RangeTblEntry *rte;
	CustomPath *cp;
	int			nrel = 0;

	if (stage != UPPERREL_GROUP_AGG || !sift_enable_agg || !(parse->hasAggs || parse->groupClause) ||
		parse->commandType != CMD_SELECT || parse->groupingSets != NIL || parse->hasWindowFuncs || parse->hasTargetSRFs ||
		parse->hasDistinctOn || root->rowMarks != NIL || parse->hasRecursive || parse->groupDistinct)
		return;
	scan_from((Node *) parse->jointree, &fs);
	if (!fs.ok)
		return;
	c->root = root;
	c->group = -1;
	foreach_int(rti, fs.rtis)
	{
		RangeTblEntry *e = rt_fetch(rti, parse->rtable);

		if (e->rtekind == RTE_RELATION)
		{
			if (c->relid != 0 || e->inh || e->tablesample || e->securityQuals != NIL ||
				(e->relkind != RELKIND_RELATION && e->relkind != RELKIND_MATVIEW))
				return;
			c->relid = rti;
			nrel++;
		}
		else if (e->rtekind == RTE_FUNCTION)
		{
			if (c->func_rti != 0 || e->funcordinality || list_length(e->functions) != 1 || !e->lateral)
				return;
			c->func_rti = rti;
			c->func = linitial_node(RangeTblFunction, e->functions);
			c->func_kind = func_kind_of(c->func);
			if (c->func_kind == FK_NONE)
				return;
		}
		else
			return;
	}
	if (nrel != 1 || bms_num_members(input_rel->relids) != list_length(fs.rtis))
		return;
	c->rel = root->simple_rel_array[c->relid];
	rte = planner_rt_fetch(c->relid, root);
	c->heapoid = rte->relid;
	if (c->rel == NULL || !find_source(c, &ii))
		return;
	build_attexprs(c);
	if (c->func_rti)
	{
		c->group = group_leaf_of(c, linitial(((FuncExpr *) c->func->funcexpr)->args));
		if (c->group < 0)
			return;
		c->used = bms_add_member(c->used, c->group);
	}

	s = palloc0(sizeof(SiftAggSpec));
	s->indexoid = c->indexoid;
	s->heapoid = c->heapoid;
	s->group = c->group;

	/* quals */
	quals = fs.quals;
	s->pred = palloc(Max(list_length(quals), 1) * sizeof(SiftAggPred *));
	{
		StringInfoData txt;

		initStringInfo(&txt);
		foreach_ptr(Node, q, quals)
		{
			SiftAggPred *p = compile_pred(c, q);

			if (p == NULL || unsupported_clause(q))
				return;
			s->pred[s->npred++] = p;
			appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
		}
		s->desc_quals = txt.data;
	}

	/* keys */
	if (list_length(parse->groupClause) > SIFT_AGG_MAX_KEYS)
		return;
	s->key = palloc0(Max(list_length(parse->groupClause), 1) * sizeof(SiftAggKey));
	foreach_node(SortGroupClause, sgc, parse->groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);

		if (!compile_key(c, sgc, e, &s->key[s->nkeys]))
			return;
		ow.keys = lappend(ow.keys, e);
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) e, list_length(scan_tlist) + 1, NULL, false));
		s->nkeys++;
	}

	/* aggregates, in the output and the HAVING clause */
	foreach_ptr(Node, e, output_rel->reltarget->exprs)
		out_walker(e, &ow);
	if (gextra && gextra->havingQual)
		out_walker((Node *) gextra->havingQual, &ow);
	if (ow.bad)
		return;
	s->agg = palloc0(Max(list_length(ow.aggs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(Aggref, a, ow.aggs)
	{
		if (!compile_agg(c, a, &s->agg[s->naggs]))
			return;
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) a, list_length(scan_tlist) + 1, NULL, false));
		s->naggs++;
	}
	if (c->other_group)
		return;
	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].filter)
			s->npred += 0;
	if (c->group < 0)
		for (int i = 0; i < c->lay->nleaves; i++)
			if (bms_is_member(i, c->used) && leaf_of(c, i)->parent >= 0)
				return;
	if (c->group >= 0 && !bms_is_member(c->group, c->used))
		return;
	s->npreds_total = c->npred;
	finish_leaves(c, s);
	s->est_groups = s->nkeys == 0 ? 1.0 : Max(estimate_num_groups(root, ow.keys, input_rel->rows, NULL, NULL), 1.0);

	/* cost and path */
	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = output_rel->reltarget;
	cp->path.parallel_safe = false;
	cp->methods = &agg_path_methods;
	agg_cost(c, s, input_rel, &cp->path, s->est_groups);
	for (int i = 0; i < s->npred; i++)
		pred_deps(root, s->pred[i]);
	for (int i = 0; i < s->nkeys; i++)
		record_deps(root, s->key[i].expr);
	for (int i = 0; i < s->naggs; i++)
		pred_deps(root, s->agg[i].filter);
	cp->custom_private = list_make2(sift_aggspec_pack(s), scan_tlist);
	add_path(output_rel, &cp->path);
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
	return &cs->scan.plan;
}

static const CustomPathMethods agg_path_methods = {"SiftAgg", agg_plan_path, NULL};

const CustomScanMethods sift_agg_plan_methods = {"SiftAgg", sift_agg_create_state};
