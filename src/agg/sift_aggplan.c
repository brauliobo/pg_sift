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

#include "sift_agg.h"
#include "sift_aggpart.h"
#include "sift_topn.h"
#include "sift_aggspec.h"
#include "sift_colsrc.h"
#include "sift_search.h"

typedef enum FuncKind
{
	FK_NONE,
	FK_RECORDSET,				/* jsonb_to_recordset(array) AS t(f type, ...) */
	FK_ELEMENTS,				/* jsonb_array_elements(array) */
	FK_ELEMENTS_TEXT,			/* jsonb_array_elements_text(array of strings): one text column, the element */
	FK_EACH,					/* jsonb_each(object) */
	FK_EACH_TEXT				/* jsonb_each_text(object) */
} FuncKind;

/* a table joined by equality to leaves (SiftAggJoin) and the combos that evaluate expressions of leaves and its columns (SiftAggCombo) */
typedef struct JoinCtx
{
	Index		rti;
	SiftJoinKind kind;
	List	   *clauses;		/* the ON clause (all of the WHERE quals that read the table of an inner join) */
	SiftAggJoin j;
	Node	   *kexpr[SIFT_AGG_MAX_JKEYS];	/* the key expressions, in terms of the query's Vars */
	int			combo;
	Path	   *cpath;			/* a right side that is a plan (sub-select, VALUES, function): the path of its rows, NULL for a table */
	List	   *args;			/* parent expressions passed to a correlated native child */
	PlannerInfo *subroot;		/* the query level its native plan was made in */
	List	   *cvars;			/* its columns the spec reads (Var, one per column): what the path outputs */
} JoinCtx;

typedef struct SlotReq
{
	Node	   *expr;
	bool		is_key;
	int			key;
	int			agg;
	bool		direct;			/* the value itself (SiftAggSlot.direct) */
	bool		late;			/* a boolean slot whose expression may raise an error (SiftAggPred.late) */
} SlotReq;

typedef struct ComboCtx
{
	int			join;			/* index in AggCtx.joins, -1 none */
	int			nl;
	int			leaf[SIFT_AGG_MAX_CLEAVES];
	int			refcombo[SIFT_AGG_MAX_CLEAVES];
	int			refcol[SIFT_AGG_MAX_CLEAVES];
	bool		typed[SIFT_AGG_MAX_CLEAVES];
	Oid			ptype[SIFT_AGG_MAX_CLEAVES];
	int32		ptypmod[SIFT_AGG_MAX_CLEAVES];
	Oid			pcoll[SIFT_AGG_MAX_CLEAVES];
	List	   *slots;			/* SlotReq */
} ComboCtx;

/* a parameter of the EXISTS sub-plan being compiled and the leaf of the outer row it stands for */
typedef struct OuterParam
{
	int			id;
	int			leaf;
} OuterParam;

typedef struct AggCtx
{
	PlannerInfo *root;
	RelOptInfo *rel;
	Index		relid;
	Oid			heapoid;
	Oid			indexoid;
	SiftSrcLayout *lay;
	int			nparts;			/* a partitioned table: its live partitions, which the scan reads one after the other (indexoid is the first's) */
	Oid		   *partheap;
	Oid		   *partindex;
	double		part_pages;		/* ... and the pages of their heaps */
	List	   *attexpr;		/* per attribute of the source: its expression with varno = relid (NULL: none) */
	List	   *attalias;		/* per attribute: another expression that denotes it (NULL: none or no list) */
	Index		func_rti;
	FuncKind	func_kind;
	RangeTblFunction *func;
	int			group;			/* leaf of the unnested array/object, -1 */
	Bitmapset  *used;			/* leaves the spec reads */
	Bitmapset  *read;			/* leaves whose values it reads, not only their JSON types */
	List	   *dpctx;			/* deparse context of expr_text */
	bool		other_group;	/* a leaf of an array the query does not unnest is read */
	int			rec_pending;	/* 1 + the leaf func_var_leaf just resolved from a column of jsonb_to_recordset (0: none), which the use_leaf that follows takes */
	Bitmapset  *rec_ok;			/* leaves read as a column of jsonb_to_recordset ... */
	Bitmapset  *rec_bad;		/* ... and the leaves read in any other way */
	int			npred;			/* ids handed out to predicate nodes */
	List	   *joins;			/* JoinCtx */
	Bitmapset  *nulling;		/* the outer joins whose nullable side is the scanned table: its Vars carry them in varnullingrels */
	List	   *combos;			/* ComboCtx */
	int			nvleaf;			/* SiftAggSpec.vleaf: leaves c->lay has after the source's */
	SiftAggVLeaf vleaf[SIFT_AGG_MAX_VLEAVES];
	List	   *rtable;			/* the range table expr_text resolves Vars in; NIL: the query's */
	Bitmapset  *elem_used;		/* leaves read inside an existential predicate (SAP_ANY_ELEM) */
	List	   *oparams;		/* OuterParam of the EXISTS sub-plan whose test is being compiled */
	double		elem_work;		/* element predicate evaluations per row of those, for the cost */
	SiftSrcStats stats;
	SiftSrcPreagg pa;			/* what the units of the index store besides their rows */
	SiftSearchPlan *search;		/* the BM25 conjuncts of the WHERE taken out: the scan reads the rows they match (sift_search.h) */
	double		topn_frac;		/* top-N: the fraction of the table the scan reads before the n-th best row cannot be beaten */
	int			nwide;			/* the conjuncts on wide leaves (SAP_WIDE) of the spec */
	int			nsubplan;		/* the hashed sub-plans its lookup tables evaluate: the leader runs them (a worker has no plan state to start them in) */
	Cost		subplan_cost;	/* their start-up: the sub-selects run once, as in the normal plan */
	bool		joinless;		/* the caller takes no join: the sub-selects are not planned */
	int			nsiblings;		/* the plans and the unnest joined to the scanned table: the order PostgreSQL runs them in is its own */
	List	   *planned;		/* the correlated children planned for the query (JoinCtx, by rti): the other scanned table of a flipped join reuses them */
} AggCtx;

/* Planner-only paths of correlated children are planned in their own query level. */
typedef struct SiftAggPath
{
	CustomPath path;
	List       *correlated;		/* the native path of each correlated child, NULL for the other joins */
	List       *subroots;		/* and the query level it belongs to */
} SiftAggPath;

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
	if (l->name[0] == '#')
		appendStringInfoString(&s, strcmp(l->name, "#typeof") == 0 ? "jsonb_typeof(" : "jsonb_array_length(");
	appendStringInfoString(&s, att);
	foreach_ptr(String, k, l->path)
		appendStringInfo(&s, ".%s", strVal(k));
	if (l->name[0] == '#')
		appendStringInfoChar(&s, ')');
	return s.data;
}

static bool array_initplan_ok(AggCtx *c, Param *prm);

/* Local expression slots share parameter numbers with PostgreSQL, but not their identity. */
static bool
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
static bool
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
	v = vleaf_add(c, att, path, leaf_of(c, val)->vtype, typid);
	c->vleaf[c->nvleaf].obj = obj;
	c->vleaf[c->nvleaf].count = false;
	c->vleaf[c->nvleaf].types = false;
	c->vleaf[c->nvleaf++].key = pstrdup(strVal(llast(path)));
	return v->id;
}

static int	typeof_leaf(AggCtx *c, Node *e);
static bool source_can_types(AggCtx *c);

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
static int
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
			r.fcol == 2 && c->group >= 0)
			return child_leaf(c, c->group, "value", cast_to);
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

static Node *
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

/* ---------- expressions of one leaf ---------- */

typedef struct LeafWalk
{
	AggCtx	   *c;
	int			leaf;			/* -1: none yet */
	Oid			ptype;			/* type of the references */
	int32		ptypmod;
	Oid			pcoll;
	bool		bad;
	List	   *subparams;		/* the output Params of the sub-plans met: the only executor parameters an expression may hold */
} LeafWalk;

static bool raising_walker(Node *n, void *context);

typedef struct OuterRef
{
	PlannerInfo *root;			/* the query the examined RTE belongs to */
	int			depth;			/* the query levels below that RTE the walk is in */
	int			skip;			/* the levels above the RTE's query whose references do not count: 0 counts the query's own */
} OuterRef;

/*
 * Does the tree read a column or an aggregate of a query above it, or a common table expression of an enclosing subquery (a plan per
 * row, not a list)? A statement-level common table expression is read once wherever it is referenced.
 */
static bool
outer_ref_walker(Node *n, OuterRef *o)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
		return ((Var *) n)->varlevelsup > o->depth + o->skip;
	if (IsA(n, Aggref) && ((Aggref *) n)->agglevelsup > o->depth + o->skip)
		return true;
	if (IsA(n, RangeTblEntry))
	{
		RangeTblEntry *e = (RangeTblEntry *) n;
		PlannerInfo *owner = o->root;

		if (e->rtekind != RTE_CTE || e->ctelevelsup <= o->depth + o->skip)
			return false;
		for (int up = e->ctelevelsup - o->depth - 1; up > 0 && owner != NULL; up--)
			owner = owner->parent_root;
		return owner == NULL || owner->parent_root != NULL;
	}
	if (IsA(n, Query))
	{
		bool		r;

		o->depth++;
		r = query_tree_walker((Query *) n, outer_ref_walker, o, QTW_EXAMINE_RTES_BEFORE);
		o->depth--;
		return r;
	}
	return expression_tree_walker(n, outer_ref_walker, o);
}

/* does the sub-select, a body of its own, read a column, aggregate or CTE of the queries around it? */
static bool
reads_outer_query(AggCtx *c, Query *q)
{
	OuterRef	oref = {c->root, 0, 0};

	return query_tree_walker(q, outer_ref_walker, &oref, QTW_EXAMINE_RTES_BEFORE);
}

/*
 * An uncorrelated ANY sub-plan that PostgreSQL hashes (x IN (SELECT ..), x NOT IN (SELECT ..) and their NULL rules): its answer is a function of
 * the tested value alone and holds for the statement, so the lookup table of the leaf may ask the sub-plan once per distinct value. The sub-select
 * is run when the scan starts, also when no row would reach it in the normal plan, so it must not raise, be volatile, lock rows, hold a sub-plan of its own
 * or read a column of an outer query (a CTE scan, an initplan of the outer level, a parameter of its plan): the groups are replayed for a rescan. The
 * extParam of its plan says that only after PostgreSQL has finished the whole plan, so the sub-select itself is walked, the copies of it that were planned too.
 */
static bool
subplan_runs_at_start(AggCtx *c, SubPlan *sp)
{
	PlannerGlobal *glob = c->root->glob;
	Query	   *q;

	if (sp->plan_id < 1 || sp->plan_id > list_length(glob->subroots))
		return false;
	q = ((PlannerInfo *) list_nth(glob->subroots, sp->plan_id - 1))->parse;
	return q->rowMarks == NIL && !contain_volatile_functions((Node *) q) && !raising_walker((Node *) q, c) && !reads_outer_query(c, q);
}

static bool
lut_subplan_ok(AggCtx *c, SubPlan *sp)
{
	return sp->subLinkType == ANY_SUBLINK && sp->useHashTable && sp->testexpr != NULL && sp->parParam == NIL && sp->args == NIL &&
		sp->setParam == NIL && subplan_runs_at_start(c, sp);
}

/*
 * The output of an uncorrelated ARRAY(SELECT ..) of the query (x = ANY (ARRAY(SELECT ..)) has it as a parameter, set by an InitPlan the first time
 * the test needs it): the lookup table asks it once, under the same conditions as a hashed sub-plan. Another kind of initplan (a scalar
 * sub-select raises for a second row) or one of an outer query keeps the normal plan.
 */
static bool
array_initplan_ok(AggCtx *c, Param *prm)
{
	if (null_input_param((Node *) prm))
		return false;
	foreach_ptr(SubPlan, sp, c->root->init_plans)
		if (list_member_int(sp->setParam, prm->paramid))
			return sp->subLinkType == ARRAY_SUBLINK && sp->parParam == NIL && sp->args == NIL && subplan_runs_at_start(c, sp);
	return false;
}

static bool
leaf_walker(Node *n, LeafWalk *w)
{
	int			id;

	if (n == NULL || w->bad)
		return false;
	if (null_input_param(n))
	{
		w->bad = true;
		return false;
	}
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
	if (IsA(n, SubPlan) && lut_subplan_ok(w->c, (SubPlan *) n))
		w->subparams = list_concat_copy(w->subparams, ((SubPlan *) n)->paramIds);
	else if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && !list_member_int(w->subparams, ((Param *) n)->paramid) &&
			 !array_initplan_ok(w->c, (Param *) n))
	{
		w->bad = true;
		return false;
	}
	else if (IsA(n, SubPlan) || IsA(n, SubLink) || IsA(n, Aggref) || IsA(n, WindowFunc) || IsA(n, GroupingFunc) ||
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
	LeafWalk	w = {c, -1, InvalidOid, -1, InvalidOid, false, NIL};

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

/*
 * PostgreSQL hashes the array of x = ANY (array) when it is a constant; one that comes from a parameter (or an ARRAY sub-select) is searched linearly for every
 * value the executor tests (a dictionary of 200k entries against 5000 elements takes seconds). The executor builds the hash table on the
 * first call and keeps it, so the parameter is read once.
 */
static void
hash_param_saop(ScalarArrayOpExpr *sa)
{
	Node	   *arr = lsecond(sa->args);
	Oid			op = sa->opno,
				lh,
				rh;

	if (!IsA(arr, Param) || (((Param *) arr)->paramkind != PARAM_EXTERN && ((Param *) arr)->paramid < SIFT_AGG_INIT_PARAM) || OidIsValid(sa->hashfuncid))
		return;
	if (!sa->useOr && !OidIsValid(op = get_negator(op)))
		return;
	if (!get_op_hash_functions(op, &lh, &rh) || lh != rh)
		return;
	sa->hashfuncid = lh;
	sa->negfuncid = sa->useOr ? InvalidOid : get_opcode(op);
}

static Node *
subst_mutator(Node *n, SubstCtx *s)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, ScalarArrayOpExpr))
	{
		Node	   *r = expression_tree_mutator(n, subst_mutator, s);

		hash_param_saop((ScalarArrayOpExpr *) r);
		return r;
	}
	if ((IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType)) &&
		match_leaf(s->c, n) >= 0)
		return (Node *) copyObject(s->param);
	if (IsA(n, SubPlan))
	{
		s->c->nsubplan++;
		s->c->subplan_cost += ((SubPlan *) n)->startup_cost;
	}
	else if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && !null_input_param(n))
		foreach_ptr(SubPlan, sp, s->c->root->init_plans)
			if (list_member_int(sp->setParam, ((Param *) n)->paramid))
			{
				Param	   *init = copyObject((Param *) n);

				s->c->nsubplan++;
				s->c->subplan_cost += sp->startup_cost;
				init->paramid += SIFT_AGG_INIT_PARAM;
				return (Node *) init;
			}
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

	return l->kind == SCL_SCALAR && !l->wide && (l->vtype == SCV_CODE || (l->vtype == SCV_I32 && !type_is_enum(l->typid)));
}

/* a leaf an expression PostgreSQL evaluates may read: a combo gives an id to each distinct value of a dictionary, integer, 64-bit integer (numeric included) or float leaf */
static bool
combo_leaf_ok(AggCtx *c, int leaf)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);

	return l->kind == SCL_SCALAR && (l->vtype == SCV_CODE || l->vtype == SCV_I32 || l->vtype == SCV_I64 || l->vtype == SCV_F64);
}

/* an integer division or remainder by a constant other than 0 and -1 (the overflow of the smallest value) cannot raise */
static bool
safe_int_divisor(const char *name, List *args)
{
	Const	   *k;

	if (strncmp(name, "int", 3) != 0 || (!strstr(name, "div") && !strstr(name, "mod")) || list_length(args) != 2)
		return false;
	k = (Const *) lsecond(args);
	if (!IsA(k, Const) || k->constisnull || (k->consttype != INT2OID && k->consttype != INT4OID && k->consttype != INT8OID))
		return false;
	return k->consttype == INT2OID ? DatumGetInt16(k->constvalue) != 0 && DatumGetInt16(k->constvalue) != -1 :
		k->consttype == INT4OID ? DatumGetInt32(k->constvalue) != 0 && DatumGetInt32(k->constvalue) != -1 :
		DatumGetInt64(k->constvalue) != 0 && DatumGetInt64(k->constvalue) != -1;
}

/*
 * Functions that raise an error for some values of their input (a zero divisor, a negative root, a text that is no number). A lookup table
 * evaluates its expression on every value of the dictionary or of the range of the unit, also on values no selected row has; the combos
 * evaluate it for the values of the tuples that reach it (docs/COL-PLAN.md, "Expressions PostgreSQL evaluates").
 */
static bool
raising_walker(Node *n, void *context)
{
	static const char *const names[] = {"int2div", "int4div", "int8div", "int24div", "int42div", "int28div", "int82div", "int48div", "int84div",
		"int2mod", "int4mod", "int8mod", "numeric_div", "numeric_mod", "numeric_div_trunc", "float8div", "float4div", "float48div",
		"float84div", "dpow", "numeric_power", "dlog1", "dlog10", "numeric_ln", "numeric_log", "dsqrt", "numeric_sqrt", "dcbrt", "acos",
		"asin", "acosh", "atanh", "trunc_div"};
	Oid			fid = InvalidOid;
	List	   *args = NIL;

	if (n == NULL)
		return false;
	if (IsA(n, Query))
		return query_tree_walker((Query *) n, raising_walker, context, 0);
	/* a context: a sub-select, whose own sub-plans and init plans may raise too (more than one row) */
	if (IsA(n, CoerceViaIO) || (context && (IsA(n, SubPlan) || IsA(n, SubLink) || (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC))))
		return true;
	if (IsA(n, FuncExpr))
	{
		fid = ((FuncExpr *) n)->funcid;
		args = ((FuncExpr *) n)->args;
	}
	else if (IsA(n, OpExpr))
	{
		set_opfuncid((OpExpr *) n);
		fid = ((OpExpr *) n)->opfuncid;
		args = ((OpExpr *) n)->args;
	}
	if (OidIsValid(fid))
	{
		const char *name = get_func_name(fid);

		for (int i = 0; name && i < lengthof(names); i++)
			if (strcmp(name, names[i]) == 0 && !safe_int_divisor(name, args))
				return true;
	}
	return expression_tree_walker(n, raising_walker, context);
}

/* the span of the values the statistics of an integer column (histogram, most common values) have seen; false: none, or the leaf is not a plain column */
static bool
leaf_stats_range(AggCtx *c, int leaf, int64 *lo, int64 *hi)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);
	Node	   *e = l->path == NIL && l->parent < 0 ? list_nth(c->attexpr, l->attno - 1) : NULL;
	HeapTuple	tp;
	AttStatsSlot sslot;
	bool		known = false;

	*lo = PG_INT64_MAX;
	*hi = PG_INT64_MIN;
	if (e == NULL || !IsA(e, Var))
		return false;
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid),
						 Int16GetDatum(((Var *) e)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tp))
		return false;
	for (int k = 0; k < 2; k++)
		if (get_attstatsslot(&sslot, tp, k == 0 ? STATISTIC_KIND_HISTOGRAM : STATISTIC_KIND_MCV, InvalidOid, ATTSTATSSLOT_VALUES))
		{
			for (int i = 0; i < sslot.nvalues; i++)
			{
				int64		v = sift_aggspec_operand_int(l->typid, sslot.values[i]);

				*lo = Min(*lo, v);
				*hi = Max(*hi, v);
				known = true;
			}
			free_attstatsslot(&sslot);
		}
	ReleaseSysCache(tp);
	return known;
}

/* the values of an integer leaf the lookup table covers: the span of the column's statistics fits, or there are none */
static bool
lut_range_ok(AggCtx *c, int leaf)
{
	int64		lo,
				hi;

	return !leaf_stats_range(c, leaf, &lo, &hi) || (double) hi - (double) lo < (double) (1 << 22);
}

/* is an expression of one dictionary or integer leaf evaluated by a lookup table (once per value of the dictionary or of the range of the leaf)? */
static bool
lut_expr_ok(AggCtx *c, int leaf, Node *expr)
{
	return lut_leaf_ok(c, leaf) && !raising_walker(expr, NULL) && (leaf_of(c, leaf)->vtype == SCV_CODE || lut_range_ok(c, leaf));
}

/*
 * Calendar keys of a timestamp leaf. date_trunc, ::date and extract of a timestamp change value only on a second, minute, hour or
 * day boundary, so the executor evaluates them once per such step of the data instead of once per distinct timestamp (which is
 * once per row): the key id of a row is a lookup of its step.
 */
typedef struct BucketUnit
{
	const char *name;
	int			secs;			/* the step the result changes on */
	double		len;			/* average seconds of a unit */
	int			cycle;			/* extract: the values it repeats after, 0: it does not */
} BucketUnit;

#define DAY_SECS		86400
#define MONTH_SECS		2629800
#define YEAR_SECS		31557600.0

/* date_trunc units (a week, month and longer ones change on a day) */
static const BucketUnit trunc_units[] = {{"second", 1, 1, 0}, {"minute", 60, 60, 0}, {"hour", 3600, 3600, 0}, {"day", DAY_SECS, DAY_SECS, 0},
	{"week", DAY_SECS, 7 * DAY_SECS, 0}, {"month", DAY_SECS, MONTH_SECS, 0}, {"quarter", DAY_SECS, 3 * MONTH_SECS, 0},
	{"year", DAY_SECS, YEAR_SECS, 0}, {"decade", DAY_SECS, 10 * YEAR_SECS, 0}, {"century", DAY_SECS, 100 * YEAR_SECS, 0},
	{"millennium", DAY_SECS, 1000 * YEAR_SECS, 0}, {NULL, 0, 0, 0}};

/* extract fields that are whole numbers of a step (second, epoch and julian have fractions of a second) */
static const BucketUnit field_units[] = {{"minute", 60, 60, 60}, {"hour", 3600, 3600, 24}, {"day", DAY_SECS, DAY_SECS, 31},
	{"dow", DAY_SECS, DAY_SECS, 7}, {"isodow", DAY_SECS, DAY_SECS, 7}, {"doy", DAY_SECS, DAY_SECS, 366}, {"week", DAY_SECS, 7 * DAY_SECS, 53},
	{"month", DAY_SECS, MONTH_SECS, 12}, {"quarter", DAY_SECS, 3 * MONTH_SECS, 4}, {"year", DAY_SECS, YEAR_SECS, 0},
	{"isoyear", DAY_SECS, YEAR_SECS, 0}, {"decade", DAY_SECS, 10 * YEAR_SECS, 0}, {"century", DAY_SECS, 100 * YEAR_SECS, 0},
	{"millennium", DAY_SECS, 1000 * YEAR_SECS, 0}, {NULL, 0, 0, 0}};

/* the unit of a date_trunc, date_part or extract of a timestamp; NULL for another call, or a unit this does not know */
static const BucketUnit *
bucket_unit(Node *e)
{
	FuncExpr   *f;
	Const	   *k;
	char	   *name;

	if (!IsA(e, FuncExpr))
		return NULL;
	f = (FuncExpr *) e;
	if (f->funcid != F_DATE_TRUNC_TEXT_TIMESTAMP && f->funcid != F_DATE_PART_TEXT_TIMESTAMP && f->funcid != F_EXTRACT_TEXT_TIMESTAMP)
		return NULL;
	k = linitial(f->args);
	if (!IsA(k, Const) || k->constisnull || k->consttype != TEXTOID)
		return NULL;
	name = TextDatumGetCString(k->constvalue);
	for (const BucketUnit *u = f->funcid == F_DATE_TRUNC_TEXT_TIMESTAMP ? trunc_units : field_units; u->name; u++)
		if (pg_strcasecmp(u->name, name) == 0)
			return u;
	return NULL;
}

/* the leaf of the expression of a key: a reference, or Param 0 once the key is compiled */
static bool
bucket_leaf(AggCtx *c, Node *e)
{
	return IsA(e, Param) || match_leaf(c, e) >= 0;
}

/*
 * The seconds an expression of the timestamp leaf keeps its value for: the leaf itself 1 (every function below has a step of a
 * second or more), a calendar function the coarser of its own step and the one of its argument. 0: the expression is something
 * else, and the key is a combo.
 */
static int
bucket_step(AggCtx *c, Node *e)
{
	const BucketUnit *u;
	int			arg;

	if (bucket_leaf(c, e))
		return 1;
	if (IsA(e, RelabelType))
		return bucket_step(c, (Node *) ((RelabelType *) e)->arg);
	if (!IsA(e, FuncExpr))
		return 0;
	if ((u = bucket_unit(e)) != NULL)
	{
		arg = bucket_step(c, lsecond(((FuncExpr *) e)->args));
		return arg ? Max(arg, u->secs) : 0;
	}
	switch (((FuncExpr *) e)->funcid)
	{
		case F_DATE_TIMESTAMP:
			arg = bucket_step(c, linitial(((FuncExpr *) e)->args));
			return arg ? Max(arg, DAY_SECS) : 0;
		case F_TIMESTAMP_DATE:
			return bucket_step(c, linitial(((FuncExpr *) e)->args));
		default:
			return 0;
	}
}

/* the distinct values an expression that bucket_step accepts takes over a stretch of `span` seconds of the leaf */
static double
bucket_groups(AggCtx *c, Node *e, double span)
{
	const BucketUnit *u;
	double		n;

	if (bucket_leaf(c, e))
		return span + 1;
	if (IsA(e, RelabelType))
		return bucket_groups(c, (Node *) ((RelabelType *) e)->arg, span);
	if ((u = bucket_unit(e)) != NULL)
	{
		n = Min(bucket_groups(c, lsecond(((FuncExpr *) e)->args), span), span / u->len + 1);
		return u->cycle ? Min(n, u->cycle) : n;
	}
	if (((FuncExpr *) e)->funcid == F_DATE_TIMESTAMP)
		return Min(bucket_groups(c, linitial(((FuncExpr *) e)->args), span), span / DAY_SECS + 1);
	return bucket_groups(c, linitial(((FuncExpr *) e)->args), span);
}

/* the groups of a bucket key from the statistics of its column; 0: unknown */
static double
bucket_key_groups(AggCtx *c, const SiftAggKey *k)
{
	int64		lo,
				hi;

	if (!leaf_stats_range(c, k->leaf, &lo, &hi))
		return 0;
	return bucket_groups(c, k->expr, (double) (hi - lo) / USECS_PER_SEC);
}

/* the groups of a key whose number the planner knows: a calendar bucket from the statistics of its column, a function of the JSON type of a path (at most seven); 0: unknown */
static double
key_groups(AggCtx *c, const SiftAggKey *k)
{
	int			nbase = c->lay->nleaves - c->nvleaf;

	if ((k->kind == SAK_LEAF || k->kind == SAK_LUT) && k->leaf >= nbase && c->vleaf[k->leaf - nbase].types)
		return 7;
	return k->bucket > 0 ? bucket_key_groups(c, k) : 0;
}

/* EXPLAIN: the step a key on a timestamp is evaluated for */
static const char *
bucket_text(int secs)
{
	return secs == 1 ? " (per second)" : secs == 60 ? " (per minute)" : secs == 3600 ? " (per hour)" : " (per day)";
}

/* the seconds a key on the timestamp leaf is evaluated once for, 0 when it is not a calendar expression */
static int
bucket_key_secs(AggCtx *c, int leaf, Node *expr)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);

	if (l->kind != SCL_SCALAR || l->vtype != SCV_I64 || l->typid != TIMESTAMPOID || contain_volatile_functions(expr))
		return 0;
	return bucket_step(c, expr);
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
	p->leaf2 = -1;
	p->desc = pstrdup("");
	return p;
}

static Node *
display_expr_mutator(Node *n, void *context)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, PlaceHolderVar))
		return display_expr_mutator((Node *) ((PlaceHolderVar *) n)->phexpr, context);
	return expression_tree_mutator(n, display_expr_mutator, context);
}

