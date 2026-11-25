/*
 * agg/safety.c - the guards of agg/safety.h. A relation is read through its own check of privileges and policies by PostgreSQL's scans; the aggregation reads the
 * columns of an index directly, so it is used only where no policy, barrier, parameter, cursor or partial index can make its rows differ from the rows of the normal plan.
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rls.h"
#include "utils/typcache.h"

#include "agg/jsonpath.h"
#include "agg/safety.h"
#include "agg/unique_key.h"

typedef struct RlsWalk
{
	const Query *query;			/* whose range table the entries are of: the one that holds their permission info */
} RlsWalk;

static bool query_has_rls(const Query *q);

/*
 * the role the policies of the entry apply to is the owner of the view it is read through, else the invoking one. A scan of the columns of an index refuses a table with row security
 * for the invoking role whoever owns the view (invoker_too); a read of the scope indexes alone is the view's own rows, and PostgreSQL applies the policies of the owner only.
 */
static bool
rte_has_rls(const Query *q, const RangeTblEntry *rte, bool invoker_too)
{
	Oid			view_owner = InvalidOid;

	if (rte == NULL)
		return true;
	if (rte->security_barrier || rte->securityQuals != NIL)
		return true;
	if (rte->rtekind != RTE_RELATION)
		return false;
	if (rte->perminfoindex > 0 && rte->perminfoindex <= list_length(q->rteperminfos))
		view_owner = getRTEPermissionInfo(q->rteperminfos, (RangeTblEntry *) rte)->checkAsUser;
	return check_enable_rls(rte->relid, view_owner, true) == RLS_ENABLED ||
		(invoker_too && OidIsValid(view_owner) && check_enable_rls(rte->relid, InvalidOid, true) == RLS_ENABLED);
}

static bool
rls_walker(Node *node, RlsWalk *w)
{
	if (node == NULL)
		return false;
	if (IsA(node, RangeTblEntry))
		return rte_has_rls(w->query, (RangeTblEntry *) node, true);
	if (IsA(node, Query))
		return query_has_rls((Query *) node);
	return expression_tree_walker(node, rls_walker, w);
}

static bool
query_has_rls(const Query *q)
{
	RlsWalk		w = {.query = q};

	return q->hasRowSecurity || query_tree_walker((Query *) q, rls_walker, &w, QTW_EXAMINE_RTES_BEFORE);
}

bool
agg_query_rte_has_rls(const Query *q, const RangeTblEntry *rte)
{
	return rte_has_rls(q, rte, false);
}

bool
agg_rte_has_rls(PlannerInfo *root, const RangeTblEntry *rte)
{
	return root == NULL || root->parse == NULL || rte_has_rls(root->parse, rte, true);
}

bool
agg_rel_security_ok(PlannerInfo *root, RelOptInfo *rel)
{
	if (agg_rte_has_rls(root, planner_rt_fetch(rel->relid, root)) || root->qual_security_level > 0)
		return false;
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		if (ri->security_level > 0)
			return false;
	return true;
}

bool
agg_query_has_rls(PlannerInfo *root)
{
	return root == NULL || root->parse == NULL || query_has_rls(root->parse);
}

static bool
param_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
		return ((Param *) node)->paramkind == PARAM_EXTERN;
	return expression_tree_walker(node, param_walker, context);
}

bool
agg_node_has_extern_param(const Node *node)
{
	return param_walker((Node *) node, NULL);
}

