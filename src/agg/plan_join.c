/*
 * agg/plan_join.c - the path with lookup joins: children, joins and the order of the lookups
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
#include "optimizer/restrictinfo.h"
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

/* ---------- the path ---------- */

bool
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

/*
 * What a correlated child is given for a column of the query. The value of a jsonb_each member is not a stored leaf (the leaf holds its
 * float8), so the child gets the jsonb the leaf stands for, built from the key (a member exists) and the number: a JSON null is the NULL
 * of the number, a number is the jsonb of its float8. A cast the child makes of it raises as it does on the stored value, for the
 * members the child runs for.
 */
static Node *
child_arg(AggCtx *c, Node *arg)
{
	Var		   *v = IsA(arg, Var) ? (Var *) arg : NULL;
	Var		   *key;
	Node	   *number;
	CaseExpr   *out = makeNode(CaseExpr);
	CaseWhen   *no_member = makeNode(CaseWhen),
			   *json_null = makeNode(CaseWhen);
	NullTest   *absent = makeNode(NullTest),
			   *nil = makeNode(NullTest);

	if (v == NULL || c->func_kind != FK_EACH || v->varlevelsup != 0 || v->varno != c->func_rti || v->varattno != 2 || v->vartype != JSONBOID)
		return arg;
	key = copyObject(v);
	key->varattno = 1;
	key->vartype = TEXTOID;
	key->vartypmod = -1;
	key->varcollid = DEFAULT_COLLATION_OID;
	number = (Node *) makeFuncExpr(F_FLOAT8_JSONB, FLOAT8OID, list_make1(arg), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	absent->arg = (Expr *) key;
	absent->nulltesttype = IS_NULL;
	absent->location = -1;
	nil->arg = (Expr *) number;
	nil->nulltesttype = IS_NULL;
	nil->location = -1;
	no_member->expr = (Expr *) absent;
	no_member->result = (Expr *) makeNullConst(JSONBOID, -1, InvalidOid);
	no_member->location = -1;
	json_null->expr = (Expr *) nil;
	json_null->result = (Expr *) makeConst(JSONBOID, -1, InvalidOid, -1, DirectFunctionCall1(jsonb_in, CStringGetDatum("null")), false, false);
	json_null->location = -1;
	out->casetype = JSONBOID;
	out->args = list_make2(no_member, json_null);
	/* a row without the member and one with a JSON null are the same NULL: the child reads the value only through casts (null_blind) */
	if (c->func_key)
	{
		no_member->expr = (Expr *) nil;
		out->args = list_make1(no_member);
	}
	out->defresult = (Expr *) makeFuncExpr(F_TO_JSONB, JSONBOID, list_make1(copyObject(number)), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	out->location = -1;
	return (Node *) out;
}

typedef struct BareRead
{
	Index		rti;			/* the unnested function */
	int			level;			/* query levels below the child's own */
	bool		key;			/* the key is read too: a row without the member has none */
} BareRead;

/* a cast the float8 of the leaf serves exactly: a numeric keeps the scale the document wrote, and a boolean is no float8 */
static bool
float_cast(Oid fid)
{
	Oid			to = json_cast_target(fid);

	return to != InvalidOid && to != NUMERICOID && to != BOOLOID;
}

/* does the child read a column of the function other than as the argument of a cast the float8 of the leaf serves */
static bool
bare_read_walker(Node *n, BareRead *x)
{
	if (n == NULL)
		return false;
	if (IsA(n, FuncExpr) && float_cast(((FuncExpr *) n)->funcid) && IsA(linitial(((FuncExpr *) n)->args), Var) &&
		((Var *) linitial(((FuncExpr *) n)->args))->varno == x->rti && ((Var *) linitial(((FuncExpr *) n)->args))->varlevelsup == x->level &&
		((Var *) linitial(((FuncExpr *) n)->args))->varattno == 2)
		return false;
	if (IsA(n, Var))
		return ((Var *) n)->varno == x->rti && ((Var *) n)->varlevelsup == x->level && (x->key || ((Var *) n)->varattno == 2);
	if (IsA(n, Query))
	{
		bool		bare;

		x->level++;
		bare = query_tree_walker((Query *) n, bare_read_walker, x, 0);
		x->level--;
		return bare;
	}
	return expression_tree_walker(n, bare_read_walker, x);
}

/* The parameters of a child and what the scan gives for each. */
static void
child_params(AggCtx *c, JoinCtx *jc, List *items)
{
	foreach_node(PlannerParamItem, item, items)
	{
		jc->j.params = lappend_int(jc->j.params, item->paramId);
		jc->args = lappend(jc->args, child_arg(c, copyObject(item->item)));
	}
}

/* The plan PostgreSQL made of a correlated sub-select, with the quals it pushed into it: its parameters are what it read of the query's rows. */
static bool
planned_child(AggCtx *c, RelOptInfo *rel, JoinCtx *jc)
{
	if ((jc->cpath = child_path(rel, true)) == NULL || !IsA(jc->cpath, SubqueryScanPath) || rel->subplan_params == NIL)
		return false;
	jc->subroot = rel->subroot;
	child_params(c, jc, rel->subplan_params);
	/* the query as written, not as planned (PostgreSQL plans only the columns the query reads, the others are NULL): the arms that read other columns of it have the same child */
	jc->j.sig = sift_agg_child_sig((Node *) list_make2(planner_rt_fetch(rel->relid, c->root)->subquery, extract_actual_clauses(rel->baserestrictinfo, false)), jc->j.params);
	return true;
}

/* the path of a correlated child planned in jc->subroot: its cheapest path under a scan of the relation of the sub-select */
static void
child_scan_path(RelOptInfo *rel, JoinCtx *jc)
{
	RelOptInfo *final = fetch_upper_rel(jc->subroot, UPPERREL_FINAL, NULL);
	SubqueryScanPath *p = makeNode(SubqueryScanPath);

	set_cheapest(final);
	p->path.pathtype = T_SubqueryScan;
	p->path.parent = rel;
	p->path.pathtarget = rel->reltarget;
	p->subpath = final->cheapest_total_path;
	p->path.rows = p->subpath->rows;
	p->path.startup_cost = p->subpath->startup_cost;
	p->path.total_cost = p->subpath->total_cost + cpu_tuple_cost * p->path.rows;
	jc->cpath = (Path *) p;
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

	/*
	 * The value a child is given is the jsonb of the float8 the leaf holds: it reads the member only through casts to a number that float8 serves
	 * exactly. The member a LEFT JOIN LATERAL jsonb_each picks is NULL for a row without it and for a JSON null alike (a JSON null casts to NULL in
	 * PostgreSQL 18): the child does not read its key either.
	 */
	if (c->func_kind == FK_EACH && bare_read_walker(e->rtekind == RTE_FUNCTION ? (Node *) e->functions : (Node *) e->subquery,
													&(BareRead) {c->func_rti, 0, c->func_key != NULL}))
		return false;
	if (c->nsiblings > 1 && raising_walker(e->rtekind == RTE_FUNCTION ? (Node *) e->functions : (Node *) e->subquery, c))
		return false;
	if (rel->baserestrictinfo != NIL && !(e->rtekind == RTE_SUBQUERY && (e->subquery->limitCount || e->subquery->limitOffset)))
		return e->rtekind == RTE_SUBQUERY && planned_child(c, rel, jc);
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
	child_params(c, jc, c->root->plan_params);
	c->root->plan_params = saved;
	if (jc->j.params == NIL)
		return false;
	jc->j.sig = sift_agg_child_sig((Node *) q, jc->j.params);
	child_scan_path(rel, jc);
	return true;
}

/* what a sub-select inside the elements of an array reads outside itself becomes a parameter whose value the scan computes (as derive_mutator's) */
typedef struct LiftCtx
{
	PlannerInfo *root;
	int			depth;			/* the query levels below the sub-select's own */
	Index		offset;			/* added to the range table indexes of the EXISTS' own level, which the scan's expressions share with the query's */
	List	   *args;			/* the expression each parameter stands for, in the terms of the merged range table */
	List	   *params;
	bool		bad;
} LiftCtx;

static Node *
lift_param(LiftCtx *x, Node *outer)
{
	Param	   *prm = makeNode(Param);
	int			at = -1;

	for (int i = 0; i < list_length(x->args); i++)
		if (equal(list_nth(x->args, i), outer))
			at = i;
	if (at < 0)
	{
		x->args = lappend(x->args, outer);
		x->params = lappend_int(x->params, assign_special_exec_param(x->root));
		at = list_length(x->args) - 1;
	}
	prm->paramkind = PARAM_EXEC;
	prm->paramid = list_nth_int(x->params, at);
	prm->paramtype = exprType(outer);
	prm->paramtypmod = exprTypmod(outer);
	prm->paramcollid = exprCollation(outer);
	prm->location = -1;
	return (Node *) prm;
}

/* the column as the scan reads it: of the query (two levels up) or of the EXISTS' tables (one up), in the merged range table */
static Node *
lift_outer(LiftCtx *x, Var *v)
{
	Var		   *own = copyObject(v);
	int			up = v->varlevelsup - x->depth;

	own->varlevelsup = 0;
	if (up == 1)
		own->varno += x->offset;
	return (Node *) own;
}

static Node *
lift_mutator(Node *n, LiftCtx *x)
{
	Var		   *v;

	if (n == NULL)
		return NULL;
	if (IsA(n, Query))
	{
		Query	   *q;

		x->depth++;
		q = query_tree_mutator((Query *) n, lift_mutator, x, 0);
		x->depth--;
		return (Node *) q;
	}
	if (derivable_call(n) && IsA(v = (Var *) strip_relabel(linitial(func_args(n))), Var) && v->varlevelsup > x->depth && v->varlevelsup <= x->depth + 2)
	{
		Node	   *call = copyObject(n);

		foreach_ptr(Node, other, func_args(call))
			if (foreach_current_index(other) > 0 && !IsA(strip_relabel(other), Const))
				return expression_tree_mutator(n, lift_mutator, x);
		*(Var *) strip_relabel(linitial(func_args(call))) = *(Var *) lift_outer(x, v);
		return lift_param(x, call);
	}
	if (IsA(n, Var) && ((Var *) n)->varlevelsup > x->depth)
	{
		if (((Var *) n)->varlevelsup > x->depth + 2)
			x->bad = true;
		else
			return lift_param(x, lift_outer(x, (Var *) n));
	}
	return expression_tree_mutator(n, lift_mutator, x);
}

/*
 * The sub-select of a LEFT JOIN LATERAL inside an EXISTS over the elements of an array (a lookup of one row per element, LIMIT 1) as a child of
 * the scan: it reads the element's columns (one query level up) and the row's (two up) as parameters, which become the inputs of jc's combo.
 * *args are the expressions of the parameters in the merged range table (the query's, then the EXISTS' shifted by offset). Not planned: false.
 */
bool
scoped_child(AggCtx *c, Query *sub, Index offset, JoinCtx *jc, List **args)
{
	LiftCtx		x = {c->root, 0, offset};
	Query	   *lifted = (Query *) query_tree_mutator(copyObject(sub), lift_mutator, &x, 0);
	List	   *saved = c->root->plan_params;

	if (x.bad || x.args == NIL || sub->rowMarks != NIL || contain_volatile_functions((Node *) sub))
		return false;
	/* the parameters are plan parameters of this level while the child is planned, as the derived calls' are (correlated_child) */
	c->root->plan_params = NIL;
	for (int i = 0; i < list_length(x.args); i++)
	{
		PlannerParamItem *item = makeNode(PlannerParamItem);

		item->item = list_nth(x.args, i);
		item->paramId = list_nth_int(x.params, i);
		c->root->plan_params = lappend(c->root->plan_params, item);
	}
	jc->j.params = x.params;
	jc->subroot = sift_subquery_planner(c->root->glob, lifted, c->root, jc->j.desc);
	c->root->plan_params = saved;
	child_scan_path(c->rel, jc);
	*args = x.args;
	return true;
}

/* the tuples that can reach a child: the scanned table's rows, each joined to all the rows of another table whose columns the child reads */
static double
child_reach(AggCtx *c, JoinCtx *jc)
{
	Relids		rels = pull_varnos(c->root, (Node *) jc->args);
	double		reach = c->rel->tuples;

	for (int rti = -1; (rti = bms_next_member(rels, rti)) >= 0;)
		if (rti != c->relid && c->root->simple_rel_array[rti] != NULL)
			reach *= Max(c->root->simple_rel_array[rti]->tuples, 1.0);
	return reach;
}

/* the rows the scan keeps of a correlated child, those of its plan for each distinct input, are bounded as the rows of a plan read once are */
static bool
child_rows_fit(AggCtx *c, JoinCtx *jc)
{
	double		reach = child_reach(c, jc);

	return jc->cpath->rows * Min(estimate_num_groups(c->root, jc->args, reach, NULL, NULL), reach) <= sift_agg_lookup_max_rows;
}

static Node *
param_position_mutator(Node *n, List *params)
{
	Param	   *p;
	int			at = 0;

	if (n == NULL)
		return NULL;
	if (IsA(n, Query))
		return (Node *) query_tree_mutator((Query *) n, param_position_mutator, params, 0);
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
		foreach_int(id, params)
		{
			if (id == ((Param *) n)->paramid)
			{
				p = copyObject((Param *) n);
				p->paramid = at;
				return (Node *) p;
			}
			at++;
		}
	return expression_tree_mutator(n, param_position_mutator, params);
}

/* a correlated child (its query) with the parameters it reads numbered by their position: the children of two arms are the same plan when these are equal */
Node *
sift_agg_child_sig(Node *n, List *params)
{
	return param_position_mutator(copyObject(n), params);
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
			jc->j.sig = done->j.sig;
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
			/* A CTE of a query level that reads nothing of the queries around it has no changing outer-row inputs, wherever it is referenced. */
			if (e->self_reference || cte_owner_correlated(owner))
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

/*
 * The ON conjuncts of a join that keeps the other table and read only the scanned table (and the rows it unnests) decide which of its
 * rows can match, as the planner would filter them before the join: they become conditions of the scan, which an EXISTS or a jsonpath test
 * can be too. The rest is the join's own.
 */
static List *
nullable_filters(AggCtx *c, FromScan *fs, List *quals, Index dim)
{
	List	   *rest = NIL;
	Relids		own = bms_make_singleton(c->relid),
				nullable = NULL;

	if (c->func_rti)
		own = bms_add_member(own, c->func_rti);
	/* an EXISTS may read any table of the nullable input: it filters the joined tuples */
	foreach_int(rti, fs->rtis)
		if (rti != dim)
			nullable = bms_add_member(nullable, rti);
	foreach_ptr(Node, q, quals)
	{
		Relids		v = pull_varnos(c->root, q);

		if (!bms_is_empty(v) && bms_is_subset(v, IsA(q, SubPlan) ? nullable : own) && !contain_volatile_functions(q))
			fs->scan_quals = lappend(fs->scan_quals, q);
		else
			rest = lappend(rest, q);
	}
	return rest;
}

/* a table or plan in the FROM list besides the scanned one: it joins by equality of its columns to leaves */
bool
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
			jc->clauses = lj->kind == SJ_RIGHT ? nullable_filters(c, fs, lj->quals, rti) : lj->quals;
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
 * `column = value` or `column = ANY (array)` (IN (list)) among the table's own conditions (the value reads no column, is not volatile and takes no
 * executor parameter): the rows of the table that can match are those of an index scan on the column, when the table has a btree on it (the executor
 * looks, map_load). The conjunct stays in the filter; this only narrows what is read. A list that keeps a large share of the table is read faster by
 * the heap scan than through the index, so it is only for a list the statistics of the table expect to keep a small share.
 */
static bool
index_qual(AggCtx *c, JoinCtx *jc, Node *q, Oid *opno, Oid *coll, Node **col, Node **val, bool *any)
{
	Node	   *l,
			   *r;

	if (IsA(q, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) q)->useOr)
	{
		ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) q;

		*opno = sa->opno;
		*coll = sa->inputcollid;
		l = strip_relabel(linitial(sa->args));
		r = lsecond(sa->args);
		*any = true;
		if (join_of_var(c, l) != jc || clause_selectivity(c->root, q, jc->rti, JOIN_INNER, NULL) * AGG_IDX_ROW >= AGG_JOIN_ROW)
			return false;
	}
	else if (IsA(q, OpExpr) && list_length(((OpExpr *) q)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) q;

		*opno = op->opno;
		*coll = op->inputcollid;
		l = strip_relabel(linitial(op->args));
		r = strip_relabel(lsecond(op->args));
		*any = false;
		if (join_of_var(c, l) != jc)
		{
			if (join_of_var(c, r) != jc)
				return false;
			*opno = get_commutator(*opno);
			l = r;
			r = strip_relabel(linitial(op->args));
		}
	}
	else
		return false;
	*col = l;
	*val = r;
	return OidIsValid(*opno) && op_mergejoinable(*opno, exprType(l)) && !unsupported_clause(c, r) && !contain_var_clause(r);
}