/* the expression as EXPLAIN shows it, its Vars resolved through the query's range table */
static char *
expr_text(AggCtx *c, Node *n)
{
	if (c->dpctx == NIL)
	{
		PlannedStmt *stmt = makeNode(PlannedStmt);
		Result	   *plan = makeNode(Result);

		stmt->rtable = c->rtable ? c->rtable : c->root->parse->rtable;
		c->dpctx = deparse_context_for_plan_tree(stmt, select_rtable_names_for_explain(stmt->rtable, NULL));
		/* the Params of a sub-plan are looked up in the plan the expression belongs to; the InitPlans of the query give
		 * their output Params (the deparser asserts on a Param that nothing generates) */
		plan->plan.initPlan = c->root->init_plans;
		c->dpctx = set_deparse_context_plan(c->dpctx, (Plan *) plan, NIL);
	}
	return deparse_expression(display_expr_mutator(n, NULL), c->dpctx, true, false);
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
	if (c->rec_pending == leaf + 1)
		c->rec_ok = bms_add_member(c->rec_ok, leaf);
	else
		c->rec_bad = bms_add_member(c->rec_bad, leaf);
	c->rec_pending = 0;
	c->used = bms_add_member(c->used, leaf);
	c->read = bms_add_member(c->read, leaf);
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

/* a column of a joined plan the spec reads (keys and columns): the path outputs it */
static void
join_var(JoinCtx *jc, Var *v)
{
	foreach_ptr(Var, x, jc->cvars)
		if (x->varattno == v->varattno)
			return;
	jc->cvars = lappend(jc->cvars, copyObject(v));
}

/* the index of column v of the joined table among the columns the expressions read; -1 when there are too many */
static int
join_col(JoinCtx *jc, Var *v)
{
	SiftAggJoin *j = &jc->j;

	join_var(jc, v);
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
		lvl = Max(lvl, cb->leaf[p] >= 0 ? leaf_level(c, cb->leaf[p]) : combo_level(c, list_nth(c->combos, cb->refcombo[p])));
	return lvl;
}

/* An input from an earlier lookup is identified by that output row, including its NULL extension. */
static int
combo_reference(AggCtx *c, ComboCtx *cb, JoinCtx *jc, Var *v)
{
	int			col = join_col(jc, v),
				p = 0;

	if (col < 0 || jc->combo >= combo_index(c, cb))
		return -1;
	while (p < cb->nl && !(cb->leaf[p] == -1 && cb->refcombo[p] == jc->combo && cb->refcol[p] == col))
		p++;
	if (p == cb->nl)
	{
		if (p == SIFT_AGG_MAX_CLEAVES)
			return -1;
		cb->leaf[p] = -1;
		cb->refcombo[p] = jc->combo;
		cb->refcol[p] = col;
		cb->ptype[p] = v->vartype;
		cb->ptypmod[p] = v->vartypmod;
		cb->pcoll[p] = v->varcollid;
		cb->typed[p] = true;
		cb->nl++;
	}
	return p;
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
	join_var(jc, (Var *) m);
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
	bool		null_input;
	bool		bad;
	List	   *subparams;		/* the output Params of the hashed sub-plans met (as LeafWalk) */
} ComboWalk;

static bool
combo_walker(Node *n, ComboWalk *w)
{
	int			id;
	JoinCtx    *jc;

	if (n == NULL || w->bad)
		return false;
	if (null_input_param(n))
	{
		w->null_input = true;
		return false;
	}
	if (IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType))
	{
		if ((jc = join_of_var(w->c, n)) != NULL)
		{
			if (w->jc == NULL || jc->combo > w->jc->combo)
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
			w->bad |= !combo_leaf_ok(w->c, id);
			return false;
		}
		w->bad |= IsA(n, Var);
	}
	if (IsA(n, SubPlan) && lut_subplan_ok(w->c, (SubPlan *) n))
	{
		w->subparams = list_concat_copy(w->subparams, ((SubPlan *) n)->paramIds);
		w->c->nsubplan++;
		w->c->subplan_cost += ((SubPlan *) n)->startup_cost;
	}
	else if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && list_member_int(w->subparams, ((Param *) n)->paramid))
		return false;
	else if ((IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC) || IsA(n, SubPlan) || IsA(n, SubLink) || IsA(n, Aggref) ||
			 IsA(n, WindowFunc) || IsA(n, GroupingFunc) || IsA(n, PlaceHolderVar) || IsA(n, CurrentOfExpr) || IsA(n, NextValueExpr) ||
			 IsA(n, SetToDefault))
		w->bad = true;
	return !w->bad && expression_tree_walker(n, combo_walker, w);
}

/* a divisor that is a constant other than zero */
static bool
nonzero_const(Node *n)
{
	Const	   *k = (Const *) strip_relabel(n);

	if (!IsA(k, Const) || k->constisnull)
		return false;
	if (k->consttype == NUMERICOID)
		return !DatumGetBool(DirectFunctionCall2(numeric_eq, k->constvalue, NumericGetDatum(int64_to_numeric(0))));
	return false;
}

/* can the function raise an error on some argument? Only what is known not to: leakproof functions (PostgreSQL's own promise), numeric arithmetic, and the widening of a leaf */
static bool
func_may_raise(Oid fid, List *args)
{
	switch (fid)
	{
		case F_NUMERIC_ADD:
		case F_NUMERIC_SUB:
		case F_NUMERIC_MUL:
		case F_NUMERIC_UMINUS:
		case F_NUMERIC_ABS:
		case F_ABS_NUMERIC:
		case F_SIGN_NUMERIC:
		case F_CEIL_NUMERIC:
		case F_CEILING_NUMERIC:
		case F_FLOOR_NUMERIC:
		case F_NUMERIC_INT2:
		case F_NUMERIC_INT4:
		case F_NUMERIC_INT8:
			return false;
		case F_ROUND_NUMERIC_INT4:
		case F_TRUNC_NUMERIC_INT4:
			return !IsA(strip_relabel(lsecond(args)), Const);
		case F_NUMERIC_DIV:
		case F_NUMERIC_MOD:
			return !nonzero_const(lsecond(args));
		case F_FLOAT8_NUMERIC:
			return !IsA(strip_relabel(linitial(args)), Var);
		default:
			return !get_func_leakproof(fid);
	}
}

/* may the expression raise an error (a division by zero, a cast, an overflow)? The leaves it reads are the Vars of the table */
static bool
may_raise_walker(Node *n, AggCtx *c)
{
	if (n == NULL)
		return false;
	if ((IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType)) && match_leaf(c, n) >= 0)
		return false;
	switch (nodeTag(n))
	{
		case T_Var:
		case T_Const:
		case T_Param:
		case T_RelabelType:
		case T_BoolExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_CaseExpr:
		case T_CaseWhen:
		case T_CoalesceExpr:
		case T_ArrayExpr:
			break;
		case T_FuncExpr:
			if (func_may_raise(((FuncExpr *) n)->funcid, ((FuncExpr *) n)->args))
				return true;
			break;
		case T_OpExpr:
		case T_DistinctExpr:
			{
				OpExpr	   *o = (OpExpr *) n;

				Oid			lt,
							rt;

				set_opfuncid(o);
				if (builtin_cmp(o->opno, &lt, &rt) < 0 && func_may_raise(o->opfuncid, o->args))
					return true;
			}
			break;
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) n;
				Oid			lt,
							rt;

				set_sa_opfuncid(sa);
				if (builtin_cmp(sa->opno, &lt, &rt) < 0 && func_may_raise(sa->opfuncid, sa->args))
					return true;
			}
			break;
		default:
			return true;
	}
	return expression_tree_walker(n, may_raise_walker, c);
}


/*
 * `column = expression of leaves` of the joined table: PostgreSQL evaluates the expression once per distinct tuple of the leaves'
 * values and the table is probed with the result (the key is not a leaf, so there is no id to read it from).
 */
static bool
join_expr_clause(AggCtx *c, JoinCtx *jc, Node *q)
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
	ComboWalk	w = {c};
	TypeClass	tc;
	Oid			mt,
				et;

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
	mt = ((Var *) m)->vartype;
	et = exprType(e);
	tc = type_class(lt);
	if (bms_is_member(jc->rti, pull_varnos(c->root, e)) || contain_volatile_functions(e))
		return false;
	if (lt == TEXTOID)
	{
		if (rt != TEXTOID || (mt != TEXTOID && mt != VARCHAROID) || (et != TEXTOID && et != VARCHAROID) ||
			!OidIsValid(op->inputcollid) || !get_collation_isdeterministic(op->inputcollid))
			return false;
	}
	else if ((tc != TC_INT && tc != TC_DATE) || tc != type_class(rt) || tc != type_class(mt) || tc != type_class(et))
		return false;
	combo_walker(e, &w);
	if (w.bad || (w.jc != NULL && w.jc->combo >= jc->combo) || (w.nl == 0 && w.vars == NIL))
		return false;
	for (int p = 0; p < w.nl; p++)
		if (combo_leaf(cb, w.leaf[p], true, w.ptype[p], w.ptypmod[p], w.pcoll[p]) < 0)
			return false;
	for (int p = 0; p < w.nl; p++)
		use_leaf(c, w.leaf[p]);
	foreach_ptr(Var, v, w.vars)
		if (combo_reference(c, cb, join_of_var(c, (Node *) v), v) < 0)
			return false;
	join_var(jc, (Var *) m);
	j->keyatt[j->nkey] = ((Var *) m)->varattno;
	j->keytype[j->nkey] = mt;
	j->keypos[j->nkey] = -1;
	jc->kexpr[j->nkey] = e;
	j->nkey++;
	return true;
}

/*
 * a slot for expr in the combo of its leaves (and of the table it reads columns of): its combo and slot numbers; false if it does not
 * fit. `force`: the join whose combo holds the slot, though the expression may read no column of its table (the rest of an ON clause).
 * `after_joins`: a deferred combo reads lookup outputs as inputs, so the expression is evaluated only for surviving joined tuples.
 */
static bool
combo_slot_in(AggCtx *c, JoinCtx *force, Node *expr, bool is_key, bool after_joins, int *ci, int *si)
{
	ComboWalk	w = {c};
	ComboCtx   *cb = NULL;
	SlotReq    *rq;

	if (contain_volatile_functions(expr))
		return false;
	combo_walker(expr, &w);
	if (force && w.jc != NULL && w.jc->combo > force->combo)
		return false;
	if (after_joins)
		w.jc = NULL;
	else if (force)
		w.jc = force;
	if (w.bad || (w.nl == 0 && w.jc == NULL && !w.null_input && (!after_joins || w.vars == NIL)))
		return false;
	if (w.jc)
		cb = list_nth(c->combos, w.jc->combo);
	else if (!after_joins)
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
	{
		JoinCtx    *jc = join_of_var(c, (Node *) v);

		if ((jc == w.jc ? join_col(jc, v) : combo_reference(c, cb, jc, v)) < 0)
			return false;
	}
	for (int p = 0; p < w.nl; p++)
		use_leaf(c, w.leaf[p]);
	rq = palloc0(sizeof(SlotReq));
	rq->expr = expr;
	rq->is_key = is_key;
	rq->agg = -1;
	rq->late = !is_key && w.jc == NULL && may_raise_walker(expr, c);
	cb->slots = lappend(cb->slots, rq);
	*ci = combo_index(c, cb);
	*si = list_length(cb->slots) - 1;
	return true;
}

static bool
combo_slot(AggCtx *c, Node *expr, bool is_key, int *ci, int *si)
{
	return combo_slot_in(c, NULL, expr, is_key, false, ci, si);
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
	if (IsA(n, ScalarArrayOpExpr))
	{
		Node	   *r = expression_tree_mutator(n, combo_sub_mutator, s);

		hash_param_saop((ScalarArrayOpExpr *) r);
		return r;
	}
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
		if (join_of_var(s->c, n) != NULL)
		{
			Var		   *v = (Var *) n;

			p->paramid = combo_reference(s->c, s->cb, join_of_var(s->c, n), v);
			Assert(p->paramid >= 0);
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
			o->refcombo[p] = cb->refcombo[p];
			o->refcol[p] = cb->refcol[p];
			o->ptype[p] = cb->ptype[p];
			o->ptypmod[p] = cb->ptypmod[p];
			o->pcoll[p] = cb->pcoll[p];
		}
		o->level = combo_level(c, cb);
		if (sub.jc)
		{
			s->join[cb->join].args = NIL;
			foreach_ptr(Node, arg, sub.jc->j.args)
				s->join[cb->join].args = lappend(s->join[cb->join].args, combo_sub_mutator(arg, &sub));
		}
		for (int k = 0; sub.jc && k < sub.jc->j.nkey; k++)
			if (sub.jc->kexpr[k])
				s->join[cb->join].keyexpr[k] = combo_sub_mutator(sub.jc->kexpr[k], &sub);
		o->nslots = list_length(cb->slots);
		o->slot = palloc0(Max(o->nslots, 1) * sizeof(SiftAggSlot));
		foreach_ptr(SlotReq, rq, cb->slots)
		{
			SiftAggSlot *sl = &o->slot[foreach_current_index(rq)];

			sl->is_key = rq->is_key;
			sl->key = rq->key;
			sl->agg = rq->agg;
			sl->direct = rq->direct;
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
		appendStringInfo(&s, "%s%s", p ? ", " : "", cb->leaf[p] >= 0 ? leaf_name(c, cb->leaf[p]) : "lookup output");
	return psprintf("%s: %s", s.data, expr_text(c, expr));
}

static SlotReq *slot_req(AggCtx *c, int combo, int slot);

/* a boolean expression of several leaves, or of leaves and the columns of the matching row of a joined table */
static SiftAggPred *
compile_combo_pred_at(AggCtx *c, Node *n, bool after_joins)
{
	int			ci,
				si;
	SiftAggPred *p;
	ComboCtx   *cb;

	if (exprType(n) != BOOLOID || !combo_slot_in(c, NULL, n, false, after_joins, &ci, &si))
		return NULL;
	cb = list_nth(c->combos, ci);
	p = new_pred(c, SAP_COMBO);
	p->leaf = ci;
	p->op = si;
	p->level = after_joins || cb->join >= 0 ? 2 : combo_level(c, cb);
	p->late = slot_req(c, ci, si)->late;
	p->desc = combo_text(c, ci, n);
	return p;
}

static SiftAggPred *
compile_combo_pred(AggCtx *c, Node *n)
{
	return compile_combo_pred_at(c, n, false);
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
			return nx_walk(c, linitial(args), o, scale, dig) && nx_rescale(o, SNX_ROUND, 0, scale, dig) && *dig <= maxd &&
				nx_put(o, SNX_INTCAST, -1, fid == F_INT2_NUMERIC ? 2 : fid == F_INT4_NUMERIC ? 4 : 8, 0, 0);
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

/* does the program raise for a NaN (a cast to an integer)? */
static bool
nx_raises(const SiftAggNum *o)
{
	for (int k = 0; k < o->n; k++)
		if (o->step[k].op == SNX_INTCAST)
			return true;
	return false;
}

/* numeric expression op operand; two expressions are compared as the sign of their comparison (SNX_CMP3) against 0 */
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
		if (!nx_walk(c, rhs, &p->num, &s2, &d2) || !nx_addsub(&p->num, SNX_CMP3, &p->num.scale, &dig, s2, d2))
			return NULL;
		operand = (Node *) makeConst(NUMERICOID, -1, InvalidOid, -1, NumericGetDatum(int64_to_numeric(0)), false, false);
	}
	if ((p->level = nx_level(c, &p->num)) < 0)
		return NULL;
	p->op = cmp;
	p->expr = operand;
	p->argtype = NUMERICOID;
	p->late = nx_raises(&p->num);
	p->desc = psprintf("%s %s %s", expr_text(c, lhs), cmp_text(cmp), expr_text(c, rhs));
	return p;
}

/* the leaf of the outer row that a parameter of the EXISTS sub-plan stands for, -1 when n is not one */
static int
outer_leaf(AggCtx *c, Node *n)
{
	n = strip_relabel(n);
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
		foreach_ptr(OuterParam, o, c->oparams)
			if (o->id == ((Param *) n)->paramid)
				return o->leaf;
	return -1;
}

/* a stored scalar that the comparison of an element with a row reads as integers or doubles: the type classes of both sides are the same */
static bool
elem_row_leaf(AggCtx *c, int leaf, TypeClass tc)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);

	return l->kind == SCL_SCALAR && !l->lossy && !l->wide && type_class(l->typid) == tc &&
		((l->vtype == SCV_I32 || l->vtype == SCV_I64) ? tc != TC_FLOAT : tc == TC_FLOAT && l->vtype == SCV_F64);
}

/*
 * `element leaf op column of the row` in the test of an EXISTS (the column reaches the sub-query as a parameter of the sub-plan): a test of the
 * element leaf and of the row leaf of the same type class (an int4 member against a bigint column, not against a float8 one), true for the
 * elements whose row passes the comparison, which is false when either side is NULL. An expression of the column, or of the element, is not a leaf.
 */
static SiftAggPred *
compile_elem_row(AggCtx *c, int cmp, Node *l, Node *r, Oid lt, Oid rt)
{
	SiftAggPred *p;
	TypeClass	tc = type_class(lt);
	int			el,
				rl;

	if (c->oparams == NIL)
		return NULL;
	if (outer_leaf(c, l) >= 0)
	{
		Node	   *t = l;

		l = r;
		r = t;
		cmp = commute(cmp);
	}
	if ((rl = outer_leaf(c, r)) < 0 || (el = match_leaf(c, l)) < 0 || tc == TC_NUMERIC || tc == TC_BOOL || type_class(rt) != tc ||
		leaf_of(c, el)->parent != c->group || leaf_of(c, rl)->parent >= 0 || !elem_row_leaf(c, el, tc) || !elem_row_leaf(c, rl, tc))
		return NULL;
	p = new_pred(c, SAP_ELEM_ROW);
	p->leaf = el;
	p->leaf2 = rl;
	p->op = cmp;
	p->level = 1;
	use_leaf(c, el);
	use_leaf(c, rl);
	p->desc = psprintf("%s %s %s", leaf_name(c, el), cmp_text(cmp), leaf_name(c, rl));
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
	if ((p = compile_elem_row(c, cmp, l, r, lt, rt)) != NULL)
		return p;
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
	if (lf->kind == SCL_SCALAR && lf->typid == NUMERICOID && lf->vtype == SCV_I64 && type_class(lt) == TC_NUMERIC)
		return compile_num_in(c, sa, leaf, r);
	if (lf->kind != SCL_SCALAR || (lf->vtype != SCV_I32 && lf->vtype != SCV_I64) || type_class(lf->typid) == TC_NONE ||
		type_class(lf->typid) != type_class(lt) || type_class(lf->typid) != type_class(rt) ||
		type_class(lf->typid) == TC_FLOAT)
		return NULL;
	p = make_leaf_pred(c, SAP_IN, leaf);
	p->op = !sa->useOr;
	p->expr = r;
	p->argtype = rt;
	p->desc = psprintf("%s %s %s", leaf_name(c, leaf), sa->useOr ? "IN" : "NOT IN", expr_text(c, r));
	p->sel = clause_selectivity(c->root, (Node *) sa, c->relid, JOIN_INNER, NULL);
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
	if (!single_leaf(c, n, &leaf, &ptype, &ptypmod, &pcoll) || !lut_expr_ok(c, leaf, n))
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
		p->late |= p->child[i]->late;
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

/* a condition that reads no column and nothing the query changes (stable functions, parameters): PostgreSQL's one-time filter */
static bool
is_gate(Node *n)
{
	return exprType(n) == BOOLOID && pseudo_const(n);
}

/* the SIFT_SRC_JT_* bits of the type names jsonb_typeof gives (a JSON null is 'null'); another string has none */
static uint32
jtype_bits(List *names)
{
	static const struct
	{
		const char *name;
		uint32		bit;
	}			known[] = {
		{"string", SIFT_SRC_JT_STRING}, {"number", SIFT_SRC_JT_NUMBER}, {"boolean", SIFT_SRC_JT_BOOLEAN},
		{"object", SIFT_SRC_JT_OBJECT}, {"array", SIFT_SRC_JT_ARRAY}, {"null", SIFT_SRC_JT_NULL}
	};
	uint32		bits = 0;

	foreach_ptr(String, s, names)
		for (int t = 0; t < lengthof(known); t++)
			if (strcmp(strVal(s), known[t].name) == 0)
				bits |= known[t].bit;
	return bits;
}

static bool
typeof_type(Oid typid)
{
	return typid == BOOLOID || typid == INT2OID || typid == INT4OID || typid == INT8OID || typid == FLOAT4OID || typid == FLOAT8OID ||
		typid == NUMERICOID || typid == TEXTOID;
}

/* the leaf of the jsonb value `x->'k'` whose chunks know the JSON type of the path: a scalar of a type a JSON value of one type fills, or the text of any scalar (astext), whose chunks keep the number and boolean kinds */
static int
typeof_leaf(AggCtx *c, Node *e)
{
	JsonRef		r = {0};

	if (!json_access(c, e, &r) || r.as_text || r.att == 0 || r.path == NIL)
		return -1;
	for (int i = 0; i < c->lay->nleaves - c->nvleaf; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == r.att && l->parent < 0 && l->kind == SCL_SCALAR && !l->lossy && l->name[0] != '#' &&
			paths_equal(l->path, r.path) && typeof_type(l->typid))
			return i;
	}
	return -1;
}

static bool
source_can_types(AggCtx *c)
{
	return OidIsValid(c->indexoid) ? sift_colsrc_can_types(get_rel_relam(c->indexoid), true) : sift_colsrc_can_types(get_rel_relam(c->heapoid), false);
}

/* the constant text operand of `jsonb_typeof(..) = 'a'` as a name list, with the call in *call; either side */
static bool
typeof_cmp_operands(OpExpr *op, Node **call, List **names)
{
	Node	   *l = strip_relabel(linitial(op->args)),
			   *r = strip_relabel(lsecond(op->args));

	if (func_of(l) != F_JSONB_TYPEOF)
	{
		Node	   *t = l;

		l = r;
		r = t;
	}
	if (!IsA(r, Const) || ((Const *) r)->constisnull)
		return false;
	*call = l;
	*names = list_make1(makeString(TextDatumGetCString(((Const *) r)->constvalue)));
	return true;
}

/*
 * A test of `jsonb_typeof(x->'k')` as the SIFT_SRC_JT_* types it keeps: `= 'a'` and `IN (..)`, their complements `<>` and `NOT IN` and IS [NOT]
 * NULL. The type is NULL where the path has no value, which no comparison keeps, so a complement is the other types but none. *call is the
 * jsonb_typeof call. False for any other test, and for a type no row has.
 */
static bool
typeof_test(Node *n, Node **call, uint32 *types)
{
	Oid			eq = TextEqualOperator,
				ne = get_negator(TextEqualOperator);
	List	   *names = NIL;
	bool		negate;

	if (IsA(n, NullTest))
	{
		*call = strip_relabel((Node *) ((NullTest *) n)->arg);
		*types = SIFT_SRC_JT_NONE;
		negate = ((NullTest *) n)->nulltesttype == IS_NOT_NULL;
	}
	else if (IsA(n, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) n;

		if (list_length(sa->args) != 2 || !((sa->opno == eq && sa->useOr) || (sa->opno == ne && !sa->useOr)) || !path_keys(lsecond(sa->args), &names))
			return false;
		*call = strip_relabel(linitial(sa->args));
		*types = jtype_bits(names);
		negate = !sa->useOr;
	}
	else if (IsA(n, OpExpr) && (((OpExpr *) n)->opno == eq || ((OpExpr *) n)->opno == ne) && list_length(((OpExpr *) n)->args) == 2)
	{
		if (!typeof_cmp_operands((OpExpr *) n, call, &names))
			return false;
		*types = jtype_bits(names);
		negate = ((OpExpr *) n)->opno == ne;
	}
	else
		return false;
	if (negate)
		*types = SIFT_SRC_JT_ALL & ~SIFT_SRC_JT_NONE & ~*types;
	return *types != 0;
}

/*
 * Does a test of these types (SIFT_SRC_JT_*) tell what only the kinds of a chunk say: a JSON null from a missing key (the types have one of them
 * and not the other), or the types of the values of an astext leaf apart (some of the string, number and boolean types and not all)? Where the
 * leaf has a segment without them (SiftSrcStats.nokinds) the source could only raise the REINDEX error.
 */
static bool
types_need_kinds(uint32 types, bool astext)
{
	uint32		vals = types & (SIFT_SRC_JT_STRING | SIFT_SRC_JT_NUMBER | SIFT_SRC_JT_BOOLEAN);

	return ((types & SIFT_SRC_JT_NONE) != 0) != ((types & SIFT_SRC_JT_NULL) != 0) ||
		(astext && vals != 0 && vals != (SIFT_SRC_JT_STRING | SIFT_SRC_JT_NUMBER | SIFT_SRC_JT_BOOLEAN));
}

/*
 * A test of the JSON type of a leaf's path as a conjunct of the query (typeof_test): the source keeps the rows whose type is one of the types,
 * a stored value by the type of the leaf and the values it could not hold by the type the chunk kept for them (SAP_TYPE). Not as a branch of OR:
 * the reads of the leaf give the values of the other rows as NULL, which only a conjunct that drops those rows makes safe.
 */
static SiftAggPred *
compile_typeof(AggCtx *c, Node *n)
{
	Node	   *call;
	uint32		types;
	int			leaf;
	SiftAggPred *p;

	n = strip_relabel(n);
	if (!typeof_test(n, &call, &types) || func_of(call) != F_JSONB_TYPEOF || (leaf = typeof_leaf(c, linitial(func_args(call)))) < 0 ||
		!source_can_types(c) || (c->stats.nokinds[leaf] && types_need_kinds(types, leaf_of(c, leaf)->astext)))
		return NULL;
	p = new_pred(c, SAP_TYPE);
	p->leaf = leaf;
	p->level = leaf_level(c, leaf);
	c->used = bms_add_member(c->used, leaf);
	c->rec_bad = bms_add_member(c->rec_bad, leaf);
	p->op = types;
	p->desc = expr_text(c, n);
	return p;
}

/*
 * The wide leaf an expression is exactly the value of: an attribute or index expression of numeric, money or
 * interval that the index keeps as lanes. No expression reads their values (their SiftSrcLeaf.typid is InvalidOid), so only compile_wide finds them.
 */
static int
wide_leaf(AggCtx *c, Node *e)
{
	int			att = attr_of_expr(c, e);

	for (int i = 0; att > 0 && i < c->lay->nleaves - c->nvleaf; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == att && l->parent < 0 && l->kind == SCL_SCALAR && !l->lossy && OidIsValid(l->wtypid) && l->wtypid == exprType(e))
			return i;
	}
	return -1;
}

static bool
source_can_wide(AggCtx *c)
{
	return OidIsValid(c->indexoid) ? sift_colsrc_can_wide(get_rel_relam(c->indexoid), true) : sift_colsrc_can_wide(get_rel_relam(c->heapoid), false);
}

