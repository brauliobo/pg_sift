/*
 * agg/plan_child.c - the lookup joins whose right side is a sub-select or a function: the child plans, their correlated parameters and signatures
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
#include "catalog/pg_inherits.h"
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
#include "optimizer/tlist.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
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
#include "agg/plan_join_int.h"

/* a path whose node outputs the columns of its choice (the others need a projection above them) */
bool
projects_columns(Path *p)
{
	return IsA(p, SubqueryScanPath) || p->pathtype == T_ValuesScan || p->pathtype == T_FunctionScan || p->pathtype == T_CteScan;
}

/* the cheapest path of the rows of a sub-select, VALUES list, function or UNION ALL of sub-selects, or of the scan of a table the plan reads for its rows */
Path *
child_path(RelOptInfo *rel, bool correlated)
{
	Path	   *best = NULL;

	foreach_ptr(Path, p, rel->pathlist)
		if ((p->param_info == NULL || (correlated && IsA(p, SubqueryScanPath))) &&
			(projects_columns(p) || IsA(p, AppendPath) || IsA(p, MergeAppendPath) ||
			 (rel->rtekind == RTE_RELATION && (p->pathtype == T_SeqScan || p->pathtype == T_IndexScan || p->pathtype == T_BitmapHeapScan))) &&
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
	sift_expand_rte(fe, 1, &names, &vars);
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

/* is arg the jsonb child_arg built for the value of a member (its cast of the leaf is the stored value as a number, a JSON null the NULL of it)? */
bool
member_arg(AggCtx *c, Node *arg)
{
	return c->func_kind == FK_EACH && IsA(arg, CaseExpr) && ((CaseExpr *) arg)->casetype == JSONBOID;
}

/* The parameters of a child and what the scan gives for each. */
int			agg_child_planning;

/*
 * The planner of the sub-select a scan reads as a lookup table: its own paths (a top-N started over for each row, topn_paths) are not offered, the
 * scan calls the child plan for each row it passes and a btree on the key answers that better
 */
static PlannerInfo *
child_planner(PlannerInfo *parent, Query *q, const char *name)
{
	PlannerInfo *sub;

	agg_child_planning++;
	PG_TRY();
	{
		sub = sift_subquery_planner(parent->glob, q, parent, name);
	}
	PG_FINALLY();
	{
		agg_child_planning--;
	}
	PG_END_TRY();
	return sub;
}

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

/* is the equality `column = value` one the lookup of a key makes, with the column's and the value's types of one class? */
static bool
first_key_ok(OpExpr *op, Var *v, Oid et)
{
	Oid			lt,
				rt;
	TypeClass	tc;

	if (builtin_cmp(op->opno, &lt, &rt) != SCMP_EQ)
		return false;
	tc = type_class(lt);
	if (lt == TEXTOID)
		return rt == TEXTOID && (v->vartype == TEXTOID || v->vartype == VARCHAROID) && (et == TEXTOID || et == VARCHAROID) &&
			OidIsValid(op->inputcollid) && get_collation_isdeterministic(op->inputcollid);
	return (tc == TC_INT || tc == TC_DATE) && tc == type_class(rt) && tc == type_class(v->vartype) && tc == type_class(et);
}

/*
 * Can the lookup table test the condition on every row it loads, where the child tests it on the key's row only? Only a comparison of columns
 * of the table with constants and a null test: they cannot raise an error, so a row nobody asks for does not raise one the child never meets.
 */
static bool
first_filter_bad(Node *n, void *context)
{
	Oid			lt,
				rt;

	if (n == NULL)
		return false;
	switch (nodeTag(n))
	{
		case T_Var:
			return ((Var *) n)->varno != 1 || ((Var *) n)->varlevelsup != 0 || ((Var *) n)->varattno <= 0;
		case T_Const:
		case T_RelabelType:
		case T_BoolExpr:
		case T_NullTest:
			break;
		case T_OpExpr:
			if (builtin_cmp(((OpExpr *) n)->opno, &lt, &rt) < 0 || (type_class(lt) == TC_NONE && lt != TEXTOID) || (type_class(rt) == TC_NONE && rt != TEXTOID))
				return true;
			break;
		default:
			return true;
	}
	return expression_tree_walker(n, first_filter_bad, context);
}

/*
 * The ORDER BY of a first-row child: columns of the table (integer, date, timestamp) in either direction, which put the rows of a key in an order
 * that holds no tie when a unique index holds the key and these columns.
 */
static bool
first_order(Query *q, SiftAggFirst *f)
{
	foreach_node(SortGroupClause, sgc, q->sortClause)
	{
		Var		   *v = (Var *) strip_relabel((Node *) get_sortgroupclause_tle(sgc, q->targetList)->expr);
		Oid			lt,
					rt;
		int			cmp = builtin_cmp(sgc->sortop, &lt, &rt);

		if (f->nord >= sift_limit(first_order_cols, SIFT_AGG_MAX_FORD) || !IsA(v, Var) || v->varno != 1 || v->varlevelsup != 0 || v->varattno <= 0 ||
			(cmp != SCMP_LT && cmp != SCMP_GT) || lt != v->vartype || rt != v->vartype || v->vartype == BOOLOID ||
			!sift_aggspec_integer_type(v->vartype))
			return false;
		f->ordatt[f->nord] = v->varattno;
		f->ordtype[f->nord] = v->vartype;
		f->orddesc[f->nord] = cmp == SCMP_GT;
		f->ordnullsfirst[f->nord++] = sgc->nulls_first;
	}
	return true;
}

/*
 * `SELECT columns FROM table WHERE column = parameter [AND ...] LIMIT 1` over a table that holds each key once (a unique index on columns
 * among those compared) returns the key's row, whatever plan it has: the scan can read the table as a lookup table (agg/first.c). Anything
 * else the query does (an order, a second table, an offset, a row lock, a condition that is not a comparison with a constant or a null test)
 * leaves the child as it is.
 */
static void
first_lookup(AggCtx *c, JoinCtx *jc)
{
	Query	   *q = jc->subroot->parse;
	SiftAggFirst *f = &jc->j.first;
	Const	   *lim = (Const *) q->limitCount;
	RangeTblEntry *rte;
	RelOptInfo *brel;
	List	   *conj;
	int			atts[SIFT_AGG_MAX_JKEYS + SIFT_AGG_MAX_FORD];

	memset(f, 0, sizeof(*f));
	if (jc->j.scoped || q->commandType != CMD_SELECT || q->setOperations != NULL || q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs ||
		q->hasSubLinks || q->hasForUpdate || q->hasRecursive || q->groupClause != NIL || q->groupingSets != NIL || q->havingQual != NULL ||
		q->distinctClause != NIL || q->cteList != NIL || q->rowMarks != NIL || q->limitOffset != NULL ||
		q->limitOption != LIMIT_OPTION_COUNT || lim == NULL || !IsA(lim, Const) || lim->constisnull || lim->consttype != INT8OID ||
		DatumGetInt64(lim->constvalue) != 1 || list_length(q->rtable) != 1 || list_length(q->targetList) > SIFT_AGG_MAX_FOUT ||
		q->jointree == NULL || list_length(q->jointree->fromlist) != 1 || q->jointree->quals == NULL)
		return;
	if (!IsA(linitial(q->jointree->fromlist), RangeTblRef) || ((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex != 1)
		return;
	rte = linitial_node(RangeTblEntry, q->rtable);
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION || rte->tablesample != NULL || rte->securityQuals != NIL ||
		has_subclass(rte->relid) || (brel = jc->subroot->simple_rel_array[1]) == NULL || !agg_lookup_fits(brel->tuples, brel->reltarget->width))
		return;
	conj = IsA(q->jointree->quals, List) ? (List *) q->jointree->quals : list_make1(q->jointree->quals);
	foreach_ptr(Node, qual, conj)
	{
		OpExpr	   *op = (OpExpr *) qual;
		Node	   *l,
				   *r;
		Var		   *v;
		Param	   *p;
		int			pos = -1;

		if (!first_filter_bad(qual, NULL))
		{
			f->qual = lappend(f->qual, qual);
			continue;
		}
		if (!IsA(qual, OpExpr) || list_length(op->args) != 2 || f->nkey == SIFT_AGG_MAX_JKEYS)
			return;
		l = strip_relabel(linitial(op->args));
		r = strip_relabel(lsecond(op->args));
		if (IsA(l, Var) && IsA(r, Param))
		{
			v = (Var *) l;
			p = (Param *) r;
		}
		else if (IsA(r, Var) && IsA(l, Param))
		{
			v = (Var *) r;
			p = (Param *) l;
		}
		else
			return;
		if (v->varno != 1 || v->varlevelsup != 0 || v->varattno <= 0 || p->paramkind != PARAM_EXEC)
			return;
		for (int i = 0; i < f->nkey; i++)
			if (f->keyatt[i] == v->varattno)
				return;
		for (int i = 0; i < list_length(jc->j.params); i++)
			if (list_nth_int(jc->j.params, i) == p->paramid)
				pos = i;
		if (pos < 0 || contain_volatile_functions(list_nth(jc->args, pos)) || !first_key_ok(op, v, exprType(strip_relabel(list_nth(jc->args, pos)))))
			return;
		f->keyatt[f->nkey] = v->varattno;
		f->keytype[f->nkey] = v->vartype;
		f->keyarg[f->nkey++] = pos;
	}
	if (!first_order(q, f))
		return;
	for (int k = 0; k < f->nkey; k++)
		atts[k] = f->keyatt[k];
	for (int k = 0; k < f->nord; k++)
		atts[f->nkey + k] = f->ordatt[k];
	if (!sift_agg_unique_key(rte->relid, f->nkey, f->nord, atts, NULL))
	{
		if (sift_explain_refusals)
		{
			StringInfoData cols;

			initStringInfo(&cols);
			for (int k = 0; k < f->nkey + f->nord; k++)
				appendStringInfo(&cols, "%s%s", k ? ", " : "", get_attname(rte->relid, atts[k], false));
			agg_report("lookup", AGG_REFUSE_LATERAL_LIMIT,
					   psprintf("no unique index on %s: end the ORDER BY with the primary key, or create a unique index on those columns, a partial one with WHERE col IS NOT NULL for a "
								"sub-select with that condition (an index on an expression of them does not count); CLUSTER on it and VACUUM keep the lookups on adjacent pages", cols.data),
					   false, 0, 0);
		}
		return;
	}
	foreach_node(TargetEntry, te, q->targetList)
	{
		Var		   *v = (Var *) te->expr;

		if (te->resjunk)
			continue;
		if (te->resno > SIFT_AGG_MAX_FOUT)
			return;
		f->col[te->resno - 1] = IsA(v, Var) && v->varno == 1 && v->varlevelsup == 0 && v->varattno > 0 ? v->varattno : 0;
		f->ncol = Max(f->ncol, te->resno);
	}
	f->relid = rte->relid;
	f->ok = true;
	jc->frows = Max(brel->tuples, 1.0);
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
	jc->subroot = child_planner(c->root, q, e->eref->aliasname);
	child_params(c, jc, c->root->plan_params);
	c->root->plan_params = saved;
	if (jc->j.params == NIL)
		return false;
	jc->j.sig = sift_agg_child_sig((Node *) q, jc->j.params);
	child_scan_path(rel, jc);
	if (e->rtekind == RTE_SUBQUERY && x.calls == NIL)
		first_lookup(c, jc);
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
	jc->j.sig = sift_agg_child_sig((Node *) lifted, x.params);
	jc->subroot = child_planner(c->root, lifted, jc->j.desc);
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

	return agg_lookup_fits(jc->cpath->rows * Min(estimate_num_groups(c->root, jc->args, reach, NULL, NULL), reach), jc->cpath->pathtarget->width);
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

/* the plan id of the common table expression a CTE scan of `root` reads; 0 if it is not one with a plan of its own */
static int
cte_plan_id(PlannerInfo *root, RangeTblEntry *e)
{
	PlannerInfo *owner = root;
	int			ndx = 0;

	for (Index up = e->ctelevelsup; up > 0 && owner != NULL; up--)
		owner = owner->parent_root;
	if (owner == NULL)
		return 0;
	foreach_node(CommonTableExpr, x, owner->parse->cteList)
	{
		if (strcmp(x->ctename, e->ctename) == 0)
			return ndx < list_length(owner->cte_plan_ids) ? list_nth_int(owner->cte_plan_ids, ndx) : 0;
		ndx++;
	}
	return 0;
}

/* the columns of the child as the first table of any query */
static Node *
own_rel_mutator(Node *n, Index *rti)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((Var *) n)->varno == *rti && ((Var *) n)->varlevelsup == 0)
	{
		Var		   *v = copyObject((Var *) n);

		v->varno = v->varnosyn = 1;
		return (Node *) v;
	}
	return expression_tree_mutator(n, own_rel_mutator, rti);
}

/*
 * What an uncorrelated child plan is, for the arms of a statement to know theirs are the same: the common table expression (the plan the
 * statement made of it) or the VALUES list, and the conditions the scan keeps on its rows. A sub-select or function keeps no identity: the
 * columns of its plan the arms do not read are NULL.
 */
Node *
plain_child_sig(AggCtx *c, RangeTblEntry *e, Index rti, RelOptInfo *rel)
{
	Node	   *what;
	int			id;

	if (e->rtekind == RTE_VALUES)
		what = (Node *) e->values_lists;
	else if (e->rtekind == RTE_CTE && (id = cte_plan_id(c->root, e)) > 0)
		what = (Node *) list_make1(makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(id), false, true));
	else
		return NULL;
	return sift_agg_child_sig(own_rel_mutator((Node *) list_make2(what, extract_actual_clauses(rel->baserestrictinfo, false)), &rti), NIL);
}

/* the plan of a correlated child, made once for the query: the other table of a flipped join reads the same child */
bool
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
			jc->j.first = done->j.first;
			jc->frows = done->frows;
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
bool
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