bool
agg_eflags_rescannable(int eflags)
{
	return (eflags & (EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) != 0;
}

bool
agg_index_complete(const IndexOptInfo *ii)
{
	return !ii->hypothetical && (ii->indpred == NIL || ii->predOK);
}

int
agg_index_col(const IndexOptInfo *ii, AttrNumber attno)
{
	for (int i = 0; i < ii->ncolumns; i++)
		if (ii->indexkeys[i] == attno && attno > 0)
			return i;
	return -1;
}

/*
 * The sub-plans and init plans of a statement the scan may start when it first needs them. The sub-select is run when the scan starts, also when no row would reach
 * it in the normal plan, so it must not raise, be volatile, lock rows, hold a sub-plan of its own or read a column of an outer query (a CTE scan, an initplan of the
 * outer level, a parameter of its plan). The extParam of its plan says that only after PostgreSQL has finished the whole plan, so the sub-select itself is walked,
 * the copies of it that were planned too.
 */
typedef struct OuterRef
{
	PlannerInfo *root;			/* the query the examined RTE belongs to */
	int			depth;			/* the query levels below that RTE the walk is in */
	int			skip;			/* the levels above the RTE's query whose references do not count: 0 counts the query's own */
} OuterRef;

static bool outer_ref_walker(Node *n, OuterRef *o);

/* a CTE of this query level changes with the rows of a query above it: one of the top level never does, a lateral or correlated one does */
static bool
cte_owner_correlated(PlannerInfo *owner)
{
	OuterRef	oref = {owner, 0, 0};

	return owner->parent_root != NULL && query_tree_walker(owner->parse, outer_ref_walker, &oref, QTW_EXAMINE_RTES_BEFORE);
}

/* does the tree read a column or an aggregate of a query above it, or a common table expression of an enclosing subquery (a plan per row, not a list)? */
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
		return owner == NULL || cte_owner_correlated(owner);
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
 * Functions that raise an error for some values of their input (a zero divisor, a negative root, a text that is no number); a sub-plan or init plan may raise too (more than
 * one row), where a sub-select that is an EXISTS, an IN or an ARRAY raises only through what it contains, which is walked.
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
	if (IsA(n, CoerceViaIO) || (context && (IsA(n, SubPlan) || (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC))))
		return true;
	if (context && IsA(n, SubLink) && (((SubLink *) n)->subLinkType == EXPR_SUBLINK || ((SubLink *) n)->subLinkType == ROWCOMPARE_SUBLINK))
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

static bool
subplan_runs_at_start(PlannerInfo *root, SubPlan *sp)
{
	PlannerGlobal *glob = root->glob;
	Query	   *q;
	OuterRef	oref = {root, 0, 0};

	if (sp->plan_id < 1 || sp->plan_id > list_length(glob->subroots))
		return false;
	q = ((PlannerInfo *) list_nth(glob->subroots, sp->plan_id - 1))->parse;
	return q->rowMarks == NIL && !contain_volatile_functions((Node *) q) && !raising_walker((Node *) q, root) &&
		!query_tree_walker(q, outer_ref_walker, &oref, QTW_EXAMINE_RTES_BEFORE);
}

bool
lut_subplan_ok(PlannerInfo *root, SubPlan *sp)
{
	return sp->subLinkType == ANY_SUBLINK && sp->useHashTable && sp->testexpr != NULL && sp->parParam == NIL && sp->args == NIL &&
		sp->setParam == NIL && subplan_runs_at_start(root, sp);
}

/* does the sub-select give one row at most whatever the data? aggregates with no GROUP BY give one; a table read by equalities to constants that hold a whole unique key gives one or none */
static bool
one_row_query(Query *q)
{
	RangeTblEntry *t;
	int			atts[64];
	int			n = 0;

	if (q->setOperations != NULL || q->hasTargetSRFs || q->groupingSets != NIL || q->hasWindowFuncs || q->limitOffset != NULL)
		return false;
	if (q->hasAggs)
		return q->groupClause == NIL;
	if (q->groupClause != NIL || q->distinctClause != NIL || q->jointree == NULL || list_length(q->rtable) != 1 || q->jointree->quals == NULL)
		return false;
	t = linitial_node(RangeTblEntry, q->rtable);
	if (t->rtekind != RTE_RELATION || t->relkind != RELKIND_RELATION)
		return false;
	foreach_ptr(Node, cl, IsA(q->jointree->quals, List) ? (List *) q->jointree->quals : make_ands_implicit((Expr *) q->jointree->quals))
	{
		OpExpr	   *op;
		Node	   *l,
				   *r;
		TypeCacheEntry *tc;

		if (!IsA(cl, OpExpr) || list_length((op = (OpExpr *) cl)->args) != 2)
			continue;
		l = strip_relabel(linitial(op->args));
		r = strip_relabel(lsecond(op->args));
		if (IsA(r, Var))
		{
			Node	   *x = l;

			l = r;
			r = x;
		}
		if (!IsA(l, Var) || ((Var *) l)->varlevelsup != 0 || ((Var *) l)->varattno < 1 || !IsA(r, Const) || ((Const *) r)->constisnull ||
			(tc = lookup_type_cache(((Var *) l)->vartype, TYPECACHE_EQ_OPR))->eq_opr != op->opno || exprType(r) != ((Var *) l)->vartype || n >= lengthof(atts))
			continue;
		atts[n++] = ((Var *) l)->varattno;
	}
	return n > 0 && sift_v6_unique_key(t->relid, n, 0, atts, NULL);
}

bool
initplan_ok(PlannerInfo *root, Param *prm)
{
	foreach_ptr(SubPlan, sp, root->init_plans)
		if (list_member_int(sp->setParam, prm->paramid))
		{
			Query	   *q;

			if (sp->parParam != NIL || sp->args != NIL || !subplan_runs_at_start(root, sp))
				return false;
			q = ((PlannerInfo *) list_nth(root->glob->subroots, sp->plan_id - 1))->parse;
			return sp->subLinkType == ARRAY_SUBLINK || (sp->subLinkType == EXPR_SUBLINK && one_row_query(q));
		}
	return false;
}