/* the operator, leaf and operand of `leaf op operand` (or the commuted form), `leaf IN / NOT IN (operands)` and `leaf IS [NOT] NULL` on a wide leaf; false for any other clause */
static bool
wide_test(AggCtx *c, Node *n, int *leaf, int *op, Node **operand, Oid *argtype)
{
	Oid			lt,
				rt;
	Node	   *l,
			   *r;

	*operand = NULL;
	*argtype = InvalidOid;
	if (IsA(n, NullTest) && !((NullTest *) n)->argisrow)
	{
		*op = ((NullTest *) n)->nulltesttype == IS_NOT_NULL ? SWT_NOTNULL : SWT_ISNULL;
		return (*leaf = wide_leaf(c, strip_relabel((Node *) ((NullTest *) n)->arg))) >= 0;
	}
	if (IsA(n, OpExpr) && list_length(((OpExpr *) n)->args) == 2 && (*op = builtin_cmp(((OpExpr *) n)->opno, &lt, &rt)) >= 0)
	{
		l = strip_relabel(linitial(((OpExpr *) n)->args));
		r = strip_relabel(lsecond(((OpExpr *) n)->args));
		if ((*leaf = wide_leaf(c, l)) >= 0 && pseudo_const(r))
			*operand = r;
		else if ((*leaf = wide_leaf(c, r)) >= 0 && pseudo_const(l))
		{
			*operand = l;
			*op = commute(*op);
		}
		else
			return false;
		*argtype = exprType(*operand);
		return *argtype == leaf_of(c, *leaf)->wtypid;
	}
	if (IsA(n, ScalarArrayOpExpr) && list_length(((ScalarArrayOpExpr *) n)->args) == 2)
	{
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) n;
		int			cmp = builtin_cmp(sa->opno, &lt, &rt);

		if (!((sa->useOr && cmp == SCMP_EQ) || (!sa->useOr && cmp == SCMP_NE)))
			return false;
		l = strip_relabel(linitial(sa->args));
		r = strip_relabel(lsecond(sa->args));
		*op = sa->useOr ? SWT_IN : SWT_NOTIN;
		*operand = r;
		*argtype = rt;
		return (*leaf = wide_leaf(c, l)) >= 0 && pseudo_const(r) && lt == leaf_of(c, *leaf)->wtypid && rt == lt;
	}
	return false;
}

/*
 * A test of a wide leaf as a conjunct of the query: the source keeps the rows that pass (SAP_WIDE) and the executor never reads the values, which
 * is why it is only a conjunct: an OR, a CASE, a FILTER or a key would have to read them.
 */
static SiftAggPred *
compile_wide(AggCtx *c, Node *n)
{
	int			leaf,
				op;
	Node	   *operand;
	Oid			argtype;
	SiftAggPred *p;

	n = strip_relabel(n);
	if (c->nwide >= SIFT_AGG_MAX_WIDE || !wide_test(c, n, &leaf, &op, &operand, &argtype) || !source_can_wide(c))
		return NULL;
	c->nwide++;
	p = new_pred(c, SAP_WIDE);
	p->leaf = leaf;
	p->level = 0;
	p->op = op;
	p->expr = operand;
	p->argtype = argtype;
	p->desc = expr_text(c, n);
	p->sel = clause_selectivity(c->root, n, c->relid, JOIN_INNER, NULL);
	return p;
}

/* the TID comparison a function is, SiftCmpOp, or -1 (<> keeps no range) */
static int
tid_cmp(Oid fn)
{
	switch (fn)
	{
		case F_TIDEQ:
			return SCMP_EQ;
		case F_TIDLT:
			return SCMP_LT;
		case F_TIDLE:
			return SCMP_LE;
		case F_TIDGT:
			return SCMP_GT;
		case F_TIDGE:
			return SCMP_GE;
		default:
			return -1;
	}
}

static bool
is_ctid(AggCtx *c, Node *n)
{
	Var		   *v = (Var *) n;

	return IsA(n, Var) && v->varno == c->relid && v->varlevelsup == 0 && v->varattno == SelfItemPointerAttributeNumber;
}

/*
 * `ctid op constant` as a conjunct of the query (op: = < <= > >=, either side): the scan reads the superblocks of the range of all the conjuncts and
 * tests the TIDs of the rows of its edges (SAP_TID, sift_aggspec_tid_range). Not as a branch of OR, an expression or a key; not for a partitioned
 * table, whose rows have a TID in each partition.
 */
static SiftAggPred *
compile_tid(AggCtx *c, Node *n)
{
	Node	   *l,
			   *r;
	int			op;
	SiftAggPred *p;

	n = strip_relabel(n);
	if (!IsA(n, OpExpr) || c->nparts > 0 || list_length(((OpExpr *) n)->args) != 2 || (op = tid_cmp(func_of(n))) < 0 ||
		!(OidIsValid(c->indexoid) ? sift_colsrc_can_tids(get_rel_relam(c->indexoid), true) : sift_colsrc_can_tids(get_rel_relam(c->heapoid), false)))
		return NULL;
	l = strip_relabel(linitial(((OpExpr *) n)->args));
	r = strip_relabel(lsecond(((OpExpr *) n)->args));
	if (is_ctid(c, r))
	{
		Node	   *t = l;

		l = r;
		r = t;
		op = commute(op);
	}
	if (!is_ctid(c, l) || !IsA(r, Const) || ((Const *) r)->constisnull)
		return NULL;
	p = new_pred(c, SAP_TID);
	p->op = op;
	p->expr = r;
	p->argtype = TIDOID;
	p->sel = 1.0;
	p->desc = expr_text(c, n);
	return p;
}

/*
 * The type of a path tested by an expression (a FILTER, a CASE, an OR, a key) while the query reads the values of the path: the leaf cannot hold
 * the strings among its numbers, so the scan would raise where PostgreSQL's guard skips them. Only a conjunct of the query (SAP_TYPE) guards a read.
 */
static bool
types_guard_reads(AggCtx *c)
{
	for (int j = 0; j < c->nvleaf; j++)
		if (c->vleaf[j].types && bms_is_member(c->vleaf[j].obj, c->read))
			return true;
	return false;
}

/* a SAP_TYPE conjunct whose leaf the query reads elsewhere says so: a row it keeps with a value the leaf cannot hold is then an error */
static void
typeof_reads(AggCtx *c, SiftAggSpec *s)
{
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_TYPE && bms_is_member(s->pred[i]->leaf, c->read))
			s->pred[i]->op |= SIFT_SRC_JT_READ;
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
	if (is_gate(n))
	{
		p = new_pred(c, SAP_GATE);
		p->expr = n;
		p->desc = psprintf("(%s)", expr_text(c, n));
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

	if (!get_op_hash_functions(sgc->eqop, &lhs, &rhs) && get_opcode(sgc->eqop) != F_CASH_EQ)
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
	else if (leaf < 0 && single_leaf(c, expr, &leaf, &ptype, &ptypmod, &pcoll) &&
			 (lut_expr_ok(c, leaf, expr) || (k->bucket = bucket_key_secs(c, leaf, expr)) > 0))
	{
		k->kind = SAK_LUT;
		k->leaf = leaf;
		k->expr = substitute_leaf(c, expr, ptype, ptypmod, pcoll);
		k->desc = psprintf("%s: %s%s", leaf_name(c, leaf), expr_text(c, expr), k->bucket > 0 ? bucket_text(k->bucket) : "");
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
 * The aggregates whose own transition and final functions run on the distinct values of a joined column: sums and averages of the
 * integer types and interval, and the average of float4 (computed in double: sum(float4) adds in float4, which a reordered
 * sum does not reproduce). Nothing in them depends on the order of the values but float4_accum's rounding, inside the comparator's
 * tolerance.
 */
static bool
mapped_pg_agg(Oid fid)
{
	switch (fid)
	{
		case F_SUM_INT2:
		case F_SUM_INT4:
		case F_SUM_INT8:
		case F_SUM_INTERVAL:
		case F_AVG_INT2:
		case F_AVG_INT4:
		case F_AVG_INT8:
		case F_AVG_FLOAT4:
		case F_AVG_INTERVAL:
			return true;
		default:
			return false;
	}
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
	bool		joined = false,
				direct;
	int			kind,
				ci,
				si;

	foreach_ptr(JoinCtx, jc, c->joins)
		joined |= bms_is_member(jc->rti, pull_varnos(c->root, arg));
	/* without a join the expression is evaluated for every tuple: its value is added as it comes, the distinct values of count(DISTINCT) get ids */
	direct = !joined && !(fid == F_COUNT_ANY && a->aggdistinct != NIL);
	if ((ai < 0 && !direct) || (!direct && (!OidIsValid(tc->eq_opr) || !get_op_hash_functions(tc->eq_opr, &lhs, &rhs))))
		return false;
	/* a direct bool_and, bool_or and every are PostgreSQL's own aggregates, not min and max of a boolean: they have a combine function and run in workers */
	if (direct && (fid == F_BOOL_AND || fid == F_BOOL_OR || fid == F_EVERY))
		return false;
	if (a->aggdistinct != NIL && fid != F_COUNT_ANY)
		return false;
	if (fid == F_COUNT_ANY)
		kind = a->aggdistinct != NIL ? SAA_M_DISTINCT : direct ? SAA_X_COUNT : SAA_M_COUNT;
	else if ((fid == F_SUM_NUMERIC || fid == F_AVG_NUMERIC) && typ == NUMERICOID)
		kind = fid == F_SUM_NUMERIC ? (direct ? SAA_X_SUM : SAA_M_SUM) : (direct ? SAA_X_AVG : SAA_M_AVG);
	else if ((fid == F_SUM_FLOAT8 || fid == F_AVG_FLOAT8) && typ == FLOAT8OID)
		kind = fid == F_SUM_FLOAT8 ? (direct ? SAA_X_SUM : SAA_M_SUM) : (direct ? SAA_X_AVG : SAA_M_AVG);
	else if (!direct && a->aggdistinct == NIL && mapped_pg_agg(fid))
		kind = SAA_M_PG;
	else if ((kind = minmax_kind(a, typ)) < 0)
		return false;
	else if (direct)
		kind = kind == SAA_M_MIN ? SAA_X_MIN : SAA_X_MAX;
	if (!combo_slot(c, normalize_expr(c, arg), !direct, &ci, &si))
		return false;
	slot_req(c, ci, si)->agg = direct ? -1 : ai;
	slot_req(c, ci, si)->direct = direct;
	out->kind = kind;
	out->aggfn = fid;
	out->nvx = 0;
	out->num.n = 0;
	out->argtype = typ;
	out->combo = ci;
	out->slot = si;
	out->desc = psprintf("%s(%s%s)", get_func_name(fid), a->aggdistinct ? "DISTINCT " : "", expr_text(c, arg));
	return true;
}

/* ---------- generic aggregates: PostgreSQL's own transition and final functions (SAA_GEN) ---------- */

#define AGG_GEN_ROW			12.0	/* a transition function call through fmgr, and the Datum of a leaf (the function's own procost comes on top) */
#define AGG_GEN_DISTINCT	10.0	/* the (group, value) pair of a DISTINCT aggregate: a hash probe */
#define AGG_GEN_SORTED		30.0	/* a row of an aggregate with ORDER BY: the tuplesort put, and its share of the sort and the read back */
#define AGG_GEN_FIRST		1.0		/* a row of a DISTINCT ON column: the compare of its sort keys with the best row of the group, the value read when it wins */
#define AGG_GEN_FIRST_KEY	1.2		/* ... and each sort key's compare */
#define AGG_GEN_FOLLOW		0.3		/* a row of the other DISTINCT ON columns: the answer of the first one */
#define AGG_GEN_HYP			200.0	/* a row of a hypothetical-set aggregate: sorted by the scan for its groups, then again by the aggregate (no gain over the normal plan) */
#define AGG_GEN_ORDERED		24.0	/* a value of an ordered-set aggregate: the tuplesort put */
#define AGG_GEN_GROUP		150.0	/* a group's final function and state, or the sort of an ordered-set group */

static void
record_deps(PlannerInfo *root, Node *n);

/* a leaf the executor can turn into a Datum of its type */
static bool
gen_leaf_ok(AggCtx *c, int id)
{
	SiftSrcLeaf *l = leaf_of(c, id);

	if (l->kind != SCL_SCALAR || l->lossy)
		return false;
	if (l->wide)
		return true;
	switch (l->typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case DATEOID:
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case TEXTOID:
		case VARCHAROID:
			return true;
		case CASHOID:
			return l->vtype == SCV_I64;
		case NUMERICOID:
			return l->vtype == SCV_I64;
		case TIMEOID:
			return l->vtype == SCV_I64;
		default:
			return l->vtype == SCV_I32 && type_is_enum(l->typid);
	}
}

typedef struct GenWalk
{
	AggCtx	   *c;
	int			nl;
	int			leaf[SIFT_AGG_MAX_GLEAVES];
	Oid			ptype[SIFT_AGG_MAX_GLEAVES];
	int32		ptypmod[SIFT_AGG_MAX_GLEAVES];
	Oid			pcoll[SIFT_AGG_MAX_GLEAVES];
	bool		bad;
} GenWalk;

/* the leaves an argument expression reads (each with one type), and whether it reads anything the executor cannot bind */
static bool
gen_walker(Node *n, GenWalk *w)
{
	int			id,
				p = 0;

	if (n == NULL || w->bad)
		return false;
	if (IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType))
	{
		if ((id = match_leaf(w->c, n)) >= 0)
		{
			while (p < w->nl && w->leaf[p] != id)
				p++;
			if (p == SIFT_AGG_MAX_GLEAVES || !gen_leaf_ok(w->c, id) ||
				(p < w->nl && (w->ptype[p] != exprType(n) || w->ptypmod[p] != exprTypmod(n) || w->pcoll[p] != exprCollation(n))))
				w->bad = true;
			else if (p == w->nl)
			{
				w->leaf[p] = id;
				w->ptype[p] = exprType(n);
				w->ptypmod[p] = exprTypmod(n);
				w->pcoll[p] = exprCollation(n);
				w->nl++;
			}
			return false;
		}
		w->bad |= IsA(n, Var);
	}
	if ((IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC) || IsA(n, SubPlan) || IsA(n, SubLink) || IsA(n, Aggref) ||
		IsA(n, WindowFunc) || IsA(n, GroupingFunc) || IsA(n, PlaceHolderVar) || IsA(n, CurrentOfExpr) || IsA(n, NextValueExpr) ||
		IsA(n, SetToDefault))
		w->bad = true;
	return !w->bad && expression_tree_walker(n, gen_walker, w);
}

static Node *
gen_sub_mutator(Node *n, GenWalk *w)
{
	int			id;

	if (n == NULL)
		return NULL;
	if ((IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType)) &&
		(id = match_leaf(w->c, n)) >= 0)
	{
		Param	   *p = makeNode(Param);
		int			pos = 0;

		while (w->leaf[pos] != id)
			pos++;
		p->paramkind = PARAM_EXEC;
		p->paramid = pos;
		p->paramtype = w->ptype[pos];
		p->paramtypmod = w->ptypmod[pos];
		p->paramcollid = w->pcoll[pos];
		p->location = -1;
		return (Node *) p;
	}
	return expression_tree_mutator(n, gen_sub_mutator, w);
}

/* does the expression read a column of a joined table */
static bool
arg_joined(AggCtx *c, Node *arg)
{
	foreach_ptr(JoinCtx, jc, c->joins)
		if (bms_is_member(jc->rti, pull_varnos(c->root, arg)))
			return true;
	return false;
}

/* an argument of a generic aggregate: a leaf, an expression of up to SIFT_AGG_MAX_GLEAVES leaves, a constant, or an expression of a joined table's columns */
static bool
compile_gen_arg(AggCtx *c, Node *arg, SiftAggGenArg *out, double *cost, bool *mapped, int ai)
{
	GenWalk		w = {c};
	int			leaf;
	QualCost	qc;

	arg = normalize_expr(c, arg);
	out->typid = exprType(arg);
	if (arg_joined(c, arg))
	{
		TypeCacheEntry *tc = lookup_type_cache(out->typid, TYPECACHE_EQ_OPR);
		RegProcedure lhs,
					rhs;

		/* the value of each joined row is the combo's: one table of values per aggregate */
		if (ai < 0 || *mapped || !OidIsValid(tc->eq_opr) || !get_op_hash_functions(tc->eq_opr, &lhs, &rhs) ||
			!combo_slot(c, arg, true, &out->combo, &out->slot))
			return false;
		out->mapped = *mapped = true;
		*cost = 4.0;
		return true;
	}
	if ((leaf = match_leaf(c, strip_relabel(arg))) >= 0)
	{
		if (!gen_leaf_ok(c, leaf))
			return false;
		out->nleaves = 1;
		out->leaf[0] = leaf;
		use_leaf(c, leaf);
		*cost = 2.0;
		return true;
	}
	if (contain_volatile_functions(arg))
		return false;
	gen_walker(arg, &w);
	if (w.bad || (w.nl == 0 && !pseudo_const(arg)))
		return false;
	out->nleaves = w.nl;
	for (int p = 0; p < w.nl; p++)
	{
		out->leaf[p] = w.leaf[p];
		use_leaf(c, w.leaf[p]);
	}
	out->expr = gen_sub_mutator(arg, &w);
	cost_qual_eval_node(&qc, arg, c->root);
	*cost = 2.0 + (w.nl ? qc.per_tuple / cpu_operator_cost : 0.0) + 2.0 * w.nl;
	return true;
}

/* the function's own cost in operator costs (its procost, or the planner support function's estimate) */
static double
fn_cost(AggCtx *c, Oid fn)
{
	QualCost	qc = {0, 0};

	add_function_cost(c->root, fn, NULL, &qc);
	return qc.per_tuple / cpu_operator_cost;
}

/*
 * Any aggregate with a transition function (and final function) that is not volatile runs on PostgreSQL's own functions: the
 * executor makes the aggregate's states as ExecAgg does (a minimal AggState for AggCheckCallContext and AggGetAggref). The
 * order of the input is the scan's (heap order of a fresh index), which is what the normal plan's sequential scan gives; an
 * ORDER BY in the call, a variadic aggregate, a hypothetical-set aggregate and an aggregate of another table's column keep the plan.
 */
/* a partial state per worker, merged by the combine function: the functions are parallel safe, the state is a plain type or comes with its serial and deserial functions */
static bool
gen_parallel_ok(Oid agg, Oid transfn, Oid combfn, Oid serfn, Oid deserfn, Oid transtype)
{
	if (!OidIsValid(combfn) || IsPolymorphicType(transtype) || func_volatile(combfn) == PROVOLATILE_VOLATILE)
		return false;
	if (transtype == INTERNALOID && (!OidIsValid(serfn) || !OidIsValid(deserfn)))
		return false;
	for (int i = 0; i < 5; i++)
	{
		Oid			fn = i == 0 ? agg : i == 1 ? transfn : i == 2 ? combfn : i == 3 ? serfn : deserfn;

		if (OidIsValid(fn) && func_parallel(fn) != PROPARALLEL_SAFE)
			return false;
	}
	return true;
}