static void
join_index_quals(AggCtx *c, JoinCtx *jc, List *conds)
{
	SiftAggJoin *j = &jc->j;

	if (jc->cpath != NULL || sift_aggjoin_preserves(jc->kind))
		return;
	foreach_ptr(Node, q, conds)
	{
		Node	   *col,
				   *val;
		Oid			opno,
					coll;
		bool		any;

		if (j->nidx == SIFT_AGG_MAX_JIDX || !index_qual(c, jc, q, &opno, &coll, &col, &val, &any))
			continue;
		j->idxatt[j->nidx] = ((Var *) col)->varattno;
		j->idxop[j->nidx] = opno;
		j->idxcoll[j->nidx] = coll;
		j->idxany[j->nidx] = any;
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

bool
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

/* the columns of a joined sub-select that its plan outputs: those the query reads, and (the plan evaluates them) those of the conditions it keeps */
static Bitmapset *
child_kept_attrs(AggCtx *c, JoinCtx *jc)
{
	RelOptInfo *rel = find_base_rel(c->root, jc->rti);
	Bitmapset  *used = NULL;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &used);
	if (projects_columns(jc->cpath))
		foreach_ptr(RestrictInfo, ri, list_concat_copy(rel->baserestrictinfo, rel->joininfo))
			pull_varattnos((Node *) ri->clause, rel->relid, &used);
	return used;
}

/*
 * A column of a sub-select that only a condition pushed down into it read is a NULL in its plan (remove_unused_subquery_outputs), so the
 * spec, which reads the condition again, must not read it: the normal plan stays. The plan of a sub-select, VALUES list or function keeps
 * the columns of the conditions it evaluates; an Append or MergeAppend outputs the relation's target only.
 */
static bool
child_cols_kept(AggCtx *c, JoinCtx *jc)
{
	Bitmapset  *used = child_kept_attrs(c, jc);

	foreach_ptr(Var, v, jc->cvars)
		if (!bms_is_member(v->varattno - FirstLowInvalidHeapAttributeNumber, used))
			return false;
	return true;
}

/*
 * A condition over the columns of a joined sub-select that PostgreSQL pushed down into it (a run condition of a ranking window, a filter of
 * its scans): its plan enforces it, so its rows are those the condition keeps and the scan does not test it again. Such a condition is
 * recognized by a column that nothing else of the query keeps in the plan's output.
 */
static bool
pushed_into_child(AggCtx *c, JoinCtx *jc, Node *q)
{
	Bitmapset  *vars = NULL;

	if (jc->cpath == NULL || jc->j.params != NIL || !projects_columns(jc->cpath) || !bms_equal(lookup_inputs(c, q), bms_make_singleton(jc->rti)))
		return false;
	pull_varattnos(q, jc->rti, &vars);
	return bms_nonempty_difference(vars, child_kept_attrs(c, jc));
}

/* the conditions of the list that no plan of a joined sub-select has enforced already */
static List *
drop_pushed(AggCtx *c, List *quals)
{
	List	   *kept = NIL;

	foreach_ptr(Node, q, quals)
	{
		bool		pushed = false;

		foreach_ptr(JoinCtx, jc, c->joins)
			pushed |= pushed_into_child(c, jc, q);
		if (!pushed)
			kept = lappend(kept, q);
	}
	return kept;
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
		else if (owner->kind == SJ_INNER && (nullable_input || !preserved_join(c)) && !IsA(q, SubPlan))
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

/* the two columns of `a = b` (the same type and collation, no cast); false for any other conjunct */
static bool
column_equality(Node *q, Var **a, Var **b, Oid *opno)
{
	OpExpr	   *op;

	if (!IsA(q, OpExpr) || list_length(((OpExpr *) q)->args) != 2)
		return false;
	op = (OpExpr *) q;
	*a = (Var *) strip_relabel(linitial(op->args));
	*b = (Var *) strip_relabel(lsecond(op->args));
	*opno = op->opno;
	return IsA(*a, Var) && IsA(*b, Var) && (*a)->varlevelsup == 0 && (*b)->varlevelsup == 0 && (*a)->varnullingrels == NULL &&
		(*b)->varnullingrels == NULL && (*a)->vartype == (*b)->vartype && (*a)->varcollid == (*b)->varcollid && op_mergejoinable(op->opno, (*a)->vartype);
}

/* the column of the scanned table that inner lookup column v equals in one of the conjuncts (NULL: none) */
static Var *
scanned_key(AggCtx *c, List *quals, Var *v, Oid opno)
{
	foreach_ptr(Node, q, quals)
	{
		Var		   *a,
				   *b;
		Oid			o;

		if (!column_equality(q, &a, &b, &o) || o != opno)
			continue;
		if (a->varno == c->relid && b->varno == v->varno && b->varattno == v->varattno)
			return a;
		if (b->varno == c->relid && a->varno == v->varno && a->varattno == v->varattno)
			return b;
	}
	return NULL;
}

/*
 * `scanned.k = A.id AND A.id = B.id` (A and B inner lookups) holds the same rows as `scanned.k = A.id AND scanned.k = B.id`: B is then probed with a
 * column of the scanned row, where its chain through A needs A resolved first. The keys of B become a conjunct of the scan (join_filters), as
 * when B is joined to the table itself: the units they miss are not read.
 */
static List *
rekey_chains(AggCtx *c, List *quals)
{
	List	   *out = list_copy(quals);
	bool		changed = true;

	while (changed)
	{
		changed = false;
		for (int i = 0; i < list_length(out); i++)
		{
			Var		   *a,
					   *b,
					   *key;
			Oid			opno;
			JoinCtx    *ja,
					   *jb;
			OpExpr	   *op;

			if (!column_equality(list_nth(out, i), &a, &b, &opno) || (ja = join_of_var(c, (Node *) a)) == NULL ||
				(jb = join_of_var(c, (Node *) b)) == NULL || ja->kind != SJ_INNER || jb->kind != SJ_INNER)
				continue;
			if (scanned_key(c, out, a, opno) == NULL)
			{
				Var		   *t = a;

				a = b;
				b = t;
			}
			if (scanned_key(c, out, b, opno) != NULL || (key = scanned_key(c, out, a, opno)) == NULL)
				continue;
			op = copyObject((OpExpr *) list_nth(out, i));
			linitial(op->args) = copyObject(key);
			lsecond(op->args) = copyObject(b);
			list_nth_cell(out, i)->ptr_value = op;
			changed = true;
		}
	}
	return out;
}

bool
setup_joins(AggCtx *c, List **quals, List **post, List **scan_quals, List **scan_post)
{
	List	   *probe_quals;

	*quals = drop_pushed(c, *quals);
	*scan_quals = drop_pushed(c, *scan_quals);
	*quals = rekey_chains(c, *quals);
	foreach_ptr(JoinCtx, jc, c->joins)
		jc->clauses = drop_pushed(c, jc->clauses);
	probe_quals = preserved_join(c) ? list_copy(*scan_quals) : list_concat_copy(*quals, *scan_quals);
	foreach_ptr(JoinCtx, jc, c->joins)
		if (jc->kind == SJ_INNER)
			probe_quals = list_concat(probe_quals, list_copy(jc->clauses));
	if (!order_lookups(c, probe_quals))
		return false;
	if (!join_quals(c, quals, post, false) || !join_quals(c, scan_quals, scan_post, true))
		return false;
	/* an EXISTS in the ON clause of an inner join filters its tuples, as in the WHERE (inside the nullable input of a preserved join, if there is one) */
	foreach_ptr(JoinCtx, jc, c->joins)
		if (jc->kind == SJ_INNER)
			foreach_ptr(Node, q, jc->clauses)
				if (IsA(q, SubPlan))
				{
					List	  **dest = preserved_join(c) ? scan_post : post;

					*dest = lappend(*dest, q);
					jc->clauses = foreach_delete_current(jc->clauses, q);
				}
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
				use_matched_leaf(c, w.leaf[p]);
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
void
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
 * The plans the joins read, as paths that output the columns the spec reads (the columns of the joined rows are positions of that output): the
 * path PostgreSQL chose for the rows of the sub-select, VALUES list or function, projected on them. The scan nodes project any column.
 */
bool
child_paths(AggCtx *c, SiftAggSpec *s)
{
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		SiftAggJoin *j = &s->join[foreach_current_index(jc)];
		PathTarget *t = create_empty_pathtarget();
		Path	   *p;
		Size		size;

		if (jc->cpath == NULL || jc->j.scoped)
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