static bool
compile_gen(AggCtx *c, Aggref *a, SiftAggAgg *out, int ai)
{
	HeapTuple	tup;
	Form_pg_aggregate ag;
	bool		hyp = a->aggkind == AGGKIND_HYPOTHETICAL,
				ordered = a->aggkind == AGGKIND_ORDERED_SET || hyp,
				hash = false,
				volatile_fn;
	Oid			transfn,
				combfn,
				serfn,
				deserfn,
				transtype;
	SiftAggGen *g;
	double		cost = 0;
	bool		mapped = false,
				wide = false;
	StringInfoData txt;

	if ((a->aggkind != AGGKIND_NORMAL && !ordered) || a->aggsplit != AGGSPLIT_SIMPLE || list_length(a->args) > SIFT_AGG_MAX_GARGS ||
		(ordered && !hyp && (list_length(a->aggorder) != 1 || list_length(a->args) != 1)) ||
		(hyp && list_length(a->aggorder) != list_length(a->args)) ||
		(a->aggdistinct != NIL && ordered))
		return false;
	foreach_ptr(Node, d, a->aggdirectargs)
		if (!pseudo_const(d))
			return false;
	if (a->aggdistinct != NIL && list_length(a->aggdistinct) != list_length(a->args))
		return false;
	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(a->aggfnoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for aggregate %u", a->aggfnoid);
	ag = (Form_pg_aggregate) GETSTRUCT(tup);
	volatile_fn = func_volatile(ag->aggtransfn) == PROVOLATILE_VOLATILE ||
		(OidIsValid(ag->aggfinalfn) && func_volatile(ag->aggfinalfn) == PROVOLATILE_VOLATILE);
	transfn = ag->aggtransfn;
	combfn = ag->aggcombinefn;
	serfn = ag->aggserialfn;
	deserfn = ag->aggdeserialfn;
	transtype = ag->aggtranstype;
	ReleaseSysCache(tup);
	if (volatile_fn)
		return false;
	g = palloc0(sizeof(SiftAggGen));
	initStringInfo(&txt);
	foreach_node(TargetEntry, te, a->args)
	{
		double		k;

		if (!compile_gen_arg(c, (Node *) te->expr, &g->arg[g->nargs], &k, &mapped, ai))
			return false;
		cost += k;
		if (!te->resjunk)
		{
			appendStringInfo(&txt, "%s%s", g->ntrans ? ", " : "", expr_text(c, (Node *) te->expr));
			g->ntrans++;
		}
		g->nargs++;
	}
	if (a->aggdistinct != NIL)
	{
		/* the distinct values of the one argument that varies (string_agg(DISTINCT x, ',')) are a hash set; any other DISTINCT is sorted */
		int			nvar = 0;

		for (int k = 0; k < g->nargs; k++)
			if (g->arg[k].nleaves > 0 || g->arg[k].mapped)
			{
				g->darg = k;
				nvar++;
			}
		hash = nvar == 1 && a->aggorder == NIL && !mapped;
	}
	if (hash || (ordered && !hyp))
	{
		Oid			t = g->arg[g->darg].typid;
		TypeCacheEntry *tc = lookup_type_cache(t, TYPECACHE_CMP_PROC_FINFO | TYPECACHE_EQ_OPR_FINFO);

		if (!OidIsValid(tc->cmp_proc) || !OidIsValid(tc->eq_opr))
			return false;
		if (ordered && !OidIsValid(linitial_node(SortGroupClause, a->aggorder)->sortop))
			return false;
	}
	for (int k = 0; k < g->nargs; k++)
		if (g->arg[k].mapped)
			slot_req(c, g->arg[k].combo, g->arg[k].slot)->agg = ai;
	g->aggref = (Node *) copyObject(a);
	((Aggref *) g->aggref)->aggfilter = NULL;
	g->direct = (Node *) a->aggdirectargs;
	g->distinct = a->aggdistinct != NIL;
	g->ordered = ordered;
	g->hyp = hyp;
	/* the representation of equal values (1.0, 1.00) that min and max keep follows the order of the rows, which the workers' do not */
	for (int k = 0; k < g->nargs; k++)
		for (int j = 0; j < g->arg[k].nleaves; j++)
			wide |= leaf_of(c, g->arg[k].leaf[j])->wide;
	g->parallel = !wide && !ordered && !mapped && !g->distinct && a->aggorder == NIL && gen_parallel_ok(a->aggfnoid, transfn, combfn, serfn, deserfn, transtype);
	g->sorted = !ordered && !hash && (a->aggorder != NIL || a->aggdistinct != NIL);
	g->cost = AGG_GEN_ROW + fn_cost(c, transfn) + cost + (g->distinct ? AGG_GEN_DISTINCT : 0) + (ordered ? AGG_GEN_ORDERED : 0) +
		(g->sorted ? AGG_GEN_SORTED : 0) + (hyp ? AGG_GEN_HYP : 0);
	out->kind = SAA_GEN;
	out->gen = g;
	out->restype = a->aggtype;
	out->collid = a->inputcollid;
	if (ordered)
	{
		StringInfoData d;

		initStringInfo(&d);
		foreach_ptr(Node, e, a->aggdirectargs)
			appendStringInfo(&d, "%s%s", foreach_current_index(e) ? ", " : "", expr_text(c, e));
		out->desc = psprintf("%s(%s) WITHIN GROUP (ORDER BY %s)", get_func_name(a->aggfnoid), d.data, txt.data);
	}
	else
	{
		StringInfoData ord;

		initStringInfo(&ord);
		foreach_node(SortGroupClause, sc, a->aggorder)
		{
			bool		rev;
			TargetEntry *te = get_sortgroupref_tle(sc->tleSortGroupRef, a->args);

			get_equality_op_for_ordering_op(sc->sortop, &rev);
			appendStringInfo(&ord, "%s%s%s%s", ord.len ? ", " : " ORDER BY ", expr_text(c, (Node *) te->expr), rev ? " DESC" : "",
							 sc->nulls_first == rev ? "" : sc->nulls_first ? " NULLS FIRST" : " NULLS LAST");
		}
		out->desc = psprintf("%s(%s%s%s)", get_func_name(a->aggfnoid), g->distinct ? "DISTINCT " : "", txt.data, ord.data);
	}
	record_plan_function_dependency(c->root, a->aggfnoid);
	foreach_node(TargetEntry, te, a->args)
		record_deps(c->root, (Node *) te->expr);
	return true;
}

static bool
compile_native_agg(AggCtx *c, Aggref *a, SiftAggAgg *out, int ai)
{
	int			leaf = -1;
	Node	   *arg = NULL;
	Oid			fid = a->aggfnoid;
	bool		ordered = a->aggkind == AGGKIND_ORDERED_SET;

	if ((a->aggkind != AGGKIND_NORMAL && !ordered) || (a->aggorder != NIL && !ordered))
		return false;
	if (a->aggdistinct != NIL && fid != F_COUNT_ANY && fid != F_SUM_NUMERIC && fid != F_AVG_NUMERIC)
		return false;
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
			if (leaf_of(c, leaf)->wide && fid != F_COUNT_ANY)
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

/*
 * The aggregate a: a kernel of the scan when there is one, else PostgreSQL's own functions on the decoded values (compile_gen).
 * What the kernel attempt marked as used is undone when it fails.
 */
static bool
compile_agg(AggCtx *c, Aggref *a, SiftAggAgg *out, int ai)
{
	SiftAggAgg	fresh;
	Bitmapset  *used;
	bool		other;

	if (a->agglevelsup != 0 || contain_volatile_functions((Node *) a))
		return false;
	out->leaf = -1;
	out->restype = a->aggtype;
	out->collid = a->inputcollid;
	if (a->aggfilter)
	{
		Node	   *filter = normalize_expr(c, (Node *) a->aggfilter);

		out->filter = arg_joined(c, filter) ? compile_combo_pred_at(c, filter, true) : compile_pred(c, filter);
		if (out->filter == NULL)
			return false;
	}
	fresh = *out;
	used = bms_copy(c->used);
	other = c->other_group;
	if (compile_native_agg(c, a, out, ai))
		return true;
	*out = fresh;
	c->used = used;
	c->other_group = other;
	return compile_gen(c, a, out, ai);
}

/* ---------- the query ---------- */

typedef struct LeftJoin
{
	Index		rti;			/* the right side, a table */
	List	   *quals;			/* its ON clause */
	SiftJoinKind kind;
} LeftJoin;

typedef struct FromScan
{
	List	   *rtis;			/* range table indexes, in FROM order */
	List	   *quals;
	List	   *scan_quals;		/* conditions inside the nullable scanned input, before preservation */
	List	   *lefts;			/* LeftJoin */
	bool		ok;
	bool		flip;			/* the scanned table is the right side of a LEFT JOIN (its other side is preserved) */
	bool		flippable;		/* a LEFT or FULL JOIN of two tables was met: the scan may be the other one */
	Bitmapset  *ojrels;			/* the outer joins that preserve the other side */
} FromScan;

/* the table of a join's right side: a table, or a pulled up sub-select over one (a FromExpr of one table with a WHERE) */
static RangeTblRef *
right_table(Node *n)
{
	if (IsA(n, FromExpr) && list_length(((FromExpr *) n)->fromlist) == 1)
		n = linitial(((FromExpr *) n)->fromlist);
	return IsA(n, RangeTblRef) ? (RangeTblRef *) n : NULL;
}

static void scan_from(Node *n, FromScan *f);

/*
 * A LEFT JOIN with the scanned table on its right (the left side is preserved: flip), or a FULL JOIN of two tables: the table that is
 * not scanned is the join, with all of its rows kept. The ON clause is the join's; whatever the WHERE says of the scanned table is not.
 */
static bool
scan_preserved(JoinExpr *j, FromScan *f)
{
	bool		full = j->jointype == JOIN_FULL;
	RangeTblRef *l = IsA(j->larg, RangeTblRef) ? (RangeTblRef *) j->larg : NULL,
			   *r = IsA(j->rarg, RangeTblRef) ? (RangeTblRef *) j->rarg : NULL;
	FromScan	right = {.ok = true};
	LeftJoin   *lj;
	Index		scanned,
				dim;

	if ((!full && j->jointype != JOIN_LEFT) || l == NULL || (full && r == NULL))
		return false;
	f->flippable = true;
	if (!full && !f->flip)
		return false;
	if (f->flip)
	{
		scan_from(j->rarg, &right);
		if (!right.ok || right.ojrels != NULL || right.rtis == NIL)
			return false;
	}
	scanned = f->flip ? linitial_int(right.rtis) : l->rtindex;
	dim = f->flip ? l->rtindex : r->rtindex;
	lj = palloc(sizeof(LeftJoin));
	lj->kind = full ? SJ_FULL : SJ_RIGHT;
	lj->rti = dim;
	lj->quals = j->quals == NULL ? NIL : IsA(j->quals, List) ? list_copy((List *) j->quals) : list_make1(j->quals);
	/* Filter the nullable input before matching; these conditions never test its preserved NULL rows. */
	f->scan_quals = list_concat(f->scan_quals, right.quals);
	f->lefts = list_concat(f->lefts, right.lefts);
	f->rtis = f->flip ? list_concat(f->rtis, right.rtis) : lappend_int(f->rtis, scanned);
	f->rtis = lappend_int(f->rtis, dim);
	f->lefts = lappend(f->lefts, lj);
	f->ojrels = bms_add_member(f->ojrels, j->rtindex);
	return true;
}

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
	else if (IsA(n, JoinExpr) && scan_preserved((JoinExpr *) n, f))
		return;
	else if (IsA(n, JoinExpr) && (((JoinExpr *) n)->jointype == JOIN_LEFT || ((JoinExpr *) n)->jointype == JOIN_SEMI ||
								  ((JoinExpr *) n)->jointype == JOIN_ANTI) && right_table(((JoinExpr *) n)->rarg) != NULL)
	{
		JoinExpr   *j = (JoinExpr *) n;
		LeftJoin   *lj = palloc(sizeof(LeftJoin));
		RangeTblRef *r = right_table(j->rarg);

		scan_from(j->larg, f);
		lj->kind = j->jointype == JOIN_LEFT ? SJ_LEFT : j->jointype == JOIN_SEMI ? SJ_SEMI : SJ_ANTI;
		lj->rti = r->rtindex;
		lj->quals = j->quals == NULL ? NIL : IsA(j->quals, List) ? list_copy((List *) j->quals) : list_make1(j->quals);
		/* the WHERE of a pulled up sub-select that is the right side filters the table's rows before the join: more of the ON clause */
		if (IsA(j->rarg, FromExpr) && ((FromExpr *) j->rarg)->quals)
			lj->quals = list_concat(lj->quals, list_copy((List *) ((FromExpr *) j->rarg)->quals));
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
		case F_JSONB_ARRAY_ELEMENTS_TEXT:
			return FK_ELEMENTS_TEXT;
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

/* the group leaf of the given kind the keys below an expression lead to: the jsonb array (object) leaf an unnested function reads */
static int
group_leaf_below(AggCtx *c, Node *arg, List *keys, SiftSrcLeafKind kind)
{
	JsonRef		r = {0};

	if (!json_access(c, arg, &r) || r.as_text || r.att == 0)
		return -1;
	r.path = list_concat_copy(r.path, keys);
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->attno == r.att && l->parent < 0 && paths_equal(l->path, r.path) && l->kind == kind)
			return i;
	}
	return -1;
}

static int
group_leaf_of(AggCtx *c, Node *arg, SiftSrcLeafKind kind)
{
	return group_leaf_below(c, arg, NIL, kind);
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
	c->oparams = saved->oparams;
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
	char		kind;			/* 's', 'n', 'b', 'z' (JSON null) */
	const char *s;
	int			len;
	Numeric		num;
	bool		b;
} Lit;

static int
child_named(AggCtx *c, int group, const char *name, bool astext)
{
	for (int i = 0; i < c->lay->nleaves; i++)
	{
		SiftSrcLeaf *l = leaf_of(c, i);

		if (l->parent == group && !l->lossy && l->astext == astext && l->kind == SCL_SCALAR && strcmp(l->name, name) == 0)
			return i;
	}
	return -1;
}

/* a string no number or boolean prints as: the text of an astext leaf equals it for a JSON string only */
static bool
lit_is_plain_text(const Lit *lit)
{
	return lit->kind == 's' && (lit->len == 0 || (!isdigit((unsigned char) lit->s[0]) && lit->s[0] != '-')) &&
		!(lit->len == 4 && memcmp(lit->s, "true", 4) == 0) && !(lit->len == 5 && memcmp(lit->s, "false", 5) == 0);
}

/* element leaf (JSON member `name`) op lit with the semantics of a jsonpath / containment comparison of JSON values, or NULL when not provable */
static SiftAggPred *
lit_cmp(AggCtx *c, int grp, const char *name, int op, const Lit *lit)
{
	int			leaf = child_named(c, grp, name, false);
	SiftAggPred *p;
	Const	   *k;
	Oid			typid;

	/* the text leaf of any scalar answers an equality with a string of its own kind: the comparison of another JSON type is unknown, not equal */
	if (leaf < 0 && op == SCMP_EQ && lit_is_plain_text(lit))
		leaf = child_named(c, grp, name, true);
	if (leaf < 0)
		return NULL;
	typid = leaf_of(c, leaf)->typid;
	if (lit->kind == 's' && typid == TEXTOID && (op == SCMP_EQ || op == SCMP_NE) && lut_leaf_ok(c, leaf))
	{
		/* bytewise equality: the C collation is deterministic whatever the database collation is */
		Param	   *pm = makeNode(Param);
		char	   *str = pnstrdup(lit->s, lit->len);
		Oid			eq = get_opfamily_member(TEXT_BTREE_FAM_OID, TEXTOID, TEXTOID, BTEqualStrategyNumber);
		OpExpr	   *e;

		pm->paramkind = PARAM_EXEC;
		pm->paramtype = TEXTOID;
		pm->paramtypmod = -1;
		pm->paramcollid = C_COLLATION_OID;
		pm->location = -1;
		k = makeConst(TEXTOID, -1, C_COLLATION_OID, -1, PointerGetDatum(cstring_to_text_with_len(lit->s, lit->len)), false, false);
		e = (OpExpr *) make_opclause(op == SCMP_NE ? get_negator(eq) : eq, BOOLOID, false, (Expr *) pm, (Expr *) k, InvalidOid, C_COLLATION_OID);
		set_opfuncid(e);
		p = make_leaf_pred(c, SAP_LUT, leaf);
		p->expr = (Node *) e;
		p->desc = psprintf("%s %s %s", leaf_name(c, leaf), cmp_text(op), quote_literal_cstr(str));
		return p;
	}
	/* jsonb numbers are numerics: only an integer leaf and an integral literal compare exactly (a float8 leaf holds the rounded value) */
	if (lit->kind == 'n' && (typid == INT2OID || typid == INT4OID || typid == INT8OID))
	{
		bool		err;
		int64		v = sift_numeric_int8(lit->num, &err);

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

static SiftAggPred *
or_of(AggCtx *c, SiftAggPred *a, SiftAggPred *b)
{
	SiftAggPred *p;

	if (a == NULL || b == NULL)
		return NULL;
	p = new_junction(c, SAP_OR, 2);
	p->child[0] = a;
	p->child[1] = b;
	junction_done(p);
	return p;
}

static bool
source_can_jnull(AggCtx *c)
{
	return OidIsValid(c->indexoid) ? sift_colsrc_can_jnull(get_rel_relam(c->indexoid), true) : sift_colsrc_can_jnull(get_rel_relam(c->heapoid), false);
}

/* the NULL slots of an element leaf by what they are (SAP_NULL ops); a JSON null and a missing key need the kinds of the source */
static SiftAggPred *
null_pred(AggCtx *c, int leaf, int op)
{
	static const char *const what[] = {"IS NULL", "IS NOT NULL", "IS JSON NULL", "IS MISSING"};
	SiftAggPred *p;

	/* a segment that did not keep the kinds cannot tell them: the normal plan, which reads the documents */
	if (op >= SNULL_JSON && (!source_can_jnull(c) || c->stats.nokinds[leaf]))
		return NULL;
	p = make_leaf_pred(c, SAP_NULL, leaf);
	p->op = op;
	p->desc = psprintf("%s %s", leaf_name(c, leaf), what[op]);
	return p;
}

static int
negate_op(int op)
{
	static const int neg[] = {SCMP_NE, SCMP_EQ, SCMP_GE, SCMP_GT, SCMP_LE, SCMP_LT};

	return neg[op];
}

/*
 * `@.name op lit` as a jsonpath filter has it, or with neg its negation `!(@.name op lit)`, on the element leaf of the member. The leaf
 * holds the members of its type and sets the others aside (overflow, an error for a scan that reads the leaf), so what is left is a value
 * of the type of the literal, a JSON null or a missing key (both NULL slots): a comparison is false for a missing key and for a JSON null
 * (true for != alone), so the negation is true for both (false for a JSON null under != ). Never unknown, which `!` could not negate.
 */
static SiftAggPred *
lit_test(AggCtx *c, int grp, const char *name, int op, const Lit *lit, bool neg)
{
	int			leaf = child_named(c, grp, name, false);

	if (leaf < 0)
		return !neg && op == SCMP_EQ ? lit_cmp(c, grp, name, op, lit) : NULL;
	if (lit->kind == 'z')
	{
		if (op != SCMP_EQ && op != SCMP_NE)
			return NULL;
		/* a JSON null equals only a JSON null; every other value, a missing key apart, differs from it */
		if (op == SCMP_EQ)
			return neg ? or_of(c, null_pred(c, leaf, 1), null_pred(c, leaf, SNULL_MISSING)) : null_pred(c, leaf, SNULL_JSON);
		return null_pred(c, leaf, neg ? 0 : 1);
	}
	if (!neg && op != SCMP_NE)
		return lit_cmp(c, grp, name, op, lit);
	if (!neg)
		return or_of(c, lit_cmp(c, grp, name, SCMP_NE, lit), null_pred(c, leaf, SNULL_JSON));
	if (op == SCMP_NE)
		return or_of(c, lit_cmp(c, grp, name, SCMP_EQ, lit), null_pred(c, leaf, SNULL_MISSING));
	return or_of(c, lit_cmp(c, grp, name, negate_op(op), lit), null_pred(c, leaf, 0));
}

/* ---- EXISTS (SELECT 1 FROM <unnest of the array> AS t(..) WHERE <element conjunction>) ---- */

typedef struct ExistsArg
{
	Query	   *q;
	RangeTblFunction *f;
	FuncKind	kind;
	List	   *params;			/* OuterParam: the columns of the outer row the sub-query's parameters stand for */
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
	c->oparams = x->params;
	return compile_conj(c, (List *) x->q->jointree->quals);
}

/* the parameters of the sub-plan whose outer expression is a leaf, and the leaf (compile_elem_row takes the ones of the row) */
static List *
outer_params(AggCtx *c, SubPlan *sp)
{
	List	   *out = NIL;
	ListCell   *lp,
			   *la;

	forboth(lp, sp->parParam, la, sp->args)
	{
		int			leaf = match_leaf(c, lfirst(la));
		OuterParam *o;

		if (leaf < 0)
			continue;
		o = palloc(sizeof(OuterParam));
		o->id = lfirst_int(lp);
		o->leaf = leaf;
		out = lappend(out, o);
	}
	return out;
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
	x.params = outer_params(c, sp);
	grp = group_leaf_of(c, outer_mutator(linitial(((FuncExpr *) x.f->funcexpr)->args), sp), func_leaf_kind(x.kind));
	return grp < 0 ? NULL : any_elem(c, grp, build_exists, &x);
}

/* a JSON string, number, boolean or null as a literal an element leaf is compared with */
static bool
jsonb_lit(const JsonbValue *v, Lit *lit)
{
	if (v->type == jbvString)
		*lit = (Lit) {'s', v->val.string.val, v->val.string.len};
	else if (v->type == jbvNumeric)
		*lit = (Lit) {'n', NULL, 0, v->val.numeric};
	else if (v->type == jbvBool)
		*lit = (Lit) {'b', NULL, 0, NULL, v->val.boolean};
	else if (v->type == jbvNull)
		*lit = (Lit) {'z'};
	else
		return false;
	return true;
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
		case jpiNotEqual:
			return SCMP_NE;
		default:
			return -1;
	}
}

/* a literal of the path, or a variable of it with its value in the constant vars of the call (a variable it lacks is an error of the heap's plan) */
static bool
path_lit(JsonPathItem *v, const Jsonb *vars, Lit *lit)
{
	JsonbValue	jv;
	int32		len;
	const char *name;

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
		case jpiNull:
			lit->kind = 'z';
			return true;
		case jpiVariable:
			name = jspGetString(v, &len);
			return vars != NULL && getKeyJsonValueFromContainer((JsonbContainer *) &vars->root, name, len, &jv) != NULL && jsonb_lit(&jv, lit);
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

typedef struct PathArg
{
	JsonPathItem *expr;
	const Jsonb *vars;			/* the constant variables of the call, NULL: none */
	bool		strict;			/* strict mode: a missing key is unknown, not false, which only a filter without ! cannot tell */
} PathArg;

/*
 * The filter expression as a predicate on the element leaves: comparisons of @.key with literals and variables, && || and ! (neg: under
 * an odd number of them, where the comparison is the negated one of lit_test and && and || swap). Every comparison of these is true or
 * false for a value the leaf holds (what it sets aside is an error of the scan), so ! is the complement and De Morgan holds.
 */
static SiftAggPred *
path_pred(AggCtx *c, int grp, JsonPathItem *v, const PathArg *a, bool neg)
{
	JsonPathItem l,
				r;
	SiftAggPred *p;
	int			op;
	char	   *key;
	Lit			lit;

	if (jspHasNext(v))
		return NULL;
	if (v->type == jpiNot)
	{
		jspGetArg(v, &l);
		return a->strict ? NULL : path_pred(c, grp, &l, a, !neg);
	}
	op = path_cmp_op(v->type);
	if (op < 0 && v->type != jpiAnd && v->type != jpiOr)
		return NULL;
	jspGetLeftArg(v, &l);
	jspGetRightArg(v, &r);
	if (op < 0)
	{
		p = new_junction(c, (v->type == jpiAnd) != neg ? SAP_AND : SAP_OR, 2);
		if ((p->child[0] = path_pred(c, grp, &l, a, neg)) == NULL || (p->child[1] = path_pred(c, grp, &r, a, neg)) == NULL)
			return NULL;
		junction_done(p);
		return p;
	}
	if ((key = path_key(&l)) != NULL && path_lit(&r, a->vars, &lit))
		;
	else if ((key = path_key(&r)) != NULL && path_lit(&l, a->vars, &lit))
		op = commute(op);
	else
		return NULL;
	return lit_test(c, grp, key, op, &lit, neg);
}

static SiftAggPred *
build_path(AggCtx *c, int grp, void *arg)
{
	PathArg    *a = arg;

	return path_pred(c, grp, a->expr, a, false);
}

/* the variables of jsonb_path_exists as an object constant (NULL with *ok: none; not ok: not a constant object) */
static const Jsonb *
path_vars(Node *vars, bool *ok)
{
	Jsonb	   *jb;

	vars = strip_relabel(vars);
	*ok = IsA(vars, Const) && !((Const *) vars)->constisnull;
	if (!*ok)
		return NULL;
	jb = DatumGetJsonbP(((Const *) vars)->constvalue);
	*ok = JB_ROOT_IS_OBJECT(jb);
	return *ok && JB_ROOT_COUNT(jb) > 0 ? jb : NULL;
}

/* `$[*] ? (filter)` on the array column, or `$.k.j[*] ? (filter)` on the array below keys of the object (lax only: a strict path raises for a missing key) */
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
	PathArg		arg = {&expr, NULL, false};
	List	   *keys = NIL;
	bool		ok = true;
	int			grp;

	if (func_of(n) == F_JSONB_PATH_EXISTS)
	{
		if (list_length(args) != 4)
			return NULL;
		arg.vars = path_vars(lthird(args), &ok);
	}
	if (!ok || !IsA(path, Const) || ((Const *) path)->constisnull)
		return NULL;
	jp = DatumGetJsonPathP(((Const *) path)->constvalue);
	jspInit(&root, jp);
	arg.strict = !(jp->header & JSONPATH_LAX);
	if (root.type != jpiRoot || !jspGetNext(&root, &any))
		return NULL;
	for (; any.type == jpiKey && !arg.strict; jspGetNext(&any, &any))
	{
		int32		len;
		const char *key = jspGetString(&any, &len);

		keys = lappend(keys, makeString(pnstrdup(key, len)));
		if (!jspHasNext(&any))
			return NULL;
	}
	if (any.type != jpiAnyArray || !jspGetNext(&any, &filt) || filt.type != jpiFilter || jspHasNext(&filt) ||
		(grp = group_leaf_below(c, linitial(args), keys, SCL_ARRAY)) < 0)
		return NULL;
	jspGetArg(&filt, &expr);
	return any_elem(c, grp, build_path, &arg);
}

/* ---- col @> '[{"k": v, ...}, ...]': every object of the right side is contained by some element ---- */

/* an element of an array of scalars (the unnamed child of the array leaf) equals the literal */
static SiftAggPred *
build_elem_lit(AggCtx *c, int grp, void *arg)
{
	return lit_test(c, grp, "", SCMP_EQ, arg, false);
}

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
		if (!jsonb_lit(&v, &lit))
			return NULL;
		if ((e = lit_test(c, grp, pnstrdup(k.val.string.val, k.val.string.len), SCMP_EQ, &lit, false)) == NULL)
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
		Lit			lit;

		(void) JsonbIteratorNext(&it, &v, true);
		if (v.type == jbvBinary && JsonContainerIsObject(v.val.binary.data))
			e = any_elem(c, grp, build_contains, v.val.binary.data);
		else if (jsonb_lit(&v, &lit) && lit.kind != 'z')	/* the JSON null of a scalar element: not proven */
			e = any_elem(c, grp, build_elem_lit, &lit);
		else
			return NULL;
		if (e == NULL)
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
 * x ? 'k', x ?| array['k', ..], x ?& array['k', ..] on an array of strings: an element is the string; for any (?|) or each (?&) of the
 * strings. The same test on an object (its keys) or a string is the other shapes of the value, which the index holds as an overflow.
 */
static SiftAggPred *
compile_has_keys(AggCtx *c, Oid fid, Node *target, Node *keys_arg)
{
	List	   *keys = NIL;
	SiftAggPred *p;
	int			grp,
				n;

	keys_arg = strip_relabel(keys_arg);
	if (fid == F_JSONB_EXISTS)
	{
		if (!IsA(keys_arg, Const) || ((Const *) keys_arg)->constisnull)
			return NULL;
		keys = list_make1(makeString(TextDatumGetCString(((Const *) keys_arg)->constvalue)));
	}
	else if (!path_keys(keys_arg, &keys) || keys == NIL)
		return NULL;
	if ((grp = group_leaf_of(c, target, SCL_ARRAY)) < 0)
		return NULL;
	n = list_length(keys);
	p = n > 1 ? new_junction(c, fid == F_JSONB_EXISTS_ALL ? SAP_AND : SAP_OR, n) : NULL;
	for (int i = 0; i < n; i++)
	{
		const char *key = strVal(list_nth(keys, i));
		Lit			lit = {'s', key, strlen(key)};
		SiftAggPred *e = any_elem(c, grp, build_elem_lit, &lit);

		if (e == NULL)
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
	if (fid == F_JSONB_EXISTS || fid == F_JSONB_EXISTS_ANY || fid == F_JSONB_EXISTS_ALL)
	{
		*target = linitial(func_args(n));
		return compile_has_keys(c, fid, *target, lsecond(func_args(n)));
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
#define AGG_COMBO_ROW		2.5		/* a tuple's entry in a combo: gathers and a hash probe */
#define AGG_JOIN_ROW		4.0		/* a row of a joined table, read when the scan starts */
#define AGG_IDX_ROW			12.0	/* a row of the joined table read through an index: the btree descent and the heap fetch */
#define AGG_SEMI_ROW		1.0		/* a tuple tested against a semi or anti join: one byte of the lookup table, or a hash probe */
#define AGG_ORDERED_ROW		6.0		/* a value kept for an ordered-set aggregate (histogram probe) */
#define AGG_NUM_STEP		0.5		/* an int128 add, subtract, multiply on a value */
#define AGG_NUM_DIV			8.0		/* round, trunc, floor, ceil: an int128 division */
#define AGG_VLEAF_ELEM		9.0		/* an element of an object looked up by key (a metric of a free key object), per row and key */
#define AGG_EVAL_SLOT		60.0	/* PostgreSQL evaluating an expression for a value combination it has not seen: the datums, a few operators, the id or the copy of the result */
#define AGG_WIDE_LEAF		3.0		/* a 64-bit leaf of a combo, per tuple: the hash probe for the id of its value */
#define AGG_DIRECT_NUM		2.0		/* the transition of a PostgreSQL-evaluated value (numeric_add): the normal plan pays it per row too, at the price PostgreSQL gives a transition function */
#define AGG_DIRECT_CMP		1.0		/* the type's comparison of one (min, max): the same in both plans */
#define AGG_REAL_CALL		5.0		/* what the executor really spends on a function or operator node (the fmgr call and its arguments), in operators */
#define AGG_REAL_NUMERIC	40.0	/* the same for a numeric function: the allocation and the digit arithmetic */
#define AGG_PLAIN_CREDIT	0.5		/* the share of the normal plan's under-priced evaluation the scan is credited with */
#define AGG_BYTE			0.1		/* a stored byte read and decompressed: about 0.3 ns, 3 GB/s */
#define AGG_UNIT_COST		200.0
#define AGG_PREAGG_GROUP	25.0	/* a group of a unit's histogram folded into the table: its key id and the aggregates (about 80 ns) */
#define AGG_PREAGG_EXACT	0.98	/* the share of exact units from which the metadata answers the query: no workers, they cost more than the answer */
#define AGG_START_COST		140000.0	/* opening the source, the lookup tables and the first units: about 0.4 ms, which a selective btree plan does not pay */
#define AGG_MERGE_GROUP		8.0		/* a group of a partial table of a worker: dumped, sent, merged */
#define AGG_ORDERED_MERGE	1.5		/* a value kept for an ordered-set aggregate, as the leader merges the histograms of the workers (the distinct pairs, at most one per value) */
#define AGG_SETUP_FACTOR	110.0	/* times parallel_setup_cost: starting the workers and opening the source in each takes about 8 ms, which in this path's estimates (a unit is worth about 0.1 us there, the unnest excess below included) is 110 times the default 1000 */
#define AGG_DEFAULT_ELEMS	3.0
#define AGG_RECHECK_FRAC	0.02	/* share of the rows the source reads again from the heap when a leaf the scan rechecks is flagged (the index does not count them) */
#define AGG_RECHECK_ROW		60.0	/* a row read again: the heap fetch by TID, the document walked again and the unit it makes */
#define AGG_WIDE_ROW		3.0		/* a row of a wide leaf: its lanes decoded, the hash probe for its entry in the unit's dictionary */
#define AGG_WIDE_ENTRY		12.0	/* a new entry of it (a numeric built, about 100 ns), per distinct value */
#define AGG_WIDE_TEST		1.0		/* a conjunct on a wide leaf of images, per dictionary entry: the engine makes its LUT once per segment (about 3 ns an entry) */
#define AGG_INLINE_WIDTH	200		/* average bytes of a text column from which its units store the strings themselves */
#define AGG_INLINE_ROW		56.0	/* a row of such a leaf read: the unit's strings are interned into a dictionary of its own */
#define AGG_INLINE_TEST		3.0		/* a row of it tested on the encoded strings (the prefixes decide most rows) */

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

/* the numeric operations of a program, and of the predicate tree (one more for the comparison of each): PostgreSQL's numeric functions in the normal plan */
static int
num_ops(const SiftAggNum *o)
{
	int			n = 0;

	for (int k = 0; k < o->n; k++)
		n += o->step[k].op > SNX_CONST;
	return n;
}

static int
pred_num_ops(const SiftAggPred *p)
{
	int			n = p == NULL ? 0 : p->num.n > 0 ? num_ops(&p->num) + 1 : 0;

	for (int i = 0; p && i < p->nchild; i++)
		n += pred_num_ops(p->child[i]);
	return n;
}

/* distinct value combinations of the leaves of a combo among `tuples`: what the statistics say of the columns, the tuples when a leaf is not one */
static double
combo_entries(AggCtx *c, const SiftAggCombo *cb, double tuples)
{
	List	   *exprs = NIL;

	for (int p = 0; p < cb->nleaves; p++)
	{
		SiftSrcLeaf *lf;

		if (cb->leaf[p] < 0)
			return tuples;
		lf = leaf_of(c, cb->leaf[p]);
		if (lf->parent >= 0 || lf->path != NIL || lf->attno > list_length(c->attexpr) || list_nth(c->attexpr, lf->attno - 1) == NULL)
			return tuples;
		exprs = lappend(exprs, list_nth(c->attexpr, lf->attno - 1));
	}
	return Min(tuples, estimate_num_groups(c->root, exprs, tuples, NULL, NULL));
}

/*
 * The evaluation PostgreSQL does for the combos with a 64-bit leaf or a direct slot: a hash probe per tuple for the id of each wide
 * leaf, and the expressions once per distinct combination of the leaves (every tuple when the column has as many values). Combos of
 * dictionary and small integer leaves have few combinations and keep the price they had.
 */
static double
combo_eval_cost(AggCtx *c, const SiftAggSpec *s, double rows, double agg_rows)
{
	double		cost = 0;

	for (int ci = 0; ci < s->ncombo; ci++)
	{
		const SiftAggCombo *cb = &s->combo[ci];
		int			wide = 0;
		bool		direct = false,
					filter = false;
		double		tuples;

		if (cb->join >= 0)
		{
			const SiftAggJoin *j = &s->join[cb->join];
			JoinCtx    *jc = list_nth(c->joins, cb->join);

			/* Without equality keys, every cached input tests every child row. */
			if (j->nkey == 0 && j->params == NIL)
			{
				double		candidates = jc->cpath ? jc->cpath->rows : c->root->simple_rel_array[jc->rti]->tuples;

				cost += combo_entries(c, cb, agg_rows) * Max(candidates, 1.0) * AGG_EVAL_SLOT;
			}
			continue;
		}
		for (int p = 0; p < cb->nleaves; p++)
			if (cb->leaf[p] >= 0)
				wide += leaf_of(c, cb->leaf[p])->vtype == SCV_I64 || leaf_of(c, cb->leaf[p])->vtype == SCV_F64;
		for (int sl = 0; sl < cb->nslots; sl++)
		{
			direct |= cb->slot[sl].direct;
			filter |= !cb->slot[sl].is_key && !cb->slot[sl].direct;
		}
		if (wide == 0 && !direct)
			continue;
		tuples = filter ? rows : agg_rows;
		cost += tuples * wide * AGG_WIDE_LEAF + combo_entries(c, cb, tuples) * cb->nslots * AGG_EVAL_SLOT;
	}
	return cost;
}

/* a tuple of a bucket key: the number of its step, the lookup of the id */
#define AGG_BUCKET_ROW		3.0

/* the buckets of a stretch of the leaf of a key, by the statistics: the tuples when there are none */
static double
bucket_count(AggCtx *c, const SiftAggKey *k, double tuples)
{
	int64		lo,
				hi;

	return leaf_stats_range(c, k->leaf, &lo, &hi) ? Min(tuples, (double) (hi - lo) / USECS_PER_SEC / k->bucket + 1) : tuples;
}

/* a lookup per tuple of a bucket key, and PostgreSQL evaluating the expression once per bucket */
static double
bucket_eval_cost(AggCtx *c, const SiftAggSpec *s, double rows)
{
	double		cost = 0;

	for (int i = 0; i < s->nkeys; i++)
		if (s->key[i].bucket > 0)
			cost += rows * AGG_BUCKET_ROW + bucket_count(c, &s->key[i], rows) * AGG_EVAL_SLOT;
	return cost;
}

/*
 * What the executor really spends on an expression, in operators (PostgreSQL prices a function at procost, 1 for almost all, while a
 * numeric one allocates and does digit arithmetic).
 */
static bool
real_cost_walker(Node *n, double *excess)
{
	List	   *args = NIL;

	if (n == NULL)
		return false;
	if (IsA(n, FuncExpr))
		args = ((FuncExpr *) n)->args;
	else if (IsA(n, OpExpr) || IsA(n, DistinctExpr))
		args = ((OpExpr *) n)->args;
	if (IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, DistinctExpr))
	{
		double		real = exprType(n) == NUMERICOID ? AGG_REAL_NUMERIC : AGG_REAL_CALL;

		foreach_ptr(Node, a, args)
			if (exprType(a) == NUMERICOID)
				real = AGG_REAL_NUMERIC;
		*excess += real;
	}
	else if (IsA(n, CoerceViaIO))
		*excess += AGG_REAL_NUMERIC;
	return expression_tree_walker(n, real_cost_walker, excess);
}

/*
 * The credit of the expressions and numeric aggregates: both plans evaluate the expressions of a query, the normal plan once per row at the price PostgreSQL gives
 * them, which is the executor's cost of a call for the cheap ones and an order of magnitude less than the real one for a numeric one;
 * the scan evaluates once per distinct combination of values and is priced with the real cost of its machinery (AGG_EVAL_SLOT). The
 * rows the normal plan evaluates the expressions for, times what its price leaves out, are what the scan saves: AGG_PLAIN_CREDIT of it.
 */
static double
plain_credit(AggCtx *c, const SiftAggSpec *s, double agg_rows)
{
	double		excess = 0;

	for (int ci = 0; ci < s->ncombo; ci++)
		if (s->combo[ci].join < 0)
			for (int sl = 0; sl < s->combo[ci].nslots; sl++)
			{
				Node	   *e = (Node *) s->combo[ci].slot[sl].expr;
				QualCost	priced;
				double		real = 0;

				real_cost_walker(e, &real);
				cost_qual_eval_node(&priced, e, c->root);
				excess += Max(real - priced.per_tuple / cpu_operator_cost, 0.0);
			}
	{
		int			ops = 0;

		for (int i = 0; i < s->npred; i++)
			ops += pred_num_ops(s->pred[i]);
		for (int i = 0; i < s->nkeys; i++)
			ops += num_ops(&s->key[i].num);
		for (int i = 0; i < s->naggs; i++)
			ops += num_ops(&s->agg[i].num) + pred_num_ops(s->agg[i].filter);
		excess += ops * (AGG_REAL_NUMERIC - 2.0);
	}
	/* the transition of a numeric sum, average or extreme: PostgreSQL prices it as one operator, the executor allocates and adds digits */
	for (int ai = 0; ai < s->naggs; ai++)
	{
		const SiftAggAgg *a = &s->agg[ai];

		if (a->restype == NUMERICOID && (a->kind == SAA_SUM_NUM || a->kind == SAA_AVG_NUM || a->kind == SAA_MIN || a->kind == SAA_MAX ||
										 a->kind == SAA_X_SUM || a->kind == SAA_X_AVG || a->kind == SAA_X_MIN || a->kind == SAA_X_MAX))
			excess += AGG_REAL_NUMERIC - 2.0;
	}
	return excess * agg_rows * AGG_PLAIN_CREDIT;
}

/* the per tuple cost of the arithmetic of a PostgreSQL-evaluated aggregate */
static double
direct_agg_cost(const SiftAggAgg *a)
{
	switch (a->kind)
	{
		case SAA_X_MIN:
		case SAA_X_MAX:
			return AGG_DIRECT_CMP;
		case SAA_X_SUM:
		case SAA_X_AVG:
			return a->restype == NUMERICOID ? AGG_DIRECT_NUM : 0;
		default:
			return 0;
	}
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
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_TID)
			kept = Min(kept, s->pred[i]->sel);
	return kept;
}

static bool
pred_uses_leaf(const SiftAggPred *p, int leaf)
{
	if (p == NULL)
		return false;
	if (p->kind != SAP_AND && p->kind != SAP_OR && (p->leaf == leaf || p->leaf2 == leaf))
		return true;
	for (int i = 0; i < p->nchild; i++)
		if (pred_uses_leaf(p->child[i], leaf))
			return true;
	return false;
}

/* is the leaf read by something besides the text tests that run on the encoded strings? */
static bool
leaf_read_whole(AggCtx *c, SiftAggSpec *s, int leaf)
{
	SiftSrcStrTest t;

	/* combos and joins read leaves in ways not followed here */
	if (s->ncombo > 0 || c->joins != NIL)
		return true;
	for (int i = 0; i < s->nkeys; i++)
		if (s->key[i].leaf == leaf && s->key[i].kind != SAK_NUM && s->key[i].kind != SAK_SET)
			return true;
	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].leaf == leaf || pred_uses_leaf(s->agg[i].filter, leaf))
			return true;
	for (int i = 0; i < s->npred; i++)
		if (pred_uses_leaf(s->pred[i], leaf) && !(s->pred[i]->leaf == leaf && sift_aggspec_str_test(s->pred[i], c->lay, c->lay->nleaves - c->nvleaf, &t)))
			return true;
	return false;
}

/* The average width of the heap column a plain text leaf is read from (0: unknown, or the leaf is a jsonb path or an expression) */
static int
leaf_heap_width(AggCtx *c, int leaf)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;

	return e && IsA(e, Var) ? get_attavgwidth(planner_rt_fetch(c->relid, c->root)->relid, ((Var *) e)->varattno) : 0;
}

/* the share of the rows of a leaf that PostgreSQL's statistics expect to hold a distinct value (1: no statistics) */
static double
leaf_distinct_frac(AggCtx *c, int leaf, double rows)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
	HeapTuple	tup;
	double		nd = 0;

	if (e == NULL || !IsA(e, Var))
		return 1.0;
	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid),
						  Int16GetDatum(((Var *) e)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tup))
		return 1.0;
	nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
	ReleaseSysCache(tup);
	return nd > 0 ? Min(nd / Max(rows, 1.0), 1.0) : nd < 0 ? Min(-nd, 1.0) : 1.0;
}

/*
 * Text leaves of long strings: a unit stores the strings inline, and reading the leaf as codes builds a dictionary of the unit
 * (measured 0.5 us a row, which the heap scan beats when few rows pass); a test on the encoded strings costs a few ns a row.
 */
static double
inline_text_cost(AggCtx *c, SiftAggSpec *s)
{
	double		cost = 0;

	for (int i = 0; i < s->nleaves; i++)
	{
		const SiftSrcLeaf *lf = &c->lay->leaf[s->leaves[i]];

		if (lf->vtype == SCV_CODE && lf->kind == SCL_SCALAR && lf->parent < 0 && leaf_heap_width(c, s->leaves[i]) >= AGG_INLINE_WIDTH)
			cost += leaf_read_whole(c, s, s->leaves[i]) ? AGG_INLINE_ROW : AGG_INLINE_TEST;
	}
	return cost;
}

/* The share of the units the pre-aggregates answer (a unit is exact when its pages are all-visible, its superblock plain); 0 when the spec cannot use them */
static double
preagg_share(AggCtx *c, SiftAggSpec *s)
{
	SiftAggPaShape shape;

	if (!sift_agg_preagg || !sift_aggspec_preagg(s, c->lay, c->lay->nleaves, &c->pa, &shape))
		return 0.0;
	return Min(Max(c->stats.exact_frac, 0.0), 1.0);
}

/* The bytes of a leaf's values in the executor's vectors: the weight of its share of the stored bytes */
static double
leaf_width(const SiftSrcLeaf *lf)
{
	return lf->vtype == SCV_I64 || lf->vtype == SCV_F64 ? 8.0 : 4.0;
}

/*
 * The stored bytes the scan reads: the leaves it reads take their share (by value width) of the compressed bytes of the source; a
 * source that does not know them (a table, a sample) is taken to compress its values by half.
 */
static double
scan_bytes(AggCtx *c, SiftAggSpec *s, double rows)
{
	double		all = 0,
				used = 0;

	for (int i = 0; i < c->lay->nleaves; i++)
		all += leaf_width(&c->lay->leaf[i]);
	for (int i = 0; i < s->nleaves; i++)
		used += leaf_width(leaf_of(c, s->leaves[i]));
	return c->stats.bytes > 0 ? c->stats.bytes * used / Max(all, 1.0) : rows * used * 0.5;
}

static bool
extern_param_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param) && ((Param *) node)->paramkind == PARAM_EXTERN)
		return true;
	return expression_tree_walker(node, extern_param_walker, context);
}

/* Does a qual of the table compare with a bound parameter? Its selectivity is a default guess of the planner, not the data's. */
static bool
rel_has_param_qual(RelOptInfo *rel)
{
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		if (extern_param_walker((Node *) ri->clause, NULL))
			return true;
	return false;
}

/* does a btree index of the joined table start with columns that the table's `column = value` conditions give (map_load picks the same) */
static bool
join_index_usable(RelOptInfo *rel, const SiftAggJoin *j)
{
	foreach_node(IndexOptInfo, ii, rel->indexlist)
	{
		if (ii->relam != BTREE_AM_OID || ii->indpred != NIL || ii->hypothetical || ii->indexkeys[0] <= 0)
			continue;
		for (int k = 0; k < j->nidx; k++)
			if (j->idxatt[k] == ii->indexkeys[0] && op_in_opfamily(j->idxop[k], ii->opfamily[0]) && ii->indexcollations[0] == j->idxcoll[k])
				return true;
	}
	return false;
}

/* the heap pages of the scanned table: those of the partitions of a partitioned table */
static double
rel_pages(AggCtx *c)
{
	return c->nparts > 0 ? c->part_pages : c->rel->pages;
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
				open_cost,
				gen_cost = 0,
				exact,
				agg_rows;
	Cost		startup,
				child_cost = 0;

	if (s->group >= 0)
		elems = elems_of(c, s->group);
	open_cost = c->search ? sift_search_open_cost(c->search, rows) : 0;
	scan_cost = c->search ? sift_search_cost(c->search, rows, units, &rows, &units) : 0;
	for (int i = 0; i < s->nleaves; i++)
		scan_cost += rows * (leaf_level(c, s->leaves[i]) ? elems_of(c, leaf_of(c, s->leaves[i])->parent) : 1.0) * AGG_SCAN_LEAF;
	scan_cost += rows * (1 + (s->group >= 0 ? elems : 0)) * AGG_FILTER_VALUE * Max(s->npred, 1);
	scan_cost += rows * c->elem_work * AGG_FILTER_VALUE;
	for (int i = 0; i < s->nvleaf; i++)
		scan_cost += sift_aggvleaf_member(&s->vleaf[i]) ? rows * elems_of(c, s->vleaf[i].obj) * AGG_VLEAF_ELEM : 0;
	scan_cost += rows * inline_text_cost(c, s);
	for (int i = 0; i < s->nleaves; i++)
		if (s->recheck[i] != SRK_NONE && c->stats.ovf[s->leaves[i]])
		{
			scan_cost += rows * AGG_RECHECK_FRAC * AGG_RECHECK_ROW;
			break;
		}
	for (int i = 0; i < s->nleaves; i++)
		if (leaf_of(c, s->leaves[i])->wide)
			scan_cost += rows * (AGG_WIDE_ROW + AGG_WIDE_ENTRY * leaf_distinct_frac(c, s->leaves[i], rows));
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_WIDE && leaf_of(c, s->pred[i]->leaf)->vtype == SCV_CODE)
			scan_cost += rows * leaf_distinct_frac(c, s->pred[i]->leaf, rows) * AGG_WIDE_TEST;
	scan_cost = (scan_cost + units * AGG_UNIT_COST) * Max(zone_kept(c, s), 1.0 / units);
	agg_rows = sel_rows * (s->group >= 0 ? elems : 1.0);
	/* the lookup joins filter before the groups: no more tuples reach them than the rows PostgreSQL expects of the join */
	if (c->joins != NIL)
		agg_rows = Min(agg_rows, Max(input_rel->rows, 1.0));
	/* the rows of a preserved side no tuple matches are tuples too */
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind))
			agg_rows += Max(c->root->simple_rel_array[jc->rti]->tuples, 1.0);
	/* a group takes a tuple: the groups PostgreSQL expects of the unnested rows it assumes (a cap of 100 a call) are more than the elements hold */
	rows_out = Min(rows_out, Max(Max(s->nsets, 1) * agg_rows, 1.0));
	scan_cost += agg_rows * (Max(s->nsets, 1) * (AGG_TUPLE + s->nkeys * AGG_KEY_ROW + s->naggs * AGG_AGG_ROW) + s->ncombo * AGG_COMBO_ROW);
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		RelOptInfo *jrel = c->root->simple_rel_array[jc->rti];
		double		tuples = Max(jrel->tuples, 1.0);

		/* Correlated children run once per distinct input, with their native path's cost. */
		if (jc->j.params != NIL)
		{
			const SiftAggCombo *cb = &s->combo[jc->combo];

			child_cost += combo_entries(c, cb, agg_rows) * (jc->cpath->total_cost + cpu_operator_cost * AGG_EVAL_SLOT);
			continue;
		}
		/* a plan is run whole; an index scan reads the rows that pass the equalities, the rest of a table is read whole */
		if (jc->cpath != NULL)
		{
			scan_cost += Max(jc->cpath->rows, 1.0) * AGG_JOIN_ROW;
			child_cost += jc->cpath->total_cost;
		}
		else if (jc->j.nidx > 0 && join_index_usable(jrel, &jc->j))
			scan_cost += Min(Max(jrel->rows, 1.0), tuples) * AGG_IDX_ROW;
		else
			scan_cost += tuples * AGG_JOIN_ROW;
		if (jc->kind == SJ_SEMI || jc->kind == SJ_ANTI)
			scan_cost += sel_rows * AGG_SEMI_ROW;
	}
	for (int i = 0; i < s->naggs; i++)
	{
		if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
			scan_cost += agg_rows * AGG_ORDERED_ROW;
		scan_cost += agg_rows * (num_cost(&s->agg[i].num) + direct_agg_cost(&s->agg[i])) + rows * pred_num_cost(s->agg[i].filter);
		if (s->agg[i].kind == SAA_GEN)
			gen_cost += agg_rows * s->agg[i].gen->cost + rows_out * AGG_GEN_GROUP;
	}
	scan_cost += combo_eval_cost(c, s, rows, agg_rows) + bucket_eval_cost(c, s, agg_rows);
	for (int i = 0; i < s->nkeys; i++)
		scan_cost += agg_rows * num_cost(&s->key[i].num);
	for (int i = 0; i < s->npred; i++)
		scan_cost += rows * pred_num_cost(s->pred[i]);
	scan_cost = Max(scan_cost - plain_credit(c, s, agg_rows), scan_cost * 0.1);
	scan_cost += scan_bytes(c, s, rows) * AGG_BYTE * Max(zone_kept(c, s), 1.0 / units);
	/* a top-N reads a fraction of the units, but opens the match set whole */
	if (s->topn)
		scan_cost = (scan_cost - open_cost) * c->topn_frac + open_cost;
	scan_cost *= cpu_operator_cost;
	/*
	 * The normal plan of an unnest expects the rows of the function (100 per call for jsonb_to_recordset, jsonb_array_elements,
	 * jsonb_each) minus its filters, and prices its sort, join and aggregate with them, though the data has a handful per row: when it
	 * expects more unnested rows than the selected orders hold elements, this path is charged the square root of the excess. The
	 * estimates of the two plans then cross where the times do: a selective store set keeps the btree plan, a whole table or a
	 * filter on the elements (which cuts the expected rows) keeps its price. A qual with a bound parameter has no such estimate (a generic
	 * plan guesses its selectivity; the values are known at execution): no charge.
	 */
	if (s->group >= 0 && !rel_has_param_qual(c->rel))
		scan_cost *= Max(sqrt(input_rel->rows / (sel_rows * elems)), 1.0);
	/* the calls of the generic aggregates are the elements the data has, whatever the normal plan expects of the unnest */
	scan_cost += gen_cost * cpu_operator_cost;
	/* rows on pages that are not all-visible are checked in the heap, a page at a time */
	scan_cost += vis * Min(sel_rows, Max(rel_pages(c), 1.0)) * random_page_cost * 0.25 * (s->topn ? c->topn_frac : 1.0);
	/* the exact units cost their groups, not their rows */
	exact = preagg_share(c, s);
	if (exact > 0)
		scan_cost += exact * (units * (AGG_UNIT_COST + Min(rows_out, 256.0) * AGG_PREAGG_GROUP) * cpu_operator_cost - scan_cost);
	startup = AGG_START_COST * cpu_operator_cost + child_cost + c->subplan_cost;
	if (s->nworkers > 0)
	{
		/* each process aggregates its share into a table of its own (one per lane for float sums); the leader merges them */
		double		tables = sift_aggspec_ordered(s) ? SIFT_AGG_LANES : s->nworkers + 1;

		scan_cost = scan_cost / parallel_divisor(s->nworkers) +
			tables * Min(rows_out, Max(agg_rows / tables, 1.0)) * AGG_MERGE_GROUP * cpu_operator_cost;
		for (int i = 0; i < s->naggs; i++)
			if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
				scan_cost += agg_rows * AGG_ORDERED_MERGE * cpu_operator_cost;
		startup += parallel_setup_cost * AGG_SETUP_FACTOR;
	}
	p->startup_cost = startup + scan_cost;
	p->total_cost = p->startup_cost + rows_out * cpu_tuple_cost;
	/* the rows of a top-N are fetched from the heap, one page each */
	if (s->topn)
		p->total_cost += rows_out * random_page_cost * 0.5;
	p->rows = rows_out;
}

/* ---------- the path ---------- */

static const CustomPathMethods agg_path_methods;

static bool
unsupported_clause(AggCtx *c, Node *n)
{
	return n != NULL && (contain_volatile_functions(n) || has_exec_param_walker(n, c));
}

/* a path whose node outputs the columns of its choice (the others need a projection above them) */
static bool
projects_columns(Path *p)
{
	return IsA(p, SubqueryScanPath) || p->pathtype == T_ValuesScan || p->pathtype == T_FunctionScan || p->pathtype == T_CteScan;
}

/* the cheapest path of the rows of a sub-select, VALUES list, function or UNION ALL of sub-selects */
static Path *
child_path(RelOptInfo *rel, bool correlated)
{
	Path	   *best = NULL;

	foreach_ptr(Path, p, rel->pathlist)
		if ((p->param_info == NULL || (correlated && IsA(p, SubqueryScanPath))) && (projects_columns(p) || IsA(p, AppendPath) || IsA(p, MergeAppendPath)) &&
			(best == NULL || p->total_cost < best->total_cost))
			best = p;
	return best;
}

/* the calls (function or operator) on one column of the enclosing query that never raise: the child takes the call's value instead of the column */
static bool
derivable_call(Node *n)
{
	Oid			fid = func_of(n);

	return list_length(func_args(n)) >= 1 && !(IsA(n, FuncExpr) && ((FuncExpr *) n)->funcretset) &&
		(fid == F_JSONB_EXISTS || fid == F_JSONB_OBJECT_FIELD_TEXT || fid == F_JSONB_TYPEOF);
}

typedef struct DeriveCtx
{
	int			depth;			/* the query levels below the child's own */
	List	   *calls;			/* the calls, as the enclosing query evaluates them */
	List	   *params;			/* the parameter that stands for each */
	PlannerInfo *root;
} DeriveCtx;

/* A call of an enclosing column with constants becomes a parameter whose value the enclosing query computes (once per distinct column value). */
static Node *
derive_mutator(Node *n, DeriveCtx *x)
{
	Node	   *a;

	if (n == NULL)
		return NULL;
	if (IsA(n, Query))
	{
		Query	   *q;

		x->depth++;
		q = query_tree_mutator((Query *) n, derive_mutator, x, 0);
		x->depth--;
		return (Node *) q;
	}
	if (derivable_call(n) && IsA(a = strip_relabel(linitial(func_args(n))), Var) && ((Var *) a)->varlevelsup == x->depth + 1)
	{
		Node	   *call = copyObject(n);
		Param	   *prm = makeNode(Param);
		int			at = -1;

		foreach_ptr(Node, other, func_args(call))
			if (foreach_current_index(other) > 0 && !IsA(strip_relabel(other), Const))
				return expression_tree_mutator(n, derive_mutator, x);
		((Var *) strip_relabel(linitial(func_args(call))))->varlevelsup = 0;
		for (int i = 0; i < list_length(x->calls); i++)
			if (equal(list_nth(x->calls, i), call))
				at = i;
		if (at < 0)
		{
			x->calls = lappend(x->calls, call);
			x->params = lappend_int(x->params, assign_special_exec_param(x->root));
			at = list_length(x->calls) - 1;
		}
		prm->paramkind = PARAM_EXEC;
		prm->paramid = list_nth_int(x->params, at);
		prm->paramtype = exprType(n);
		prm->paramtypmod = exprTypmod(n);
		prm->paramcollid = exprCollation(n);
		prm->location = -1;
		return (Node *) prm;
	}
	return expression_tree_mutator(n, derive_mutator, x);
}

/* does the child still read a column of the enclosing query that a derived call took (it would have to take the whole column)? */
static bool
derived_column_walker(Node *n, DeriveCtx *x)
{
	if (n == NULL)
		return false;
	if (IsA(n, Query))
	{
		bool		r;

		x->depth++;
		r = query_tree_walker((Query *) n, derived_column_walker, x, 0);
		x->depth--;
		return r;
	}
	if (IsA(n, Var) && ((Var *) n)->varlevelsup == x->depth + 1)
		foreach_ptr(Node, call, x->calls)
		{
			Var		   *v = (Var *) strip_relabel(linitial(func_args(call)));

			if (v->varno == ((Var *) n)->varno && v->varattno == ((Var *) n)->varattno)
				return true;
		}
	return expression_tree_walker(n, derived_column_walker, x);
}

/* The sub-select a correlated function stands for: its columns, with the function's arguments read from inside as outer references. */
static Query *
function_query(RangeTblEntry *e)
{
	Query	   *q = makeNode(Query);
	RangeTblEntry *fe = copyObject(e);
	RangeTblRef *ref = makeNode(RangeTblRef);
	List	   *names = NIL,
			   *vars = NIL;
	ListCell   *lv,
			   *ln;

	if (e->funcordinality || list_length(e->functions) != 1)
		return NULL;
	fe->lateral = false;
	IncrementVarSublevelsUp(linitial_node(RangeTblFunction, fe->functions)->funcexpr, 1, 0);
	q->commandType = CMD_SELECT;
	q->canSetTag = true;
	q->rtable = list_make1(fe);
	ref->rtindex = 1;
	q->jointree = makeFromExpr(list_make1(ref), NULL);
#if PG_VERSION_NUM >= 180000
	expandRTE(fe, 1, 0, VAR_RETURNING_DEFAULT, -1, false, &names, &vars);
#else
	expandRTE(fe, 1, 0, -1, false, &names, &vars);
#endif
	forboth(lv, vars, ln, names)
		q->targetList = lappend(q->targetList, makeTargetEntry(lfirst(lv), list_length(q->targetList) + 1, pstrdup(strVal(lfirst(ln))), false));
	return q;
}

/* The plan PostgreSQL made of a correlated sub-select, with the quals it pushed into it: its parameters are what it read of the query's rows. */
static bool
planned_child(RelOptInfo *rel, JoinCtx *jc)
{
	if ((jc->cpath = child_path(rel, true)) == NULL || !IsA(jc->cpath, SubqueryScanPath) || rel->subplan_params == NIL)
		return false;
	jc->subroot = rel->subroot;
	foreach_node(PlannerParamItem, item, rel->subplan_params)
	{
		jc->j.params = lappend_int(jc->j.params, item->paramId);
		jc->args = lappend(jc->args, copyObject(item->item));
	}
	return true;
}

/*
 * A sub-select or function that reads the query's other tables is planned in a query level of its own, with what it reads of them as
 * parameters: the scan gives it their values for each distinct input and runs the plan PostgreSQL makes of it (an Append of the arms of
 * a UNION ALL, an unnest of a joined table's array, a LIMIT lookup). A column it reads only through calls that never raise (key
 * presence) is replaced by the call's value, which the scan then needs, not the column. The scan runs the child for the distinct values of the
 * tuples that reach its join, which are those PostgreSQL's plan reaches it with only when no other join or unnest can come before it: a child that can
 * raise (raising_walker) keeps the normal plan unless it is the only one.
 */
static bool
correlated_child(AggCtx *c, RangeTblEntry *e, RelOptInfo *rel, JoinCtx *jc)
{
	DeriveCtx	x = {0, NIL, NIL, c->root};
	Query	   *q = e->rtekind == RTE_FUNCTION ? function_query(e) : copyObject(e->subquery),
			   *derived;
	List	   *saved = c->root->plan_params;
	RelOptInfo *final;
	SubqueryScanPath *p = makeNode(SubqueryScanPath);

	if (c->nsiblings > 1 && raising_walker(e->rtekind == RTE_FUNCTION ? (Node *) e->functions : (Node *) e->subquery, c))
		return false;
	if (rel->baserestrictinfo != NIL)
		return e->rtekind == RTE_SUBQUERY && planned_child(rel, jc);
	if (q == NULL)
		return false;
	derived = query_tree_mutator(copyObject(q), derive_mutator, &x, 0);
	if (x.calls != NIL && !query_tree_walker(derived, derived_column_walker, &x, 0))
		q = derived;
	else
		x.calls = NIL;
	/* the calls are plan parameters of this level while the child is planned, as PostgreSQL's own for a PlaceHolderVar are */
	c->root->plan_params = NIL;
	for (int i = 0; i < list_length(x.calls); i++)
	{
		PlannerParamItem *item = makeNode(PlannerParamItem);

		item->item = list_nth(x.calls, i);
		item->paramId = list_nth_int(x.params, i);
		c->root->plan_params = lappend(c->root->plan_params, item);
	}
	jc->subroot = sift_subquery_planner(c->root->glob, q, c->root, e->eref->aliasname);
	foreach_node(PlannerParamItem, item, c->root->plan_params)
	{
		jc->j.params = lappend_int(jc->j.params, item->paramId);
		jc->args = lappend(jc->args, copyObject(item->item));
	}
	c->root->plan_params = saved;
	if (jc->j.params == NIL)
		return false;
	final = fetch_upper_rel(jc->subroot, UPPERREL_FINAL, NULL);
	set_cheapest(final);
	p->path.pathtype = T_SubqueryScan;
	p->path.parent = rel;
	p->path.pathtarget = rel->reltarget;
	p->subpath = final->cheapest_total_path;
	p->path.rows = p->subpath->rows;
	p->path.startup_cost = p->subpath->startup_cost;
	p->path.total_cost = p->subpath->total_cost + cpu_tuple_cost * p->path.rows;
	jc->cpath = (Path *) p;
	return true;
}

/* the rows the scan keeps of a correlated child, those of its plan for each distinct input, are bounded as the rows of a plan read once are */
static bool
child_rows_fit(AggCtx *c, JoinCtx *jc)
{
	return jc->cpath->rows * Min(estimate_num_groups(c->root, jc->args, c->rel->tuples, NULL, NULL), c->rel->tuples) <= sift_agg_lookup_max_rows;
}

/* the plan of a correlated child, made once for the query: the other table of a flipped join reads the same child */
static bool
planned_correlated_child(AggCtx *c, RangeTblEntry *e, RelOptInfo *rel, JoinCtx *jc)
{
	JoinCtx    *hit;

	foreach_ptr(JoinCtx, done, c->planned)
		if (done->rti == jc->rti)
		{
			jc->cpath = done->cpath;
			jc->subroot = done->subroot;
			jc->j.params = done->j.params;
			jc->args = done->args;
			return done->cpath != NULL;
		}
	hit = palloc0(sizeof(JoinCtx));
	hit->rti = jc->rti;
	if (correlated_child(c, e, rel, jc) && child_rows_fit(c, jc))
		*hit = *jc;
	c->planned = lappend(c->planned, hit);
	return hit->cpath != NULL;
}

/* a parameter that a query above carries into the tree (PostgreSQL has replaced a column of it in the expressions of the query itself) */
static bool
ancestor_param_walker(Node *n, PlannerInfo *root)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
		for (PlannerInfo *up = root->parent_root; up != NULL; up = up->parent_root)
			foreach_node(PlannerParamItem, item, up->plan_params)
				if (item->paramId == ((Param *) n)->paramid)
					return true;
	return expression_tree_walker(n, ancestor_param_walker, root);
}

/* does the right side read a column (or aggregate, or CTE) of the query, or above it when skip is 1? */
static bool
reads_outer(AggCtx *c, RangeTblEntry *e, int skip)
{
	/* the expressions of a function or VALUES list are in the query itself, the body of a sub-select one level below it */
	OuterRef	oref = {c->root, e->rtekind == RTE_SUBQUERY ? 0 : -1, skip};
	Node	   *expr = e->rtekind == RTE_VALUES ? (Node *) e->values_lists : (Node *) e->functions;

	if (e->rtekind == RTE_SUBQUERY)
		return query_tree_walker(e->subquery, outer_ref_walker, &oref, QTW_EXAMINE_RTES_BEFORE);
	if (e->rtekind != RTE_VALUES && e->rtekind != RTE_FUNCTION)
		return false;
	return outer_ref_walker(expr, &oref) || (skip && ancestor_param_walker(expr, c->root));
}

/*
 * The right side of a join that is a plan: a sub-select PostgreSQL did not pull up (GROUP BY, LIMIT, DISTINCT, a set operation), a VALUES list
 * or a function that reads nothing of the query's other tables. Its rows are those of PostgreSQL's plan of it, read once when the scan starts
 * (SiftAggJoin.child). A correlated subquery keeps its native parameterized path and is rescanned per cached input tuple.
 */
static bool
add_plan_join(AggCtx *c, RangeTblEntry *e, Index rti, JoinCtx *jc, const char **what)
{
	RelOptInfo *rel = c->root->simple_rel_array[rti];

	switch (e->rtekind)
	{
		case RTE_CTE:
		{
			PlannerInfo *owner = c->root;

			for (int level = 0; level < e->ctelevelsup; level++)
				owner = owner->parent_root;
			*what = "cte";
			/* A statement-level CTE has no changing outer-row inputs, wherever it is referenced. */
			if (owner->parent_root != NULL || e->self_reference)
				return false;
			break;
		}
		case RTE_SUBQUERY:
			*what = "subquery";
			if (e->subquery->rowMarks != NIL || contain_volatile_functions((Node *) e->subquery))
				return false;
			break;
		case RTE_VALUES:
			*what = "values";
			if (contain_volatile_functions((Node *) e->values_lists))
				return false;
			break;
		default:
			*what = "function";
			if (contain_volatile_functions((Node *) e->functions))
				return false;
			break;
	}
	/*
	 * A column of a query above the aggregate's own reaches the child as a parameter of that query's plan, which changes per row of it; the
	 * scan replays its groups when rescanned (agg_rescan) and never sees it. Only the query's own tables are an input of the child.
	 */
	if (rel == NULL || rel->reloptkind != RELOPT_BASEREL || reads_outer(c, e, 1))
		return false;
	if (reads_outer(c, e, 0) || !bms_is_empty(rel->lateral_relids))
		return (e->rtekind == RTE_SUBQUERY || e->rtekind == RTE_FUNCTION) && planned_correlated_child(c, e, rel, jc);
	if ((jc->cpath = child_path(rel, false)) == NULL || jc->cpath->rows > sift_agg_lookup_max_rows)
		return false;
	jc->j.child = 0;
	foreach_ptr(JoinCtx, o, c->joins)
		jc->j.child += o->cpath != NULL && o->j.params == NIL;
	return true;
}

/* a table or plan in the FROM list besides the scanned one: it joins by equality of its columns to leaves */
static bool
add_join(AggCtx *c, FromScan *fs, Index rti)
{
	RangeTblEntry *e = rt_fetch(rti, c->root->parse->rtable);
	JoinCtx    *jc = palloc0(sizeof(JoinCtx));
	const char *what = NULL;

	jc->rti = rti;
	jc->j.child = -1;
	jc->j.fslot = -1;
	if (e->rtekind == RTE_RELATION)
	{
		if (e->relkind != RELKIND_RELATION)
			return false;
		jc->j.relid = e->relid;
	}
	else if (!add_plan_join(c, e, rti, jc, &what))
		return false;
	foreach_ptr(LeftJoin, lj, fs->lefts)
		if (lj->rti == rti)
		{
			jc->kind = lj->kind;
			jc->clauses = lj->quals;
		}
	jc->j.kind = jc->kind;
	jc->j.desc = psprintf("%s%s", what ? (e->rtekind == RTE_VALUES ? "values" : psprintf("%s (%s)", e->eref->aliasname, what)) : get_rel_name(e->relid),
						  jc->kind == SJ_LEFT ? " (left)" : jc->kind == SJ_SEMI ? " (semi)" : jc->kind == SJ_ANTI ? " (anti)" :
						  jc->kind == SJ_RIGHT ? " (preserved)" : jc->kind == SJ_FULL ? " (full)" : "");
	jc->combo = combo_index(c, new_combo(c, list_length(c->joins)));
	c->joins = lappend(c->joins, jc);
	return true;
}

/*
 * `column = value` among the table's own conditions (the value reads no column, is not volatile and takes no executor parameter): the rows of
 * the table that can match are those of an index scan on the column, when the table has a btree on it (the executor looks, map_load).
 * The conjunct stays in the filter; this only narrows what is read.
 */
static void
join_index_quals(AggCtx *c, JoinCtx *jc, List *conds)
{
	SiftAggJoin *j = &jc->j;

	if (jc->cpath != NULL || sift_aggjoin_preserves(jc->kind))
		return;
	foreach_ptr(Node, q, conds)
	{
		OpExpr	   *op;
		Node	   *l,
				   *r,
				   *val;
		Oid			opno;

		if (!IsA(q, OpExpr) || j->nidx == SIFT_AGG_MAX_JIDX || list_length(((OpExpr *) q)->args) != 2)
			continue;
		op = (OpExpr *) q;
		l = strip_relabel(linitial(op->args));
		r = strip_relabel(lsecond(op->args));
		opno = op->opno;
		if (join_of_var(c, l) == jc)
			val = r;
		else if (join_of_var(c, r) == jc)
		{
			val = l;
			l = r;
			opno = get_commutator(opno);
		}
		else
			continue;
		if (!OidIsValid(opno) || !op_mergejoinable(opno, exprType(l)) || unsupported_clause(c, val) || contain_var_clause(val))
			continue;
		j->idxatt[j->nidx] = ((Var *) l)->varattno;
		j->idxop[j->nidx] = opno;
		j->idxcoll[j->nidx] = op->inputcollid;
		j->idxval[j->nidx++] = val;
	}
}

/* the rest of the ON clause of a joined table: a boolean slot of its combo, over the leaves and the columns of a candidate row */
static bool
join_filter(AggCtx *c, JoinCtx *jc, List *conds)
{
	int			ci,
				si;

	join_index_quals(c, jc, conds);

	if (!combo_slot_in(c, jc, normalize_expr(c, (Node *) make_ands_explicit(conds)), false, false, &ci, &si) || ci != jc->combo)
		return false;
	jc->j.fslot = si;
	return true;
}

static bool
preserved_join(AggCtx *c)
{
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind))
			return true;
	return false;
}

/* Nulling join RTIs describe expression boundaries, not lookup inputs. */
static Relids
lookup_inputs(AggCtx *c, Node *q)
{
	Relids		vars = pull_varnos(c->root, q);

	for (int rti = -1; (rti = bms_next_member(vars, rti)) >= 0;)
		if (rt_fetch(rti, c->root->parse->rtable)->rtekind == RTE_JOIN)
			vars = bms_del_member(vars, rti);
	return vars;
}

/* A conjunct can probe this lookup when all its other inputs have already been produced. */
static bool
lookup_ready(AggCtx *c, JoinCtx *jc, List *quals, Relids done)
{
	Relids		available = bms_add_member(bms_copy(done), jc->rti);

	if (jc->j.params != NIL)
		return bms_is_subset(lookup_inputs(c, (Node *) jc->args), done);
	foreach_ptr(Node, q, quals)
	{
		Relids		vars = lookup_inputs(c, q);

		if (bms_is_member(jc->rti, vars) && bms_is_subset(vars, available))
			return true;
	}
	return false;
}

static bool
order_inner_lookups(AggCtx *c, List *pending, List *quals, List **ordered, Relids *done)
{
	while (pending != NIL)
	{
		JoinCtx    *next = NULL;

		foreach_ptr(JoinCtx, jc, pending)
			if (lookup_ready(c, jc, quals, *done))
			{
				next = jc;
				break;
			}
		if (next == NULL)
			return false;
		*ordered = lappend(*ordered, next);
		*done = bms_add_member(*done, next->rti);
		pending = list_delete_ptr(pending, next);
	}
	return true;
}

/* Inner lookups commute within a join boundary; outer/semi/anti joins keep their relative position. */
static bool
order_lookups(AggCtx *c, List *quals)
{
	List	   *pending = NIL,
			   *ordered = NIL,
			   *combos = NIL;
	Relids		done = bms_make_singleton(c->relid);
	int			child = 0;

	if (c->func_rti)
		done = bms_add_member(done, c->func_rti);
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		if (jc->kind == SJ_INNER)
		{
			pending = lappend(pending, jc);
			continue;
		}
		if (!order_inner_lookups(c, pending, quals, &ordered, &done))
			return false;
		pending = NIL;
		foreach_ptr(Node, q, jc->clauses)
			if (!bms_is_subset(lookup_inputs(c, q), bms_add_member(bms_copy(done), jc->rti)))
				return false;
		if (jc->j.params != NIL && !bms_is_subset(lookup_inputs(c, (Node *) jc->args), done))
			return false;
		ordered = lappend(ordered, jc);
		done = bms_add_member(done, jc->rti);
	}
	if (!order_inner_lookups(c, pending, quals, &ordered, &done))
		return false;
	Assert(list_length(c->combos) == list_length(c->joins));
	foreach_ptr(JoinCtx, jc, ordered)
	{
		ComboCtx   *cb = list_nth(c->combos, jc->combo);

		jc->combo = list_length(combos);
		cb->join = jc->combo;
		combos = lappend(combos, cb);
		if (jc->cpath && jc->j.params == NIL)
			jc->j.child = child++;
	}
	c->joins = ordered;
	c->combos = combos;
	return true;
}

/*
 * The conjuncts of the WHERE and of the ON clauses that read a joined table. Those of an inner join are its ON clause (the same rows
 * either way); those of an outer, semi or anti join filter its output rows (*post). In the ON clause of every join an equality of a
 * column with a leaf, or with an expression of leaves, is a key; the rest filters the table's rows for each tuple (join_filter).
 * *quals is left with the conjuncts that read no joined table.
 */
static bool
join_quals(AggCtx *c, List **quals, List **post, bool nullable_input)
{
	List	   *rest = NIL;

	foreach_ptr(Node, q, *quals)
	{
		Relids		v = pull_varnos(c->root, q);
		JoinCtx    *owner = NULL;

		foreach_ptr(JoinCtx, jc, c->joins)
			if (bms_is_member(jc->rti, v))
			{
				owner = jc;
			}
		if (owner == NULL)
			rest = lappend(rest, q);
		else if (owner->kind == SJ_INNER && (nullable_input || !preserved_join(c)))
			owner->clauses = lappend(owner->clauses, q);
		else
			*post = lappend(*post, q);
	}
	/* a WHERE the scanned table's NULL row can pass is evaluated after the outer join: it is not a filter of the rows the scan reads */
	if (rest != NIL && !nullable_input && preserved_join(c))
		return false;
	*quals = rest;
	return true;
}

static bool
setup_joins(AggCtx *c, List **quals, List **post, List **scan_quals, List **scan_post)
{
	List	   *probe_quals = preserved_join(c) ? list_copy(*scan_quals) : list_concat_copy(*quals, *scan_quals);

	foreach_ptr(JoinCtx, jc, c->joins)
		if (jc->kind == SJ_INNER)
			probe_quals = list_concat(probe_quals, list_copy(jc->clauses));
	if (!order_lookups(c, probe_quals))
		return false;
	if (!join_quals(c, quals, post, false) || !join_quals(c, scan_quals, scan_post, true))
		return false;
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		List	   *conds = NIL;
		ComboCtx   *cb = list_nth(c->combos, jc->combo);

		foreach_ptr(Node, arg, jc->args)
		{
			Node       *expr = normalize_expr(c, arg);
			ComboWalk   w = {c};

			combo_walker(expr, &w);
			if (w.bad || (w.jc && w.jc->combo >= jc->combo))
				return false;
			for (int p = 0; p < w.nl; p++)
			{
				if (combo_leaf(cb, w.leaf[p], true, w.ptype[p], w.ptypmod[p], w.pcoll[p]) < 0)
					return false;
				use_leaf(c, w.leaf[p]);
			}
			foreach_ptr(Var, v, w.vars)
				if (combo_reference(c, cb, join_of_var(c, (Node *) v), v) < 0)
					return false;
			jc->j.args = lappend(jc->j.args, expr);
		}
		foreach_ptr(Node, q, jc->clauses)
			if (jc->j.params != NIL || (!join_clause(c, jc, q) && !join_expr_clause(c, jc, q)))
				conds = lappend(conds, q);
		if ((jc->j.nkey == 0 && conds == NIL && jc->j.params == NIL) || (conds != NIL && !join_filter(c, jc, conds)))
			return false;
	}
	return true;
}

/* ---------- partitioned tables ---------- */

/* a column index of a partition: its layout in the terms of the partitioned table's columns, its statistics and pre-aggregates */
typedef struct PartCand
{
	Oid			indexoid;
	SiftSrcLayout *lay;
	SiftSrcStats stats;
	SiftSrcPreagg pa;
} PartCand;

typedef struct PartVars
{
	PlannerInfo *root;
	Index		rti;			/* the partition */
	Index		top;			/* the partitioned table */
	bool		ok;
} PartVars;

/* the attribute of the parent that has the partition's attribute att (0: none) */
static AttrNumber
parent_attno(AppendRelInfo *ai, AttrNumber att)
{
	foreach_ptr(Node, tv, ai->translated_vars)
		if (tv != NULL && IsA(tv, Var) && ((Var *) tv)->varattno == att)
			return foreach_current_index(tv) + 1;
	return 0;
}

/* A column of a partition as the column of the partitioned table, through every level (the attribute numbers differ after a dropped column) */
static Node *
part_vars_mutator(Node *n, PartVars *pv)
{
	Var		   *v;

	if (n == NULL)
		return NULL;
	if (!IsA(n, Var))
		return expression_tree_mutator(n, part_vars_mutator, pv);
	v = copyObject((Var *) n);
	for (Index r = pv->rti; r != pv->top && v->varattno > 0; r = pv->root->append_rel_array[r]->parent_relid)
		v->varattno = parent_attno(pv->root->append_rel_array[r], v->varattno);
	pv->ok &= v->varattno > 0;
	return (Node *) v;
}

/* The candidates of a leaf partition: its sift_col indexes, with the layouts in the terms of the partitioned table; none when it cannot be read */
static List *
part_candidates(AggCtx *c, RelOptInfo *leaf)
{
	RangeTblEntry *rte = planner_rt_fetch(leaf->relid, c->root);
	List	   *cands = NIL;

	if (rte->relkind != RELKIND_RELATION || get_rel_relam(rte->relid) != HEAP_TABLE_AM_OID || rte->securityQuals != NIL)
		return NIL;
	foreach_node(IndexOptInfo, ii, leaf->indexlist)
	{
		PartCand   *pc;
		PartVars	pv = {c->root, leaf->relid, c->relid, true};
		Relation	rel;
		List	   *exprs = NIL;

		if (!sift_colsrc_can_part(ii->relam) || ii->hypothetical || ii->indpred != NIL)
			continue;
		pc = palloc0(sizeof(PartCand));
		pc->indexoid = ii->indexoid;
		rel = index_open(ii->indexoid, AccessShareLock);
		pc->lay = sift_colsrc_layout(rel);
		sift_colsrc_stats(rel, &pc->stats);
		sift_colsrc_preagg(rel, &pc->pa);
		index_close(rel, AccessShareLock);
		foreach_ptr(Node, e, pc->lay->attexprs)
			exprs = lappend(exprs, part_vars_mutator(e, &pv));
		pc->lay->attexprs = exprs;
		if (pv.ok)
			cands = lappend(cands, pc);
	}
	return cands;
}

/* the partitions that the planner kept, the partitioned ones replaced by theirs: false when a partition is not one a column index reads */
static void
part_leaves(RelOptInfo *rel, List **leaves)
{
	for (int i = -1; (i = bms_next_member(rel->live_parts, i)) >= 0;)
	{
		RelOptInfo *p = rel->part_rels[i];

		if (IS_DUMMY_REL(p))
			continue;
		if (p->part_scheme != NULL)
			part_leaves(p, leaves);
		else
			*leaves = lappend(*leaves, p);
	}
}

/* the names of the columns the partitions are split by, at every level */
static void
part_key_names(AggCtx *c, RelOptInfo *rel, List **names)
{
	Oid			relid = planner_rt_fetch(rel->relid, c->root)->relid;

	for (int k = 0; k < rel->part_scheme->partnatts; k++)
		foreach_node(Var, v, pull_var_clause((Node *) rel->partexprs[k], 0))
			*names = list_append_unique(*names, makeString(get_attname(relid, v->varattno, false)));
	for (int i = -1; (i = bms_next_member(rel->live_parts, i)) >= 0;)
		if (rel->part_rels[i]->part_scheme != NULL)
			part_key_names(c, rel->part_rels[i], names);
}

/*
 * A parameter in a condition on a column the partitions are split by prunes them when the plan runs, which the scan does not do (it reads every
 * partition PostgreSQL's planning kept, and applies the condition to the rows): the normal plan keeps that pruning.
 */
static bool
part_runtime_pruning(AggCtx *c)
{
	List	   *names = NIL;

	part_key_names(c, c->rel, &names);
	foreach_node(RestrictInfo, ri, c->rel->baserestrictinfo)
		if (extern_param_walker((Node *) ri->clause, NULL))
			foreach_node(Var, v, pull_var_clause((Node *) ri->clause, 0))
				if (list_member(names, makeString(get_attname(c->heapoid, v->varattno, false))))
					return true;
	return false;
}

/*
 * The source of a partitioned table: the live partitions after PostgreSQL's pruning, each read through a sift_col index of the same layout, the
 * columns in the terms of the table (the query is compiled once, against them). A partition without such an index, of another storage, or whose
 * index differs from the others' keeps the normal plan, and so does a partitioned table whose partitions are pruned when the plan runs.
 */
static bool
part_source(AggCtx *c)
{
	List	   *leaves = NIL,
			   *first;
	int			n;

	if (c->rel->part_scheme == NULL || c->rel->nparts == 0)
		return false;
	part_leaves(c->rel, &leaves);
	n = list_length(leaves);
	if (n == 0 || n > SIFT_AGG_MAX_PARTS || part_runtime_pruning(c))
		return false;
	first = part_candidates(c, linitial_node(RelOptInfo, leaves));
	foreach_ptr(PartCand, f, first)
	{
		List	   *picked = list_make1(f);

		for (int i = 1; i < n && picked != NIL; i++)
		{
			PartCand   *m = NULL;

			foreach_ptr(PartCand, pc, part_candidates(c, list_nth_node(RelOptInfo, leaves, i)))
				if (sift_aggpart_same_layout(f->lay, pc->lay) && memcmp(&f->pa, &pc->pa, sizeof(f->pa)) == 0)
				{
					m = pc;
					break;
				}
			picked = m ? lappend(picked, m) : NIL;
		}
		if (picked == NIL)
			continue;
		c->nparts = n;
		c->partheap = palloc(n * sizeof(Oid));
		c->partindex = palloc(n * sizeof(Oid));
		memset(&c->stats, 0, sizeof(c->stats));
		for (int i = 0; i < n; i++)
		{
			RelOptInfo *leaf = list_nth_node(RelOptInfo, leaves, i);
			PartCand   *pc = list_nth(picked, i);

			c->partheap[i] = planner_rt_fetch(leaf->relid, c->root)->relid;
			c->partindex[i] = pc->indexoid;
			c->part_pages += leaf->pages;
			sift_aggpart_add_stats(&c->stats, &pc->stats);
		}
		c->lay = f->lay;
		c->pa = f->pa;
		c->indexoid = f->indexoid;
		return true;
	}
	return false;
}

/* Opens the source of the rel and its layout; false if none */
static bool
find_source(AggCtx *c, IndexOptInfo **ixout)
{
	RangeTblEntry *rte = planner_rt_fetch(c->relid, c->root);
	Relation	rel;

	*ixout = NULL;
	if (rte->relkind == RELKIND_PARTITIONED_TABLE)
		return part_source(c);
	if (sift_colsrc_is_table(get_rel_relam(rte->relid)))
	{
		rel = table_open(rte->relid, NoLock);
		c->lay = sift_colsrc_layout(rel);
		sift_colsrc_stats(rel, &c->stats);
		sift_colsrc_preagg(rel, &c->pa);
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
		sift_colsrc_preagg(rel, &c->pa);
		index_close(rel, AccessShareLock);
		c->indexoid = ii->indexoid;
		*ixout = ii;
		return true;
	}
	return false;
}

static List *
vars_of_rel(AggCtx *c, List *exprs)
{
	List	   *out = NIL;

	foreach_ptr(Node, e, exprs)
	{
		Node	   *x = e ? copyObject(e) : NULL;

		if (x)
			ChangeVarNodes(x, 1, c->relid, 0);
		out = lappend(out, x);
	}
	return out;
}

static void
build_attexprs(AggCtx *c)
{
	c->attexpr = vars_of_rel(c, c->lay->attexprs);
	c->attalias = vars_of_rel(c, c->lay->attalias);
}

static int
cmp_int(const void *a, const void *b)
{
	return *(const int *) a - *(const int *) b;
}

static SiftSrcRecheck spec_recheck(AggCtx *c, const SiftAggSpec *s, int m);
static bool recordset_recheck_ok(AggCtx *c);
static Bitmapset *recordset_columns(AggCtx *c);

static void
finish_leaves(AggCtx *c, SiftAggSpec *s)
{
	int			n = 0,
				m = -1;

	/* the columns of jsonb_to_recordset that nothing reads but a value they cannot store makes it raise: the source reads those rows again */
	if (recordset_recheck_ok(c))
	{
		Bitmapset  *cols = recordset_columns(c);

		while ((m = bms_next_member(cols, m)) >= 0)
			if (c->stats.ovf[m])
			{
				c->used = bms_add_member(c->used, m);
				c->rec_ok = bms_add_member(c->rec_ok, m);
			}
		m = -1;
	}
	s->leaves = palloc(Max(bms_num_members(c->used), 1) * sizeof(int));
	while ((m = bms_next_member(c->used, m)) >= 0)
		s->leaves[n++] = m;
	s->nleaves = n;
	qsort(s->leaves, n, sizeof(int), cmp_int);
	s->recheck = palloc0(Max(n, 1));
	for (int i = 0; i < n; i++)
		s->recheck[i] = s->leaves[i] < c->lay->nleaves ? spec_recheck(c, s, s->leaves[i]) : SRK_NONE;
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
	List	   *live_rtis = NIL,
			   *joined = NIL;
	int			nin = 0;

	scan_from((Node *) parse->jointree, fs);
	if (!fs->ok)
		return false;
	/* Join removal leaves the original FROM tree in place, but removes its base relations. */
	foreach_int(rti, fs->rtis)
		if (root->simple_rel_array[rti] != NULL)
			live_rtis = lappend_int(live_rtis, rti);
	fs->rtis = live_rtis;
	c->root = root;
	c->group = -1;
	c->nulling = fs->ojrels;
	foreach_int(rti, fs->rtis)
	{
		RangeTblEntry *e = rt_fetch(rti, parse->rtable);

		allrtis = bms_add_member(allrtis, rti);
		if (e->rtekind == RTE_RELATION)
		{
			bool		part = e->inh && e->relkind == RELKIND_PARTITIONED_TABLE;

			if ((e->inh && !part) || e->tablesample || e->securityQuals != NIL ||
				(e->relkind != RELKIND_RELATION && e->relkind != RELKIND_MATVIEW && !part))
				return false;
			if (c->relid == 0)
				c->relid = rti;
			else if (part || c->joinless)
				return false;
			else
				joined = lappend_int(joined, rti);
		}
		else if (e->rtekind == RTE_FUNCTION && e->lateral && c->func_rti == 0 && !e->funcordinality && list_length(e->functions) == 1 &&
				 func_kind_of(linitial_node(RangeTblFunction, e->functions)) != FK_NONE)
		{
			/* the scan unnests the elements that pass: a row without one is no row of an outer join (it keeps NULL columns) */
			foreach_ptr(LeftJoin, lj, fs->lefts)
				if (lj->rti == rti)
					return false;
			c->func_rti = rti;
			c->func = linitial_node(RangeTblFunction, e->functions);
			c->func_kind = func_kind_of(c->func);
		}
		else if (input_rel == NULL || c->joinless ||
				 (e->rtekind != RTE_SUBQUERY && e->rtekind != RTE_VALUES && e->rtekind != RTE_FUNCTION && e->rtekind != RTE_CTE))
			return false;
		else
			joined = lappend_int(joined, rti);
	}
	if (c->relid == 0)
		return false;
	/* a plan joined to the scan by an outer join keeps its own rows: the scanned table is never the nullable side */
	foreach_ptr(LeftJoin, lj, fs->lefts)
		if (lj->rti == c->relid)
			return false;
	c->rel = root->simple_rel_array[c->relid];
	c->heapoid = planner_rt_fetch(c->relid, root)->relid;
	/* the joins come after the source: planning the sub-selects of a query with no column index to scan would be a waste */
	if (c->rel == NULL || !find_source(c, &ii))
		return false;
	c->nsiblings = list_length(joined) + (c->func_rti != 0);
	foreach_int(rti, joined)
		if (!add_join(c, fs, rti))
			return false;
	if (input_rel != NULL)
	{
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
	/* the rows of a preserved side that nothing matched are known at the end of a single scan of one join */
	if (fs->ojrels && (input_rel == NULL || c->nparts > 0))
		return false;
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind) && jc != llast(c->joins))
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

/* agg_base, then the other table of an outer join of two tables as the scanned one when the first did not give a plan (a preserved side) */
static bool
agg_base_either(AggCtx *c, PlannerInfo *root, RelOptInfo *input_rel, FromScan *fs)
{
	List	   *planned;

	if (agg_base(c, root, input_rel, fs))
		return true;
	if (!fs->flippable)
		return false;
	planned = c->planned;
	memset(c, 0, sizeof(AggCtx));
	c->planned = planned;
	*fs = (FromScan) {.ok = true, .flip = true};
	return agg_base(c, root, input_rel, fs);
}

static SiftAggSpec *
agg_spec_new(AggCtx *c)
{
	SiftAggSpec *s = palloc0(sizeof(SiftAggSpec));

	s->indexoid = c->indexoid;
	s->heapoid = c->heapoid;
	s->nparts = c->nparts;
	s->partheap = c->partheap;
	s->partindex = c->partindex;
	s->group = c->group;
	return s;
}

/* the share of the heap pages the TID range of the conjuncts leaves, on the first of them (zone_kept) */
static void
tid_share(AggCtx *c, SiftAggSpec *s)
{
	uint64		lo,
				hi;

	if (!sift_aggspec_tid_range(s, &lo, &hi))
		return;
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_TID)
		{
			s->pred[i]->sel = lo >= hi ? 0.0 : Min(((double) ((hi - 1) >> 16) - (double) (lo >> 16) + 1.0) / Max(rel_pages(c), 1.0), 1.0);
			return;
		}
}

/* the WHERE of the scan: bitmap predicates, and the conjuncts over the joined rows */
static bool
agg_quals(AggCtx *c, SiftAggSpec *s, FromScan *fs)
{
	List	   *quals = fs->quals,
			   *joined = NIL,
			   *scan_quals = fs->scan_quals,
			   *scan_post = NIL;
	StringInfoData txt;

	if (!setup_joins(c, &quals, &joined, &scan_quals, &scan_post))
		return false;
	quals = list_concat(quals, scan_quals);
	s->pred = palloc(Max(list_length(quals), 1) * sizeof(SiftAggPred *));
	s->post = palloc(Max(list_length(quals) + list_length(joined) + list_length(scan_post), 1) * sizeof(SiftAggPred *));
	initStringInfo(&txt);
	foreach_ptr(Node, q, quals)
	{
		Node	   *e = normalize_expr(c, q);
		SiftAggPred *p = compile_typeof(c, e);

		if (p == NULL)
			p = compile_wide(c, e);
		if (p == NULL)
			p = compile_tid(c, e);
		if (p == NULL)
			p = compile_pred(c, e);
		if (p == NULL || unsupported_clause(c, q))
			return false;
		if (p->level > 1)
			s->post[s->npost++] = p;
		else
			s->pred[s->npred++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	foreach_ptr(Node, q, scan_post)
	{
		SiftAggPred *p = compile_combo_pred(c, normalize_expr(c, q));

		if (p == NULL || unsupported_clause(c, q))
			return false;
		s->post[s->npost++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	s->nprepost = preserved_join(c) ? s->npost : 0;
	foreach_ptr(Node, q, joined)
	{
		SiftAggPred *p = compile_combo_pred(c, normalize_expr(c, q));

		if (p == NULL || unsupported_clause(c, q))
			return false;
		s->post[s->npost++] = p;
		appendStringInfo(&txt, "%s%s", txt.len ? " AND " : "", p->desc);
	}
	s->desc_quals = txt.data;
	tid_share(c, s);
	return true;
}

static void
spec_deps(PlannerInfo *root, SiftAggSpec *s)
{
	if (s->search)
		record_deps(root, s->search);
	for (int i = 0; i < s->npred; i++)
		pred_deps(root, s->pred[i]);
	for (int i = 0; i < s->nkeys; i++)
		record_deps(root, s->key[i].expr);
	for (int i = 0; i < s->naggs; i++)
		pred_deps(root, s->agg[i].filter);
	for (int i = 0; i < s->ncombo; i++)
		for (int k = 0; k < s->combo[i].nslots; k++)
			record_deps(root, s->combo[i].slot[k].expr);
	for (int i = 0; i < s->njoin; i++)
		for (int k = 0; k < s->join[i].nkey; k++)
			record_deps(root, s->join[i].keyexpr[k]);
	for (int a = 0; a < s->narms; a++)
	{
		for (int i = 0; i < s->arm[a].nfilter; i++)
			pred_deps(root, s->arm[a].filter[i]);
		for (int i = 0; i < s->arm[a].ninner; i++)
			pred_deps(root, s->arm[a].inner[i].filter);
	}
}

/* the groups the scan makes: those of the keys, or of each grouping set */
/* what PostgreSQL has statistics of for a key: the column behind an expression that denotes a stored generated column, else the expression */
static Node *
stats_expr(AggCtx *c, Node *e)
{
	int			att = attr_of_expr(c, e);

	return att > 0 ? list_nth(c->attexpr, att - 1) : e;
}

static double
set_groups(AggCtx *c, PlannerInfo *root, SiftAggSpec *s, List *keys, double rows)
{
	double		n = 0;

	for (int set = 0; set < Max(s->nsets, 1); set++)
	{
		List	   *sub = NIL;
		double		bucketed = 1.0,
					g;

		for (int j = 0; j < list_length(keys); j++)
		{
			double		b = key_groups(c, &s->key[j]);

			if (s->nsets != 0 && !(s->setmask[set] >> j & 1))
				continue;
			if (b > 0)
				bucketed *= b;
			else
				sub = lappend(sub, stats_expr(c, list_nth(keys, j)));
		}
		/* Min and Max evaluate their arguments more than once: the estimate is made once */
		g = sub == NIL ? 1.0 : estimate_num_groups(root, sub, rows, NULL, NULL);
		n += Min(rows, Max(g, 1.0) * bucketed);
	}
	return n;
}

/* the position (1..) of column att in the output of a joined plan */
static int
child_pos(List *vars, int att)
{
	foreach_ptr(Var, v, vars)
		if (v->varattno == att)
			return foreach_current_index(v) + 1;
	elog(ERROR, "sift: column %d is not an output of the joined plan", att);
}

/* A path with the given number of workers: the spec travels with it, and the cheaper of the paths wins */
static void
add_agg_path(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, List *scan_tlist, int nworkers)
{
	SiftAggPath *ap = (SiftAggPath *) newNode(sizeof(SiftAggPath), T_CustomPath);
	CustomPath *cp = &ap->path;

	s->nworkers = nworkers;
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = target;
	cp->path.parallel_safe = false;
	cp->methods = &agg_path_methods;
	agg_cost(c, s, input_rel, &cp->path, s->est_groups);
	cp->custom_private = list_make2(sift_aggspec_pack(s), scan_tlist);
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		ap->correlated = lappend(ap->correlated, jc->j.params != NIL ? ((SubqueryScanPath *) jc->cpath)->subpath : NULL);
		ap->subroots = lappend(ap->subroots, jc->subroot);
		if (jc->cpath != NULL && jc->j.params == NIL)
			cp->custom_paths = lappend(cp->custom_paths, jc->cpath);
	}
	add_path(output_rel, &cp->path);
}

/*
 * A column of a sub-select that only a condition pushed down into it read is a NULL in its plan (remove_unused_subquery_outputs), so the
 * spec, which reads the condition again, must not read it: the normal plan stays. The plan of a sub-select, VALUES list or function keeps
 * the columns of the conditions it evaluates; an Append or MergeAppend outputs the relation's target only.
 */
static bool
child_cols_kept(AggCtx *c, JoinCtx *jc)
{
	RelOptInfo *rel = find_base_rel(c->root, jc->rti);
	Bitmapset  *used = NULL;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &used);
	if (projects_columns(jc->cpath))
		foreach_ptr(RestrictInfo, ri, list_concat_copy(rel->baserestrictinfo, rel->joininfo))
			pull_varattnos((Node *) ri->clause, rel->relid, &used);
	foreach_ptr(Var, v, jc->cvars)
		if (!bms_is_member(v->varattno - FirstLowInvalidHeapAttributeNumber, used))
			return false;
	return true;
}

/*
 * The plans the joins read, as paths that output the columns the spec reads (the columns of the joined rows are positions of that output): the
 * path PostgreSQL chose for the rows of the sub-select, VALUES list or function, projected on them. The scan nodes project any column.
 */
static bool
child_paths(AggCtx *c, SiftAggSpec *s)
{
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		SiftAggJoin *j = &s->join[foreach_current_index(jc)];
		PathTarget *t = create_empty_pathtarget();
		Path	   *p;
		Size		size;

		if (jc->cpath == NULL)
			continue;
		if (!child_cols_kept(c, jc))
			return false;
		if (jc->j.params != NIL)
			continue;
		/* the child's own columns, before any outer join nulls them: the plan below the projection outputs them so */
		foreach_ptr(Var, v, jc->cvars)
		{
			Var		   *own = copyObject(v);

			own->varnullingrels = NULL;
			add_column_to_pathtarget(t, (Expr *) own, 0);
		}
		set_pathtarget_cost_width(c->root, t);
		for (int k = 0; k < j->nkey; k++)
			j->keyatt[k] = child_pos(jc->cvars, j->keyatt[k]);
		for (int k = 0; k < j->ncol; k++)
			j->colatt[k] = child_pos(jc->cvars, j->colatt[k]);
		if (!projects_columns(jc->cpath))
		{
			jc->cpath = (Path *) create_projection_path(c->root, c->root->simple_rel_array[jc->rti], jc->cpath, t);
			continue;
		}
		size = IsA(jc->cpath, SubqueryScanPath) ? sizeof(SubqueryScanPath) : sizeof(Path);
		p = memcpy(palloc(size), jc->cpath, size);
		p->pathtarget = t;
		jc->cpath = p;
	}
	return true;
}

/*
 * Workers for the scan, as PostgreSQL decides them for a parallel seq scan of the table (size, reloption, the GUCs), when the whole
 * query is safe to run in a worker (the workers evaluate the spec's expressions) and the spec's tables can be merged
 * (sift_aggspec_parallel_ok).
 */
static int
parallel_workers(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	/* the relations' own flags, as PostgreSQL's grouping does: the query-wide hazard counts the sub-selects it pulled up into semi joins as restricted */
	if (!sift_aggspec_parallel_ok(s) || c->nsubplan > 0 || !c->root->glob->parallelModeOK || !output_rel->consider_parallel ||
		!input_rel->consider_parallel || !c->rel->consider_parallel || max_parallel_workers_per_gather <= 0)
		return 0;
	/* of the equal keys 1.0 and 1.00 the group shows the one of the first row, which the workers' tables merge in no order */
	for (int i = 0; s->nkeys > 0 && i < s->nleaves; i++)
		if (leaf_of(c, s->leaves[i])->wide)
			return 0;
	return compute_parallel_worker(c->rel, rel_pages(c), -1, max_parallel_workers_per_gather);
}

/* can SQL read a leaf of this type from the text of a scalar through its input function (the leaf types of the shredder)? */
static bool
text_io_leaf(const SiftSrcLeaf *l)
{
	if (l->wide || l->kind != SCL_SCALAR)
		return false;
	switch (l->typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case TEXTOID:
			return true;
		default:
			return false;
	}
}

/*
 * Does jsonb_to_recordset read the unnested list alone: nothing else reads it or one of its columns, and each column the query reads is read as
 * a column of the record (and has a type SQL reads from the text of a scalar)? Then a row whose document holds a value the leaves cannot store is
 * read again by the source as the function reads it.
 */
static bool
recordset_recheck_ok(AggCtx *c)
{
	int			m = -1;

	if (c->func_kind != FK_RECORDSET || c->group < 0 || c->search || bms_is_member(c->group, c->rec_bad) ||
		!sift_colsrc_can_recheck(get_rel_relam(c->indexoid), true))
		return false;
	for (int j = 0; j < c->nvleaf; j++)
		if (c->vleaf[j].obj == c->group)	/* the length of the list: a row that raises has no length */
			return false;
	while ((m = bms_next_member(c->used, m)) >= 0)
		if (leaf_of(c, m)->parent == c->group && (!bms_is_member(m, c->rec_ok) || bms_is_member(m, c->rec_bad) || !text_io_leaf(leaf_of(c, m))))
			return false;
	return true;
}

/*
 * The columns the function declares that have a leaf in the index: it reads all of them for every element, whether the query does or not, and
 * raises for a value its type does not take.
 */
static Bitmapset *
recordset_columns(AggCtx *c)
{
	Bitmapset  *cols = NULL;
	RangeTblFunction *f = c->func;

	for (int i = 0; i < list_length(f->funccolnames); i++)
	{
		int			leaf = child_leaf(c, c->group, strVal(list_nth(f->funccolnames, i)), list_nth_oid(f->funccoltypes, i));

		if (leaf >= 0 && text_io_leaf(leaf_of(c, leaf)))
			cols = bms_add_member(cols, leaf);
	}
	return cols;
}

/* How the query reads leaf m when the source holds a value it cannot store (SRK_NONE: it does not, such a value is an error for the scan) */
static SiftSrcRecheck
leaf_recheck(AggCtx *c, int m)
{
	if (!recordset_recheck_ok(c))
		return SRK_NONE;
	if (m == c->group)
		return SRK_RECORDSET;
	return leaf_of(c, m)->parent == c->group && (bms_is_member(m, c->used) || bms_is_member(m, recordset_columns(c))) ? SRK_TEXT : SRK_NONE;
}

/* does a conjunct of the rows read one of the leaves? Those rows would be dropped before the error of the list they hold is raised */
static bool
pred_reads_any(const SiftAggPred *p, const Bitmapset *leaves)
{
	if (p->leaf >= 0 && bms_is_member(p->leaf, leaves))
		return true;
	if (p->leaf2 >= 0 && bms_is_member(p->leaf2, leaves))
		return true;
	for (int i = 0; i < p->nchild; i++)
		if (pred_reads_any(p->child[i], leaves))
			return true;
	return false;
}

/* SRK_NONE unless leaf m is read in a way the source can read again, and no conjunct of the rows reads the unnested list or its columns or is applied after the others (it may drop a row that the error would reach first) */
static SiftSrcRecheck
spec_recheck(AggCtx *c, const SiftAggSpec *s, int m)
{
	Bitmapset  *list = NULL;

	if (leaf_recheck(c, m) == SRK_NONE)
		return SRK_NONE;
	list = bms_add_member(list, c->group);
	for (int i = 0; i < c->lay->nleaves; i++)
		if (leaf_of(c, i)->parent == c->group)
			list = bms_add_member(list, i);
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->level == 0 && (s->pred[i]->late || pred_reads_any(s->pred[i], list)))
			return SRK_NONE;
	return leaf_recheck(c, m);
}

/*
 * Does the spec read a leaf that holds values the index cannot store? The scan raises an error when a row of its snapshot has one
 * (check_overflow), the normal plan reads them from the heap: the query keeps its plan. Unless the source reads the rows again as SQL reads
 * them (spec_recheck). A numeric leaf is not one: its only such value is NaN, which the scan evaluates (SiftSrcVec.ovf). Nor is the leaf of a
 * jsonb_typeof conjunct (SAP_TYPE): the source keeps the rows of the types and tells the type of each value it cannot store.
 */
static bool
reads_overflow(AggCtx *c, const SiftAggSpec *s)
{
	Bitmapset  *typed = NULL;
	int			m = -1;

	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_TYPE)
			typed = bms_add_member(typed, s->pred[i]->leaf);
	while ((m = bms_next_member(c->used, m)) >= 0)
		if (c->stats.ovf[m] && !bms_is_member(m, typed) && spec_recheck(c, s, m) == SRK_NONE)
		{
			ereport(DEBUG1, (errmsg("sift: the aggregate pushdown does not read \"%s\": some unit holds values the leaf cannot store", leaf_name(c, m))));
			return true;
		}
	return false;
}

/* the leaves the spec reads and the combos; false when the spec cannot be run. keys: the group expressions of root's query */
static bool
spec_finish(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, List *keys)
{
	if (c->other_group || types_guard_reads(c))
		return false;
	share_ordered(s);
	if (c->group < 0)
		for (int i = 0; i < c->lay->nleaves; i++)
			if (bms_is_member(i, c->used) && leaf_of(c, i)->parent >= 0)
				return false;
	if (c->group >= 0 && !bms_is_member(c->group, c->used))
		return false;
	if (list_length(c->combos) > SIFT_AGG_MAX_COMBOS || list_length(c->joins) > SIFT_AGG_MAX_JOINS)
		return false;
	typeof_reads(c, s);
	s->npreds_total = c->npred;
	s->nvleaf = c->nvleaf;
	memcpy(s->vleaf, c->vleaf, sizeof(c->vleaf));
	c->used = bms_union(c->used, c->elem_used);
	if (reads_overflow(c, s))
		return false;
	finish_leaves(c, s);
	finish_combos(c, s);
	s->est_groups = set_groups(c, root, s, keys, input_rel->rows);
	if (c->search)
		s->search = c->search->spec;

	spec_deps(root, s);
	return true;
}

/*
 * A set filter bound as one array parameter (`col = ANY($1)`) has no length in a generic plan: PostgreSQL assumes 10 elements and prices an
 * index or bitmap scan for them, while the scan of this node takes the whole array into a byte table when it starts and costs the same
 * for 10 or 100,000 elements. The paths that fetch the rows of such a set one by one are charged the elements sift.agg_param_set_size
 * assumes (their cost grows with the set), and only when this node can run the query, so that a query it cannot take keeps the plan it
 * has. A custom plan sees the array and needs none of this.
 */
int			sift_agg_param_set_size = 1000;
int			sift_agg_lookup_max_rows = 1000000;

static double
param_set_ratio(AggCtx *c)
{
	double		f = 1.0;

	if (sift_agg_param_set_size <= 0)
		return 1.0;
	foreach_node(RestrictInfo, ri, c->rel->baserestrictinfo)
	{
		ScalarArrayOpExpr *sa;
		Node	   *arr;
		double		n0;

		if (!IsA(ri->clause, ScalarArrayOpExpr))
			continue;
		sa = (ScalarArrayOpExpr *) ri->clause;
		if (!sa->useOr || list_length(sa->args) != 2)
			continue;
		arr = strip_relabel(lsecond(sa->args));
		if (!IsA(arr, Param) || ((Param *) arr)->paramkind != PARAM_EXTERN || !IsA(strip_relabel(linitial(sa->args)), Var) ||
			match_leaf(c, linitial(sa->args)) < 0)
			continue;
		n0 = estimate_array_length(c->root, arr);
		if (sift_agg_param_set_size > n0)
			f *= sift_agg_param_set_size / n0;
	}
	return Min(f, Max(c->rel->tuples / Max(c->rel->rows, 1.0), 1.0));
}

/* the scan of the rows of the table a plan reads them with, when it is an index or bitmap scan under aggregation, sorts and gathers */
static Path *
row_scan_of(Path *p)
{
	for (;;)
		switch (nodeTag(p))
		{
			case T_IndexPath:
			case T_BitmapHeapPath:
				return p;
			case T_AggPath:
				p = ((AggPath *) p)->subpath;
				break;
			case T_GroupPath:
				p = ((GroupPath *) p)->subpath;
				break;
			case T_SortPath:
				p = ((SortPath *) p)->subpath;
				break;
			case T_IncrementalSortPath:
				p = ((IncrementalSortPath *) p)->spath.subpath;
				break;
			case T_GatherPath:
				p = ((GatherPath *) p)->subpath;
				break;
			case T_GatherMergePath:
				p = ((GatherMergePath *) p)->subpath;
				break;
			case T_ProjectionPath:
				p = ((ProjectionPath *) p)->subpath;
				break;
			default:
				return NULL;
		}
}

static void
param_set_penalty(AggCtx *c, RelOptInfo *output_rel)
{
	double		f = param_set_ratio(c);

	if (f <= 1.0)
		return;
	foreach_ptr(Path, p, output_rel->pathlist)
		if (row_scan_of(p) != NULL)
		{
			p->startup_cost *= f;
			p->total_cost *= f;
		}
}

static bool first_fits(SiftAggSpec *s);

/* the cost; adds the paths to output_rel */
static void
agg_finish(AggCtx *c, SiftAggSpec *s, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, List *keys, List *scan_tlist)
{
	int			nworkers;

	if (!spec_finish(c, s, root, input_rel, keys) || !first_fits(s) || !child_paths(c, s))
		return;
	param_set_penalty(c, output_rel);
	add_agg_path(c, s, input_rel, output_rel, target, scan_tlist, 0);
	nworkers = preagg_share(c, s) >= AGG_PREAGG_EXACT ? 0 : parallel_workers(c, s, input_rel, output_rel);
	if (nworkers > 0)
		add_agg_path(c, s, input_rel, output_rel, target, scan_tlist, nworkers);
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

static void topn_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel);

/* Does the grouping have a key that is not a constant? */
static bool
groupClause_has_nonconst(PlannerInfo *root, List *groupClause)
{
	foreach_node(SortGroupClause, sgc, groupClause)
		if (!IsA(get_sortgroupclause_expr(sgc, root->processed_tlist), Const))
			return true;
	return false;
}

/* ---------- DISTINCT ON: the first row of a group by the ORDER BY ---------- */

#define FIRST_MAX_KEYS		4		/* sort columns after the DISTINCT ON columns */
#define FIRST_GROUP_BYTES	96		/* what a group of a DISTINCT ON column keeps besides its values: the best row's header, the state and the result */

/*
 * Do the sort columns hold the columns of a unique index of the table (plain columns, NOT NULL or with NULLs not distinct), so that no two
 * rows tie? The first row of a group by the ORDER BY is then the same in every plan: a tie would be PostgreSQL's sort's choice.
 */
static bool
sort_is_total(AggCtx *c, List *sortexprs)
{
	Bitmapset  *atts = NULL;
	Relation	rel;
	bool		total = false;

	foreach_ptr(Node, e, sortexprs)
	{
		Var		   *v = (Var *) strip_relabel(e);

		if (IsA(v, Var) && v->varno == c->relid && v->varlevelsup == 0 && v->varattno > 0)
			atts = bms_add_member(atts, v->varattno);
	}
	rel = table_open(c->heapoid, NoLock);
	foreach_oid(ioid, RelationGetIndexList(rel))
	{
		HeapTuple	it = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(ioid));
		Form_pg_index ix = (Form_pg_index) GETSTRUCT(it);
		bool		all = ix->indisunique && ix->indimmediate && ix->indisvalid && ix->indnkeyatts > 0 &&
			heap_attisnull(it, Anum_pg_index_indpred, NULL) && heap_attisnull(it, Anum_pg_index_indexprs, NULL);

		for (int i = 0; all && i < ix->indnkeyatts; i++)
		{
			int			att = ix->indkey.values[i];
			HeapTuple	at = SearchSysCache2(ATTNUM, ObjectIdGetDatum(c->heapoid), Int16GetDatum(att));

			all = bms_is_member(att, atts) && (ix->indnullsnotdistinct || (HeapTupleIsValid(at) && ((Form_pg_attribute) GETSTRUCT(at))->attnotnull));
			if (HeapTupleIsValid(at))
				ReleaseSysCache(at);
		}
		ReleaseSysCache(it);
		total |= all;
	}
	table_close(rel, NoLock);
	return total;
}

/*
 * The value of e in the first row of a group by the sort columns `more`: an aggregate that only carries the argument and the sort keys of
 * the generic aggregates (its function is array_agg, whose catalog entry the compile of a generic aggregate reads); the executor keeps
 * the best row as the rows pass (SiftAggGen.first), and the group's value is e, of its own type.
 */
static Aggref *
first_aggref(AggCtx *c, Node *e, List *more)
{
	Aggref	   *a;
	Oid			t = exprType(e);
	List	   *args = list_make1(makeTargetEntry((Expr *) copyObject(e), 1, NULL, false)),
			   *order = NIL;

	if (type_is_array(t) || arg_joined(c, e))
		return NULL;
	foreach_node(SortGroupClause, sgc, more)
	{
		TargetEntry *k = makeTargetEntry((Expr *) copyObject(get_sortgroupclause_expr(sgc, c->root->processed_tlist)), list_length(args) + 1, NULL, true);
		SortGroupClause *o = copyObject(sgc);

		o->tleSortGroupRef = k->ressortgroupref = k->resno;
		args = lappend(args, k);
		order = lappend(order, o);
	}
	a = makeNode(Aggref);
	a->aggfnoid = F_ARRAY_AGG_ANYNONARRAY;
	a->aggtype = t;
	a->aggcollid = a->inputcollid = exprCollation(e);
	a->aggtranstype = INTERNALOID;
	a->aggargtypes = list_make1_oid(t);
	a->args = args;
	a->aggorder = order;
	a->aggkind = AGGKIND_NORMAL;
	a->aggsplit = AGGSPLIT_SIMPLE;
	a->aggno = a->aggtransno = -1;
	a->location = -1;
	return a;
}

/*
 * The output of DISTINCT ON: the ON columns are the group keys; any other expression of the output (the select list, and the sort columns
 * the sort above reads) is the value of the first row of its group, one aggregate for each, in the scan tuple at the place of the expression.
 */
static bool
first_aggs(AggCtx *c, PathTarget *target, List *keys, List *more, List **firsts)
{
	/* the normal plan evaluates a volatile function for every row, before it picks one */
	if (contain_volatile_functions((Node *) target->exprs))
		return false;
	foreach_ptr(Node, e, target->exprs)
	{
		OutWalk		w = {keys, NIL, false};
		Aggref	   *a;
		bool		have = false;

		out_walker(e, &w);
		if (!w.bad)
			continue;
		if ((a = first_aggref(c, e, more)) == NULL)
			return false;
		foreach_ptr(Aggref, f, *firsts)
			have |= equal(f, a);
		if (!have)
			*firsts = lappend(*firsts, a);
	}
	return true;
}

/* a DISTINCT ON aggregate keeps the best row instead of sorting the rows of the group; the first of them compares the sort keys, the others follow its answer */
static void
first_mode(SiftAggAgg *out, int nsort, bool leader)
{
	SiftAggGen *g = out->gen;

	g->sorted = false;
	g->first = true;
	g->parallel = true;
	g->cost = leader ? AGG_GEN_FIRST + nsort * AGG_GEN_FIRST_KEY : AGG_GEN_FOLLOW;
	out->desc = psprintf("first_row%s", out->desc + strlen("array_agg"));
}

/* do the best rows of the groups fit the memory a hash aggregate gets? */
static bool
first_fits(SiftAggSpec *s)
{
	double		bytes = 0;

	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].kind == SAA_GEN && s->agg[i].gen->first)
		{
			const SiftAggGen *g = s->agg[i].gen;

			bytes += FIRST_GROUP_BYTES;
			for (int k = 0; k < g->nargs; k++)
				bytes += sizeof(Datum) + 1 + get_typavgwidth(g->arg[k].typid, -1);
		}
	return bytes == 0 || s->est_groups * bytes <= (double) get_hash_memory_limit();
}

/* the sort columns after the DISTINCT ON columns, when the ORDER BY is a total order of the rows */
static bool
distinct_on_more(AggCtx *c, Query *parse, List **more)
{
	List	   *sortexprs = NIL;

	foreach_node(SortGroupClause, sgc, parse->sortClause)
	{
		sortexprs = lappend(sortexprs, get_sortgroupclause_expr(sgc, c->root->processed_tlist));
		if (arg_joined(c, llast(sortexprs)))
			return false;
		if (get_sortgroupref_clause_noerr(sgc->tleSortGroupRef, parse->distinctClause) == NULL)
			*more = lappend(*more, sgc);
	}
	return list_length(*more) <= FIRST_MAX_KEYS && sort_is_total(c, sortexprs);
}

/*
 * The rows of one scan grouped by `groupClause` (the GROUP BY of the query, or the columns of its SELECT DISTINCT, which is a grouping with no
 * aggregate): the SiftAgg path of output_rel, whose rows are those of `target` (the grouping target of the query, or its distinct target).
 */
static void
group_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, GroupPathExtraData *gextra, List *groupClause)
{
	Query	   *parse = root->parse;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	OutWalk		ow = {NIL, NIL, false};
	List	   *scan_tlist = NIL,
			   *more = NIL,
			   *firsts = NIL;
	bool		lead = true;

	if (!agg_base_either(c, root, input_rel, &fs))
		return;
	s = agg_spec_new(c);
	if (c->nparts == 0)
		c->search = sift_search_extract(root, c->rel, &fs.quals, c->indexoid, c->heapoid, false);
	if (!agg_quals(c, s, &fs))
		return;

	/* keys */
	if (list_length(groupClause) + (parse->groupingSets != NIL) > SIFT_AGG_MAX_KEYS)
		return;
	s->key = palloc0((list_length(groupClause) + 1) * sizeof(SiftAggKey));
	foreach_node(SortGroupClause, sgc, groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);

		/* a constant adds no group: the output keeps it as the constant it is (not with grouping sets, whose empty set has no key, and not alone: an empty input has no group) */
		if (IsA(e, Const) && parse->groupingSets == NIL && groupClause_has_nonconst(root, groupClause))
			continue;
		if (!compile_key(c, sgc, e, &s->key[s->nkeys]))
			return;
		combo_set_key(c, &s->key[s->nkeys], s->nkeys);
		ow.keys = lappend(ow.keys, e);
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) e, list_length(scan_tlist) + 1, NULL, false));
		s->nkeys++;
	}
	if (parse->groupingSets != NIL)
		add_sets(parse, s, &scan_tlist);

	/* DISTINCT ON: the other columns are the first row's */
	if (parse->hasDistinctOn)
	{
		if (!distinct_on_more(c, parse, &more) || !first_aggs(c, target, ow.keys, more, &firsts))
			return;
		ow.aggs = firsts;
	}
	else
	{
		/* aggregates, in the output and the HAVING clause */
		foreach_ptr(Node, e, target->exprs)
			out_walker(e, &ow);
	}
	if (gextra && gextra->havingQual)
		out_walker((Node *) gextra->havingQual, &ow);
	if (ow.bad)
		return;
	s->agg = palloc0(Max(list_length(ow.aggs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(Node, a, ow.aggs)
	{
		bool		first = list_member_ptr(firsts, a);

		if (!(IsA(a, GroupingFunc) ? compile_grouping(parse, (GroupingFunc *) a, &s->agg[s->naggs]) :
			  compile_agg(c, (Aggref *) a, &s->agg[s->naggs], s->naggs)))
			return;
		if (first)
		{
			first_mode(&s->agg[s->naggs], list_length(more), lead);
			lead = false;
		}
		scan_tlist = lappend(scan_tlist, makeTargetEntry(first ? (Expr *) linitial_node(TargetEntry, ((Aggref *) a)->args)->expr : (Expr *) a,
														 list_length(scan_tlist) + 1, NULL, false));
		s->naggs++;
	}
	agg_finish(c, s, root, input_rel, output_rel, target, ow.keys, scan_tlist);
}

void
sift_agg_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel,
					 void *extra)
{
	Query	   *parse = root->parse;

	if (stage == UPPERREL_ORDERED)
	{
		topn_paths(root, input_rel, output_rel);
		return;
	}
	if (!sift_enable_agg || parse->commandType != CMD_SELECT || parse->hasTargetSRFs || root->rowMarks != NIL || parse->hasRecursive)
		return;
	/* SELECT DISTINCT of a plain scan is a group-by with no aggregate; over a grouped or windowed query it is not the scan's */
	if (stage == UPPERREL_DISTINCT)
	{
		if (parse->distinctClause != NIL && root->upper_targets[UPPERREL_DISTINCT] != NULL &&
			!(parse->hasAggs || parse->groupClause || parse->groupingSets || parse->hasWindowFuncs || parse->havingQual))
			group_paths(root, input_rel, output_rel, root->upper_targets[UPPERREL_DISTINCT], NULL, parse->distinctClause);
		return;
	}
	if (stage != UPPERREL_GROUP_AGG || parse->hasDistinctOn || !(parse->hasAggs || parse->groupClause || parse->groupingSets))
		return;
	/* a window function is planned above the grouping stage: this stage is the same with or without one */
	if (list_length(parse->rtable) > 0 && parse->jointree->fromlist != NIL && list_length(parse->jointree->fromlist) == 1 &&
		IsA(linitial(parse->jointree->fromlist), RangeTblRef) &&
		rt_fetch(((RangeTblRef *) linitial(parse->jointree->fromlist))->rtindex, parse->rtable)->rtekind == RTE_SUBQUERY)
	{
		if (!parse->hasWindowFuncs)
			sift_agg_two_level(root, input_rel, output_rel, (GroupPathExtraData *) extra);
		return;
	}
	group_paths(root, input_rel, output_rel, output_rel->reltarget, (GroupPathExtraData *) extra, parse->groupClause);
}

/* ---------- top-N: ORDER BY <columns> LIMIT n ---------- */

/* The rows a plan assumes for a LIMIT it cannot know (a parameter of a generic plan): a page of a list */
#define TOPN_ASSUMED_ROWS	1000

/* LIMIT and OFFSET are the same at every execution of the plan: no sub-plan, no executor parameter, no volatile function */
static bool
topn_bound_ok(Node *e)
{
	return e == NULL || (sift_operand_ok(e) && !contain_volatile_functions(e));
}

/* The value the planner expects of a LIMIT or OFFSET (a parameter is a constant of a custom plan); -1 for NULL, `unknown` when it is not a constant */
static int64
topn_value(PlannerInfo *root, Node *e, int64 unknown)
{
	Node	   *v = estimate_expression_value(root, e);

	if (!IsA(v, Const))
		return unknown;
	return ((Const *) v)->constisnull ? -1 : DatumGetInt64(((Const *) v)->constvalue);
}

/*
 * LIMIT and OFFSET of the query: the rows the top-N must produce, as far as the planner knows them; 0 when there is no bound it can use.
 * A parameter of a generic plan is not known: the scan gets its value when it starts, and handles any (sift_topn.h), so the plan assumes
 * a few rows; a value the planner knows, past the cap, keeps the normal plan.
 */
static int64
topn_rows(Query *parse, PlannerInfo *root)
{
	int64		count,
				offset;

	if (parse->limitCount == NULL || parse->limitOption != LIMIT_OPTION_COUNT || !topn_bound_ok(parse->limitCount) || !topn_bound_ok(parse->limitOffset))
		return 0;
	count = topn_value(root, parse->limitCount, TOPN_ASSUMED_ROWS);
	offset = parse->limitOffset ? topn_value(root, parse->limitOffset, 0) : 0;
	if (count < 1 || offset < 0 || count > SIFT_TOPN_MAX_ROWS || offset > SIFT_TOPN_MAX_ROWS)
		return 0;
	return count + offset <= SIFT_TOPN_MAX_ROWS ? count + offset : 0;
}

/* The sort column i of the query as a leaf the top-N can order by, in the direction of its operator */
static bool
topn_key(AggCtx *c, SortGroupClause *sgc, int *leaf, bool *desc, StringInfo txt)
{
	Node	   *e = (Node *) get_sortgroupclause_expr(sgc, c->root->processed_tlist);
	Oid			opfamily,
				opcintype;
#if PG_VERSION_NUM >= 180000
	CompareType strategy;
#define TOPN_DESC	COMPARE_GT
#else
	int16		strategy;
#define TOPN_DESC	BTGreaterStrategyNumber
#endif

	if (!get_ordering_op_properties(sgc->sortop, &opfamily, &opcintype, &strategy))
		return false;
	*leaf = match_leaf(c, normalize_expr(c, e));
	if (*leaf < 0 || !sift_topn_key_ok(leaf_of(c, *leaf)) || opcintype != leaf_of(c, *leaf)->typid ||
		opfamily != get_opclass_family(GetDefaultOpClass(opcintype, BTREE_AM_OID)))
		return false;
	*desc = strategy == TOPN_DESC;
	appendStringInfo(txt, "%s%s%s", txt->len ? ", " : "", leaf_name(c, *leaf), *desc ? " DESC" : "");
	if (sgc->nulls_first != *desc)
		appendStringInfoString(txt, sgc->nulls_first ? " NULLS FIRST" : " NULLS LAST");
	use_leaf(c, *leaf);
	return true;
}

/*
 * A first sort column that follows the physical order of the table (pg_statistic correlation) lets the scan stop at the end its best values
 * are at: it reads a window of superblocks, and more only while the rows it needs are not in it. Otherwise it reads them all.
 */
static double
topn_fraction(AggCtx *c, Node *key, double matches, int64 limit)
{
	Node	   *x = strip_relabel(key);
	HeapTuple	tp;
	AttStatsSlot sslot;
	double		corr = 0,
				nsb = Max(c->stats.nunits, 1.0);

	if (!IsA(x, Var))
		return 1.0;
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(c->heapoid), Int16GetDatum(((Var *) x)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tp))
		return 1.0;
	if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		corr = sslot.nnumbers > 0 ? sslot.numbers[0] : 0;
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	if (fabs(corr) < 0.9)
		return 1.0;
	return Min(1.0, (8.0 + 2.0 * nsb * (double) limit / Max(matches, 1.0)) / nsb);
}

/*
 * The scan tuple of the top-N is the heap columns the output reads, in custom_scan_tlist: their attribute numbers go in the spec, and
 * the node's tlist (PostgreSQL's projection) computes the rest from them.
 */
static bool
topn_columns(AggCtx *c, PathTarget *target, List **scan_tlist, int *attno)
{
	List	   *vars;

	if (contain_volatile_functions((Node *) target->exprs))
		return false;
	vars = pull_var_clause((Node *) target->exprs, PVC_RECURSE_PLACEHOLDERS);
	foreach_node(Var, v, vars)
	{
		if (v->varno != c->relid || v->varlevelsup != 0 || v->varattno <= 0)
			return false;
		if (list_member(*scan_tlist, v) || tlist_member((Expr *) v, *scan_tlist))
			continue;
		if (list_length(*scan_tlist) >= MaxHeapAttributeNumber)
			return false;
		attno[list_length(*scan_tlist)] = v->varattno;
		*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) copyObject(v), list_length(*scan_tlist) + 1, NULL, false));
	}
	return true;
}

static void
topn_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	Query	   *parse = root->parse;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	CustomPath *cp;
	StringInfoData txt;
	List	   *scan_tlist = NIL;
	uint64		tlo,
				thi;
	int			leaf[SIFT_TOPN_MAX_KEYS];
	bool		desc[SIFT_TOPN_MAX_KEYS],
				nulls_first[SIFT_TOPN_MAX_KEYS];
	int		   *attno;
	int64		rows = topn_rows(parse, root);
	int			nkey = list_length(parse->sortClause);
	PathTarget *target = root->upper_targets[UPPERREL_ORDERED];

	if (!sift_enable_agg || !sift_enable_topn || rows == 0 || target == NULL || parse->commandType != CMD_SELECT || nkey == 0 || nkey > SIFT_TOPN_MAX_KEYS ||
		root->sort_pathkeys == NIL || parse->hasAggs || parse->groupClause || parse->groupingSets || parse->havingQual || parse->hasWindowFuncs ||
		parse->hasTargetSRFs || parse->distinctClause || parse->setOperations || parse->hasSubLinks || parse->hasRecursive || root->rowMarks != NIL)
		return;
	c->joinless = true;
	if (!agg_base(c, root, input_rel, &fs) || c->nparts > 0 || c->joins != NIL || c->func_rti != 0 || fs.lefts != NIL ||
		!(OidIsValid(c->indexoid) ? sift_colsrc_can_tids(get_rel_relam(c->indexoid), true) : sift_colsrc_can_tids(get_rel_relam(c->heapoid), false)))
		return;
	s = agg_spec_new(c);
	c->search = sift_search_extract(root, c->rel, &fs.quals, c->indexoid, c->heapoid, true);
	if (!agg_quals(c, s, &fs) || s->npost > 0 || sift_aggspec_tid_range(s, &tlo, &thi))
		return;
	initStringInfo(&txt);
	foreach_node(SortGroupClause, sgc, parse->sortClause)
	{
		int			j = foreach_current_index(sgc);

		if (!topn_key(c, sgc, &leaf[j], &desc[j], &txt))
			return;
		nulls_first[j] = sgc->nulls_first;
	}
	attno = palloc(MaxHeapAttributeNumber * sizeof(int));
	if (!topn_columns(c, target, &scan_tlist, attno) || !spec_finish(c, s, root, input_rel, NIL))
		return;
	c->topn_frac = topn_fraction(c, (Node *) get_sortgroupclause_expr(linitial_node(SortGroupClause, parse->sortClause), root->processed_tlist),
								 input_rel->rows, rows);
	s->topn = sift_topn_spec(nkey, leaf, desc, nulls_first, list_length(scan_tlist), attno, copyObject(parse->limitCount), copyObject(parse->limitOffset));
	record_deps(root, parse->limitCount);
	record_deps(root, parse->limitOffset);
	s->desc_topn = txt.data;

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = target;
	cp->path.parallel_safe = false;
	cp->path.pathkeys = root->sort_pathkeys;
	cp->methods = &agg_path_methods;
	agg_cost(c, s, input_rel, &cp->path, Min((double) rows, Max(input_rel->rows, 1.0)));
	cp->custom_private = list_make2(sift_aggspec_pack(s), scan_tlist);
	add_path(output_rel, &cp->path);
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
	List	   *gsgc;			/* their SortGroupClauses */
	List	   *used;			/* the group expressions that are an outer key */
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

/* the subqueries of a UNION ALL: a member that is itself a UNION ALL (a subquery of arms in an arm's place) gives its own */
static bool
append_arms(PlannerInfo *root, Index parent, List **arms)
{
	foreach_node(AppendRelInfo, ai, root->append_rel_list)
	{
		RelOptInfo *rel;
		int			i = 0;

		if (ai->parent_relid != parent)
			continue;
		rel = root->simple_rel_array[ai->child_relid];
		if (rel == NULL)
			return false;
		foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
			if (!rinfo->pseudoconstant)
				return false;
		foreach_ptr(Node, tv, ai->translated_vars)
			if (tv == NULL || !IsA(tv, Var) || ((Var *) tv)->varno != ai->child_relid || ((Var *) tv)->varattno != ++i)
				return false;
		if (rel->subroot != NULL)
			*arms = lappend(*arms, rel->subroot);
		else if (!rt_fetch(ai->child_relid, root->parse->rtable)->inh || !append_arms(root, ai->child_relid, arms))
			return false;
	}
	return true;
}

/*
 * The arms of the outer query's one FROM item: the subqueries of a UNION ALL (appendrel) or the subquery itself. The quals of the
 * outer query that PostgreSQL pushed into the arms (to their WHERE, or HAVING) are theirs now: an arm whose rel still has a qual
 * (one it did not push: volatile, a one-time filter) is not one.
 */
static List *
outer_arms(PlannerInfo *root, Index srti, List **gates)
{
	List	   *arms = NIL;
	RelOptInfo *parent = root->simple_rel_array[srti];

	if (parent == NULL)
		return NIL;
	/* the conditions without a column stay above the arms as PostgreSQL's one-time filter: the scan takes them as its own gate */
	foreach_node(RestrictInfo, rinfo, parent->baserestrictinfo)
		if (rinfo->pseudoconstant)
			*gates = lappend(*gates, rinfo->clause);
		else if (!rt_fetch(srti, root->parse->rtable)->inh)
			return NIL;
	if (!rt_fetch(srti, root->parse->rtable)->inh)
		return parent->subroot ? list_make1(parent->subroot) : NIL;
	return append_arms(root, srti, &arms) ? arms : NIL;
}

/*
 * The table an arm reads: through its CTE scan to the common table expression's root, or the arm's own root (its FROM is the scan itself:
 * a table with its unnests and lookups, which is what an arm over a common table expression that PostgreSQL inlined looks like); NULL if
 * the arm is not a simple aggregate
 */
static PlannerInfo *
arm_base(PlannerInfo *arm, ArmIn *in)
{
	Query	   *q = arm->parse;
	RangeTblEntry *rte;

	if (!q->hasAggs || q->groupClause == NIL || q->groupingSets || q->hasWindowFuncs || q->hasTargetSRFs ||
		q->distinctClause || q->sortClause || q->limitCount || q->limitOffset || q->groupDistinct || q->hasForUpdate ||
		q->setOperations || q->cteList != NIL)
		return NULL;
	in->sub.rti = 0;
	if (list_length(q->jointree->fromlist) != 1 || !IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return arm;
	rte = rt_fetch(((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex, q->rtable);
	if (rte->rtekind != RTE_CTE)
		return arm;
	in->sub.rti = ((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex;
	return cte_root(arm, rte);
}

/*
 * Two arms that read their own copy of one scan (a common table expression that PostgreSQL inlined into each): the same tables and the same
 * WHERE. The entries of pulled up subqueries and of the grouping belong to their arm, and the column names a plan prunes differ: none counts.
 */
static bool
arm_private_rte(RTEKind kind)
{
#if PG_VERSION_NUM >= 180000
	if (kind == RTE_GROUP)
		return true;
#endif
	return kind == RTE_SUBQUERY;
}

static bool
same_scan(PlannerInfo *a, PlannerInfo *b)
{
	ListCell   *la,
			   *lb;

	if (list_length(a->parse->rtable) != list_length(b->parse->rtable) || !equal(a->parse->jointree, b->parse->jointree))
		return false;
	forboth(la, a->parse->rtable, lb, b->parse->rtable)
	{
		RangeTblEntry same = *(RangeTblEntry *) lfirst(la),
				   *rb = lfirst(lb);

		same.eref = rb->eref;
		if (same.rtekind != rb->rtekind || (!arm_private_rte(same.rtekind) && !equal(&same, rb)))
			return false;
	}
	return true;
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
		case SAA_X_SUM:
		case SAA_X_AVG:
			/* the expression is evaluated for every tuple of the scan: an arm's own WHERE or a FILTER would keep it off the tuples they drop */
			if (ia->restype != FLOAT8OID || in->quals != NIL || ia->filter != NULL)
				return -1;
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

/* the conditions of the outer query without a column, which PostgreSQL left above the arms, as conjuncts of the scan */
static bool
arm_gates(AggCtx *c, SiftAggSpec *s, List *gates)
{
	s->pred = repalloc(s->pred, Max(s->npred + list_length(gates), 1) * sizeof(SiftAggPred *));
	foreach_ptr(Node, q, gates)
	{
		SiftAggPred *p = compile_pred(c, normalize_expr(c, q));

		if (p == NULL || p->kind != SAP_GATE)
			return false;
		s->pred[s->npred++] = p;
		s->desc_quals = psprintf("%s%s%s", s->desc_quals, *s->desc_quals ? " AND " : "", p->desc);
	}
	return true;
}

typedef struct HavingSub
{
	AggCtx	   *c;
	ArmIn	   *in;
	List	   *src;			/* inner aggregate of each Param made */
	bool		bad;
} HavingSub;

/* the inner aggregates of a HAVING conjunct become Params; a column (a group expression) or a volatile function is not allowed */
static Node *
having_mutator(Node *n, HavingSub *h)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Aggref))
	{
		int			ai = h->bad || list_length(h->src) >= SIFT_AGG_MAX_PARAMS ? -1 : arm_inner_agg(h->c, h->in, n, exprType(n));
		Param	   *p = makeNode(Param);

		if (ai < 0)
			h->bad = true;
		p->paramkind = PARAM_EXEC;
		p->paramid = list_length(h->src);
		p->paramtype = exprType(n);
		p->paramtypmod = -1;
		p->paramcollid = InvalidOid;
		p->location = -1;
		h->src = lappend_int(h->src, ai);
		return (Node *) p;
	}
	if (IsA(n, Var) || (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC) || IsA(n, SubPlan) || IsA(n, WindowFunc) || IsA(n, GroupingFunc))
		h->bad = true;
	return expression_tree_mutator(n, having_mutator, h);
}

/* the conjuncts of the arm's HAVING that are not `inner aggregate op constant`: PostgreSQL evaluates them on the values of each cell */
static bool
arm_having_expr(AggCtx *c, ArmIn *in, List *quals)
{
	HavingSub	h = {c, in, NIL, false};
	SiftAggArm *a = in->out;

	if (contain_volatile_functions((Node *) quals))
		return false;
	a->hexpr = having_mutator((Node *) make_ands_explicit(quals), &h);
	a->nhp = list_length(h.src);
	a->hsrc = palloc(Max(a->nhp, 1) * sizeof(int));
	foreach_int(ai, h.src)
		a->hsrc[foreach_current_index(ai)] = ai;
	return !h.bad;
}

static bool
count_aggs_walker(Node *n, int *count)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
		(*count)++;
	return expression_tree_walker(n, count_aggs_walker, count);
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
 * A cell is an inner group, the share of one row in one group of an arm. When an arm groups by more than the outer keys and its id, one
 * row has several cells in an outer group, so the group expressions that are no outer key are keys of the cells: one key each, shared by
 * the arms that have an equal expression, after the outer keys and before the number of the set. They name no group (a group has id 0
 * for them, and the scan emits NULL).
 */
static bool
hidden_keys(AggCtx *c, SiftAggSpec *s, ArmIn *arms, int narms, List **scan_tlist, bool sets)
{
	int			nouter = s->nkeys;
	List	   *defs = NIL;

	for (int k = 0; k < narms; k++)
		for (int i = 0; i < list_length(arms[k].group); i++)
		{
			Node	   *g = list_nth(arms[k].group, i);
			SortGroupClause *sgc = list_nth_node(SortGroupClause, arms[k].gsgc, i);
			int			pos;
			SiftAggKey *key;

			if (equal(g, arms[k].id) || list_member(arms[k].used, g))
				continue;
			pos = -1;
			foreach_ptr(Node, d, defs)
				if (equal(d, g))
					pos = foreach_current_index(d);
			if (pos < 0)
			{
				if (s->nkeys + sets >= SIFT_AGG_MAX_KEYS)
					return false;
				key = &s->key[s->nkeys];
				if (!compile_key(c, sgc, g, key))
					return false;
				combo_set_key(c, key, s->nkeys);
				*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) makeNullConst(key->typid, key->typmod, key->collid),
																   list_length(*scan_tlist) + 1, NULL, false));
				defs = lappend(defs, g);
				pos = s->nkeys++ - nouter;
			}
			arms[k].out->hmask |= (uint32) 1 << (nouter + pos);
		}
	s->nhidden = s->nkeys - nouter;
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
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	OutWalk		ow = {NIL, NIL, false};
	List	   *scan_tlist = NIL;
	int			okey[SIFT_AGG_MAX_KEYS];
	int			ncols = 0;
	List	   *gates = NIL;
	int			nkeyslots = list_length(parse->groupClause) + 1;

	if (list_length(parse->groupClause) > SIFT_AGG_MAX_KEYS - 1)
		return;
	roots = outer_arms(root, srti, &gates);
	if (roots == NIL || list_length(roots) > AGG_MAX_ARMS)
		return;
	foreach_ptr(PlannerInfo, ar, roots)
	{
		PlannerInfo *b = arm_base(ar, &arms[narms]);

		if (b == NULL || (base != NULL && b != base && !(b == ar && same_scan(base, b))))
			return;
		if (base == NULL)
			base = b;
		arms[narms].sub.tlist = b->parse->targetList;
		narms++;
	}
	if (!agg_base(c, base, NULL, &fs) || c->nparts > 0)
		return;
	s = agg_spec_new(c);
	if (!agg_quals(c, s, &fs) || !arm_gates(c, s, gates))
		return;

	/* the arms in the terms of the scanned table */
	for (int k = 0; k < narms; k++)
	{
		Query	   *aq = ((PlannerInfo *) list_nth(roots, k))->parse;
		ArmIn	   *in = &arms[k];
		int			nid = 0,
					nagg;
		List	   *rest = NIL;

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
			in->gsgc = lappend(in->gsgc, sgc);
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
		nagg = ncols;
		count_aggs_walker(aq->havingQual, &nagg);
		in->out->inner = palloc0((nagg + 1) * sizeof(SiftAggAgg));
		in->out->having = palloc0(Max(list_length((List *) aq->havingQual), 1) * sizeof(SiftAggCond));
		foreach_ptr(Node, q, (List *) aq->havingQual)
		{
			Node	   *hq = normalize_expr(c, cte_sub_mutator(q, &in->sub));

			if (arm_having(c, in, hq, &in->out->having[in->out->nhaving]))
				in->out->nhaving++;
			else
				rest = lappend(rest, hq);
		}
		if (rest != NIL && !arm_having_expr(c, in, rest))
			return;
		in->out->filter = palloc(Max(list_length(in->quals), 1) * sizeof(SiftAggPred *));
		foreach_ptr(Node, q, in->quals)
		{
			SiftAggPred *p = compile_combo_pred(c, q);

			if (p == NULL || unsupported_clause(c, q))
				return;
			in->out->filter[in->out->nfilter++] = p;
		}
	}

	/* the outer keys: columns that every arm groups by or has as NULL */
	for (int k = 0; k < narms; k++)
		nkeyslots += list_length(arms[k].group);
	s->key = palloc0(nkeyslots * sizeof(SiftAggKey));
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
			arms[k].used = lappend(arms[k].used, x);
			arms[k].out->keymask |= (uint32) 1 << s->nkeys;
		}
		if (def == NULL || !compile_key(c, sgc, def, &s->key[s->nkeys]))
			return;
		combo_set_key(c, &s->key[s->nkeys], s->nkeys);
		ow.keys = lappend(ow.keys, e);
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) e, list_length(scan_tlist) + 1, NULL, false));
		s->nkeys++;
	}
	if (!hidden_keys(c, s, arms, narms, &scan_tlist, parse->groupingSets != NIL))
		return;
	if (parse->groupingSets != NIL)
		add_sets(parse, s, &scan_tlist);

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
		if (!(IsA(a, GroupingFunc) ? compile_grouping(parse, (GroupingFunc *) a, &s->agg[s->naggs]) :
			  IsA(a, Aggref) && outer_agg(c, s, (Aggref *) a, srti, arms, narms, &s->agg[s->naggs])))
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
	agg_finish(c, s, root, input_rel, output_rel, output_rel->reltarget, ow.keys, scan_tlist);
}

/* ---------- plan ---------- */

/* the native plans of the correlated children, made now that the plan has its place in the statement; true when the spec holds their IDs */
static bool
plan_correlated_children(PlannerInfo *root, CustomPath *best, SiftAggSpec *spec)
{
	bool		planned = false;

	for (int i = 0; i < spec->njoin; i++)
		if (spec->join[i].params != NIL)
		{
			Path	   *subpath = list_nth(((SiftAggPath *) best)->correlated, i);
			PlannerInfo *subroot = list_nth(((SiftAggPath *) best)->subroots, i);
			Plan       *plan = create_plan(subroot, subpath);

			root->glob->subplans = lappend(root->glob->subplans, plan);
			root->glob->subpaths = lappend(root->glob->subpaths, subpath);
			root->glob->subroots = lappend(root->glob->subroots, subroot);
			spec->join[i].subplan = list_length(root->glob->subplans);
			planned = true;
		}
	return planned;
}

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
	cs->custom_plans = custom_plans;
	cs->custom_scan_tlist = copyObject(scan_tlist);
	cs->custom_private = plan_correlated_children(root, best, spec) ? sift_aggspec_pack(spec) : pack;
	cs->methods = &sift_agg_plan_methods;
	if (root->hasHavingQual && root->parse->havingQual)
		cs->scan.plan.qual = (List *) copyObject(root->parse->havingQual);
	(void) gextra;
	if (OidIsValid(spec->indexoid))
		root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->indexoid);
	for (int i = 0; i < spec->nparts; i++)
		root->glob->relationOids = lappend_oid(lappend_oid(root->glob->relationOids, spec->partheap[i]), spec->partindex[i]);
	for (int i = 0; i < spec->njoin; i++)
		if (OidIsValid(spec->join[i].relid))
			root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->join[i].relid);
	return &cs->scan.plan;
}

static const CustomPathMethods agg_path_methods = {"SiftAgg", agg_plan_path, NULL};

const CustomScanMethods sift_agg_plan_methods = {"SiftAgg", sift_agg_create_state};
