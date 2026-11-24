/*
 * col/f2/f2_v6_scope.c - the planning of the scope aggregate of SiftAggV6: `facts f JOIN pairs s ON s.member = f.fk WHERE s.key = K` with count, sum, avg, min and max of the integer, float8 and
 * numeric leaves of the summary the facts keep for the scope (col/f2/f2_scope.h), a sift_col index of format major 2 on the facts or the facts themselves when they are a sift_table of it, is the cube of the key K, read without a row of the facts: one tuple for the key, or for
 * every key of the summary when the statement groups by the key without naming one, or for every month of it when it groups by date_trunc('month') of the time leaf. The facts may be limited
 * to a window of whole months of the time leaf and to the rows whose soft-delete leaf is NULL (the rows it holds a value in leave the cubes with the rows of the deletion vectors); the pairs may
 * be the relation itself or a single-table sub-select or materialized CTE over it. The executor (scope_v6_run.c) reads the summary when it counts what the heap shows the snapshot, and runs the
 * join as the planner priced it, the child of the node, when it does not.
 */
/* check-memory: allow-allocs every array here follows the aggregates of one statement and the keys of one summary */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_opfamily_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

#include "agg/agg.h"
#include "agg/v6/v6_safety.h"
#include "agg/spec/spec.h"
#include "agg/v6/v6_card.h"
#include "col/f2/f2_v6_scope_int.h"
#include "agg/v6/v6_settings.h"
#include "col/f2/f2_v6_scope_match.h"
#include "col/am.h"
#include "col/am/format_ops.h"
#include "col/col.h"
#include "col/f2/f2_scope.h"
#include "col/f2/f2_store.h"
#include "col/scope.h"
#include "table/dispatch.h"
#include "core/catname.h"

static const CustomPathMethods scope_path_methods = {"SiftAggV6Scope", sift_agg_v6_scope_plan_path, NULL};

/* The attribute of the pairs relation a Var of the pairs side is: its own, or the one the sub-select or CTE takes it from */
static AttrNumber
pairs_attno(const ScopePlan *p, const Var *v)
{
	if (v->varno != p->pairs->relid || v->varlevelsup != 0)
		return InvalidAttrNumber;
	if (p->pairs_query != NULL)
	{
		const TargetEntry *te = v->varattno >= 1 && v->varattno <= list_length(p->pairs_query->targetList) ? list_nth(p->pairs_query->targetList, v->varattno - 1) : NULL;

		return te != NULL && IsA(te->expr, Var) ? ((Var *) te->expr)->varattno : InvalidAttrNumber;
	}
	return v->varattno;
}

static bool
is_key_var(const ScopePlan *p, const Node *n)
{
	return IsA(n, Var) && pairs_attno(p, (const Var *) n) == p->leg.key;
}

/* The column of the relation that keeps the summary a Var of the facts is: the attribute of a table, the position in the index of an index */
int
sift_v6_scope_index_col(const ScopePlan *p, const Var *v)
{
	if (v->varno != p->fact->relid || v->varlevelsup != 0)
		return -1;
	if (p->index == NULL)
		return v->varattno > 0 ? v->varattno : -1;
	for (int i = 0; i < p->index->ncolumns; i++)
		if (p->index->indexkeys[i] == v->varattno)
			return i + 1;
	return -1;
}

static AttrNumber
fact_attno(const ScopePlan *p, int col)
{
	return p->index == NULL ? (AttrNumber) col : p->index->indexkeys[col - 1];
}

static Oid
opfamily_of(const ScopePlan *p, int col)
{
	return p->index == NULL ? sift_f2_store_open(p->irel)->opfamily[col] : p->index->opfamily[col - 1];
}

static int
measure_of(const ScopePlan *p, const Var *v)
{
	const int	col = sift_v6_scope_index_col(p, v);

	for (int m = 0; col > 0 && m < p->layout.nmeas; m++)
		if (p->layout.meas_col[m] == col)
			return m;
	return -1;
}

static bool
is_int(Oid typid)
{
	return typid == INT2OID || typid == INT4OID || typid == INT8OID;
}

/* The columns of the tuple the node leaves are those of its scan tlist, in the order the statement names them */
static void
add_col(ScopePlan *p, Expr *e, int what)
{
	foreach_node(TargetEntry, te, p->scan_tlist)
		if (equal(te->expr, e))
			return;
	p->scan_tlist = lappend(p->scan_tlist, makeTargetEntry(copyObject(e), list_length(p->scan_tlist) + 1, NULL, false));
	p->spec.col[p->spec.ncols++] = what;
}

static bool
compile_agg(ScopePlan *p, Aggref *a)
{
	SiftV6Agg	g = {.restype = a->aggtype};
	const Expr *arg;
	int			m;

	if (!sift_v6_agg_kind(a, &g.kind) || sift_v6_kind_distinct(g.kind) || p->spec.ncols == SCOPE_COLS_MAX)
		return false;
	if (g.kind != SV6_COUNT_STAR)
	{
		arg = list_length(a->args) == 1 ? ((TargetEntry *) linitial(a->args))->expr : NULL;
		if (arg == NULL || !IsA(arg, Var) || (m = measure_of(p, (const Var *) arg)) < 0)
			return false;
		g.arg = (SiftV6Col) {.attno = m,.typid = ((const Var *) arg)->vartype,.scale = p->layout.meas_scale[m]};
		if (g.kind != SV6_COUNT && !is_int(g.arg.typid) && g.arg.typid != NUMERICOID && !(g.arg.typid == FLOAT8OID && !sift_agg_exact_float))
			return false;
	}
	if (p->spec.naggs == p->cap_aggs)
	{
		p->cap_aggs = p->cap_aggs == 0 ? 16 : p->cap_aggs * 2;
		p->spec.agg = p->spec.agg ? repalloc(p->spec.agg, (Size) p->cap_aggs * sizeof(SiftV6Agg)) : palloc((Size) p->cap_aggs * sizeof(SiftV6Agg));
	}
	p->spec.agg[p->spec.naggs] = g;
	add_col(p, (Expr *) a, p->spec.naggs++);
	return true;
}

/* The expressions of the statement that are not aggregates are the columns it groups by (or the key it names) */
static bool
compile_walk(Node *n, ScopePlan *p)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
		return !compile_agg(p, (Aggref *) n);
	if (is_key_var(p, n) && (p->spec.by_key || p->spec.key_given))
	{
		add_col(p, (Expr *) n, SCOPE_COL_KEY);
		return false;
	}
	if (p->month_expr != NULL && equal(n, p->month_expr))
	{
		add_col(p, (Expr *) n, SCOPE_COL_MONTH);
		return false;
	}
	if (IsA(n, Var))
		return true;
	return expression_tree_walker(n, compile_walk, p);
}

static bool
compile_exprs(ScopePlan *p, Node *e)
{
	return !compile_walk(e, p);
}

/* key = constant or parameter, on the pairs relation or in the sub-select that is the pairs */
static bool
key_operand(const ScopePlan *p, OpExpr *op, bool inner, Expr **rhs)
{
	Node	   *l,
			   *r;

	if (list_length(op->args) != 2 || !sift_v6_key_equality(p->spec.key_type, op->opno, op->inputcollid))
		return false;
	l = sift_v6_strip_relabel(linitial(op->args));
	r = sift_v6_strip_relabel(lsecond(op->args));
	if (!(inner ? IsA(l, Var) && ((Var *) l)->varattno == p->leg.key : is_key_var(p, l)) || !(IsA(r, Const) ? !((Const *) r)->constisnull : IsA(r, Param)) ||
		!sift_v6_key_arg_ok(p->spec.key_type, exprType(r)))
		return false;
	*rhs = (Expr *) r;
	return true;
}

/* key = ANY(array of integers), an array constant or a parameter: IN (K1, K2, ...) */
static bool
key_array_operand(const ScopePlan *p, ScalarArrayOpExpr *sa, bool inner, Expr **rhs)
{
	Node	   *l,
			   *r;

	if (!sa->useOr || list_length(sa->args) != 2 || !sift_v6_key_equality(p->spec.key_type, sa->opno, sa->inputcollid))
		return false;
	l = sift_v6_strip_relabel(linitial(sa->args));
	r = sift_v6_strip_relabel(lsecond(sa->args));
	if (!(inner ? IsA(l, Var) && ((Var *) l)->varattno == p->leg.key : is_key_var(p, l)) || !(IsA(r, Const) ? !((Const *) r)->constisnull : IsA(r, Param)) ||
		!sift_v6_key_arg_ok(p->spec.key_type, get_element_type(exprType(r))))
		return false;
	*rhs = (Expr *) r;
	return true;
}

static bool
key_clause(const ScopePlan *p, Node *clause, bool inner, Expr **rhs)
{
	if (IsA(clause, OpExpr))
		return key_operand(p, (OpExpr *) clause, inner, rhs);
	return IsA(clause, ScalarArrayOpExpr) && key_array_operand(p, (ScalarArrayOpExpr *) clause, inner, rhs);
}

/* The conditions of the statement on the pairs besides the key are the predicate of the unique btree that makes the pairs, no more and no less: it reads exactly the rows that are pairs */
static bool
pairs_predicate_ok(const ScopePlan *p, List *rest, bool inner)
{
	List	   *pred = copyObject(p->leg.pred);

	if (pred == NIL || rest == NIL)
		return pred == rest;
	ChangeVarNodes((Node *) pred, 1, inner ? 1 : p->pairs->relid, 0);
	return predicate_implied_by(pred, rest, false) && predicate_implied_by(rest, pred, false);
}

/* The conditions on the pairs are the key = K or key IN (K1, K2, ...), from the statement or from the sub-select, and the predicate of the pairs; no key: every key (only a statement that groups by the key) */
static bool
compile_key_filter(ScopePlan *p)
{
	List	   *outer = p->pairs->baserestrictinfo;
	List	   *inner = p->pairs_query != NULL && p->pairs_query->jointree->quals != NULL ? make_ands_implicit((Expr *) p->pairs_query->jointree->quals) : NIL;
	List	   *rest_outer = NIL,
			   *rest_inner = NIL;
	Expr	   *rhs = NULL;
	int			nkeys = 0;

	foreach_node(RestrictInfo, ri, outer)
		if (key_clause(p, (Node *) ri->clause, false, &rhs))
			nkeys++;
		else
			rest_outer = lappend(rest_outer, ri->clause);
	foreach_ptr(Node, q, inner)
		if (key_clause(p, q, true, &rhs))
			nkeys++;
		else
			rest_inner = lappend(rest_inner, q);
	if (nkeys > 1 || (p->pairs_query != NULL ? rest_outer != NIL || !pairs_predicate_ok(p, rest_inner, true) : !pairs_predicate_ok(p, rest_outer, false)))
		return false;
	if (rhs == NULL)
		return p->spec.by_key;
	p->key_expr = rhs;
	p->spec.key_given = true;
	p->spec.key_array = type_is_array(exprType((Node *) rhs));
	return true;
}

static bool
time_type_ok(Oid typid)
{
	return typid == TIMESTAMPTZOID || typid == TIMESTAMPOID || typid == DATEOID;
}

/* time >= lo / time > lo and time < hi / time <= hi, each once, on constants or parameters of the leaf's own type: the window the months of the cubes answer */
static bool
compile_window_qual(ScopePlan *p, OpExpr *op)
{
	Node	   *l = list_length(op->args) == 2 ? linitial(op->args) : NULL;
	Node	   *r = l ? lsecond(op->args) : NULL;
	Oid			opno = op->opno;
	int			strat;

	if (l == NULL)
		return false;
	if ((IsA(l, Const) || IsA(l, Param)) && IsA(r, Var))
	{
		Node	   *t = l;

		l = r;
		r = t;
		opno = get_commutator(opno);
	}
	if (!OidIsValid(opno) || !IsA(l, Var) || !(IsA(r, Const) ? !((Const *) r)->constisnull : IsA(r, Param)) || !time_type_ok(((Var *) l)->vartype) ||
		exprType(r) != ((Var *) l)->vartype || p->layout.time_col == 0 || sift_v6_scope_index_col(p, (Var *) l) != p->layout.time_col)
		return false;
	strat = get_op_opfamily_strategy(opno, opfamily_of(p, p->layout.time_col));
	p->spec.time_type = ((Var *) l)->vartype;
	if ((strat == BTGreaterEqualStrategyNumber || strat == BTGreaterStrategyNumber) && p->lo_expr == NULL)
	{
		p->lo_expr = (Expr *) r;
		p->spec.lo_strat = strat;
		return true;
	}
	if ((strat == BTLessStrategyNumber || strat == BTLessEqualStrategyNumber) && p->hi_expr == NULL)
	{
		p->hi_expr = (Expr *) r;
		p->spec.hi_strat = strat;
		return true;
	}
	return false;
}

/* `leaf IS NULL`: the soft-delete test. The rows it holds a value in are the entity side's to count out of the cubes */
static bool
compile_soft_delete(ScopePlan *p, NullTest *nt)
{
	const int	col = IsA(nt->arg, Var) && !nt->argisrow ? sift_v6_scope_index_col(p, (Var *) nt->arg) : -1;

	if (nt->nulltesttype != IS_NULL || col < 1 || p->spec.excl_col != 0 || !sift_f2_scope_excl_ok(p->irel, p->spec.ord, col))
		return false;
	p->spec.excl_col = col;
	return true;
}

static bool
compile_fact_qual(ScopePlan *p, Expr *clause)
{
	if (IsA(clause, NullTest))
		return compile_soft_delete(p, (NullTest *) clause);
	return IsA(clause, OpExpr) && compile_window_qual(p, (OpExpr *) clause);
}

static bool
compile_fact_quals(ScopePlan *p)
{
	if (!agg_rel_security_ok(p->root, p->fact))
		return false;
	foreach_node(RestrictInfo, ri, p->fact->baserestrictinfo)
		if (ri->pseudoconstant || !compile_fact_qual(p, ri->clause))
			return false;
	p->spec.ranged = p->lo_expr != NULL || p->hi_expr != NULL;
	return !p->spec.ranged || (p->lo_expr != NULL && p->hi_expr != NULL) || p->spec.by_month;
}

/* date_trunc('month', time leaf): the leaf a timestamp or a timestamptz, or a date the way PostgreSQL casts it to one of them */
static bool
month_group(ScopePlan *p, Node *g)
{
	const FuncExpr *fe = IsA(g, FuncExpr) ? (const FuncExpr *) g : NULL;
	const Node *arg;
	const Const *unit;

	if (fe == NULL || p->layout.time_col == 0 || p->month_expr != NULL || get_func_namespace(fe->funcid) != PG_CATALOG_NAMESPACE || strcmp(sift_func_name(fe->funcid), "date_trunc") != 0 ||
		list_length(fe->args) != 2 || (fe->funcresulttype != TIMESTAMPTZOID && fe->funcresulttype != TIMESTAMPOID))
		return false;
	unit = linitial(fe->args);
	arg = lsecond(fe->args);
	if (!IsA(unit, Const) || unit->constisnull || strcmp(TextDatumGetCString(unit->constvalue), "month") != 0)
		return false;
	if (IsA(arg, FuncExpr) && list_length(((FuncExpr *) arg)->args) == 1 && exprType(linitial(((FuncExpr *) arg)->args)) == DATEOID)
		arg = linitial(((FuncExpr *) arg)->args);
	if (!IsA(arg, Var) || sift_v6_scope_index_col(p, (Var *) arg) != p->layout.time_col || !time_type_ok(((Var *) arg)->vartype))
		return false;
	p->month_expr = (Expr *) g;
	p->spec.by_month = true;
	p->spec.month_type = fe->funcresulttype;
	p->spec.time_type = ((Var *) arg)->vartype;
	return true;
}

/* The key and the month, once each; the columns PostgreSQL left out of the grouping because the key is a constant have none */
static bool
compile_groups(ScopePlan *p)
{
	List	   *tlist = p->root->parse->targetList;

	foreach_node(SortGroupClause, sgc, p->root->processed_groupClause)
	{
		Node	   *g = get_sortgroupclause_expr(sgc, tlist);

		if (is_key_var(p, g) && !p->spec.by_key)
		{
			p->spec.by_key = true;
			add_col(p, (Expr *) g, SCOPE_COL_KEY);
		}
		else if (month_group(p, g))
			add_col(p, (Expr *) g, SCOPE_COL_MONTH);
		else
			return false;
	}
	p->spec.grouped_const = p->root->processed_groupClause == NIL && p->root->parse->groupClause != NIL;
	return true;
}

/* The join the node falls back to leaves its columns as the statement names them: the scan tlist follows that order (what only the HAVING names comes last) */
static void
order_like_target(ScopePlan *p, const PathTarget *target)
{
	List	   *sorted = NIL;
	bool		taken[SCOPE_COLS_MAX] = {false};
	int			col[SCOPE_COLS_MAX],
				n = 0;

	foreach_ptr(Expr, e, target->exprs)
		for (int i = 0; i < p->spec.ncols; i++)
			if (equal(((TargetEntry *) list_nth(p->scan_tlist, i))->expr, e))
			{
				if (!taken[i])
				{
					taken[i] = true;
					col[n++] = p->spec.col[i];
					sorted = lappend(sorted, list_nth(p->scan_tlist, i));
				}
				break;
			}
	for (int i = 0; i < p->spec.ncols; i++)
		if (!taken[i])
		{
			col[n++] = p->spec.col[i];
			sorted = lappend(sorted, list_nth(p->scan_tlist, i));
		}
	for (int i = 0; i < n; i++)
		((TargetEntry *) list_nth(sorted, i))->resno = i + 1;
	memcpy(p->spec.col, col, n * sizeof(int));
	p->scan_tlist = sorted;
}

static bool
compile_target(ScopePlan *p, GroupPathExtraData *extra, RelOptInfo *out)
{
	if (!(compile_groups(p) && compile_key_filter(p) && compile_fact_quals(p) && (p->spec.by_key || p->spec.key_given) && (!p->spec.grouped_const || p->spec.key_given) &&
		  (!p->spec.key_array || p->spec.by_key || !p->spec.by_month) &&
		  compile_exprs(p, (Node *) out->reltarget->exprs) && (extra->havingQual == NULL || compile_exprs(p, extra->havingQual))))
		return false;
	order_like_target(p, out->reltarget);
	return true;
}

/* s.member = f.fk and nothing else joins the two: the equivalence class of the two columns has no other member */
static bool
joined_by_member(const ScopePlan *p, AttrNumber fk)
{
	foreach_node(EquivalenceClass, ec, p->root->eq_classes)
	{
		bool		has_fact = false,
					has_pairs = false;

		if (ec->ec_has_const || ec->ec_has_volatile || list_length(ec->ec_members) != 2)
			continue;
		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			const Var  *v = (const Var *) sift_v6_strip_relabel((Node *) em->em_expr);

			if (!IsA(v, Var))
				break;
			has_fact |= v->varno == p->fact->relid && v->varattno == fk;
			has_pairs |= pairs_attno(p, v) == p->leg.member;
		}
		if (has_fact && has_pairs)
			return true;
	}
	return false;
}

bool
sift_v6_plain_table(const RangeTblEntry *rte)
{
	return rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && !rte->inh && rte->tablesample == NULL && rte->securityQuals == NIL;
}

static bool
plain_relation(PlannerInfo *root, RelOptInfo *rel)
{
	return rel != NULL && rel->reloptkind == RELOPT_BASEREL && sift_v6_plain_table(planner_rt_fetch(rel->relid, root)) && rel->lateral_relids == NULL && rel->joininfo == NIL;
}

/* One table, its rows by one condition, its columns as they are: no row it says is another's (a security qual, a limit, a grouping, a second relation) */
static bool
single_table_query(const Query *q)
{
	if (q->commandType != CMD_SELECT || q->hasAggs || q->hasWindowFuncs || q->hasSubLinks || q->hasTargetSRFs || q->hasRowSecurity || q->groupClause != NIL || q->distinctClause != NIL ||
		q->sortClause != NIL || q->limitCount != NULL || q->limitOffset != NULL || q->setOperations != NULL || q->cteList != NIL || q->rowMarks != NIL || q->havingQual != NULL ||
		list_length(q->rtable) != 1 || list_length(q->jointree->fromlist) != 1 || !sift_v6_plain_table(linitial(q->rtable)) || !IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return false;
	foreach_node(TargetEntry, te, q->targetList)
		if (te->resjunk || !IsA(te->expr, Var) || ((Var *) te->expr)->varno != 1)
			return false;
	return true;
}

/* The pairs side that is a sub-select or a materialized CTE over one table: the query it computes */
static Query *
pairs_subquery(PlannerInfo *root, const RelOptInfo *rel)
{
	const RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	Query	   *q = NULL;

	if (rte->rtekind == RTE_SUBQUERY)
		q = rte->subquery;
	else if (rte->rtekind == RTE_CTE && !rte->self_reference && rte->ctelevelsup == 0)
		foreach_node(CommonTableExpr, cte, root->parse->cteList)
			if (strcmp(cte->ctename, rte->ctename) == 0)
				q = castNode(Query, cte->ctequery);
	return q;
}

/* The statement joins the two relations by one join, or by the semi join of IN or EXISTS: a member of the pairs of one key is there once, so a semi join keeps the rows an inner join does */
static bool
pairs_joined(ScopePlan *p)
{
	p->semi = p->root->join_info_list != NIL;
	return !p->semi || sift_v6_semi_joins(p->root, p->pairs->relid);
}

/* A semi join names no key of the pairs and counts a member once: one key and not an array of them */
static bool
semi_ok(const ScopePlan *p)
{
	return !p->semi || (p->spec.key_given && !p->spec.key_array && !p->spec.by_key);
}

static bool
pairs_ok(ScopePlan *p)
{
	RelOptInfo *rel = p->pairs;

	if (rel == NULL || rel->reloptkind != RELOPT_BASEREL || rel->lateral_relids != NULL || rel->joininfo != NIL || !agg_rel_security_ok(p->root, rel))
		return false;
	if (sift_v6_plain_table(planner_rt_fetch(rel->relid, p->root)))
		return true;
	p->pairs_query = pairs_subquery(p->root, rel);
	if (p->pairs_query != NULL && !single_table_query(p->pairs_query))
	{
		p->pairs_raw = rel->subroot != NULL ? rel->subroot->parse : p->pairs_query;
		p->pairs_query = NULL;
	}
	return p->pairs_query != NULL || p->pairs_raw != NULL;
}

static Oid
pairs_table(const ScopePlan *p)
{
	return p->pairs_query != NULL ? ((RangeTblEntry *) linitial(p->pairs_query->rtable))->relid : planner_rt_fetch(p->pairs->relid, p->root)->relid;
}

static bool
shape_ok(PlannerInfo *root, RelOptInfo *input_rel)
{
	Query	   *parse = root->parse;

	return sift_enable_agg && parse->hasAggs && !parse->hasRowSecurity && root->qual_security_level == 0 && !parse->hasDistinctOn && !parse->hasTargetSRFs && !parse->hasWindowFuncs &&
		parse->groupingSets == NIL && list_length(root->join_info_list) <= 1 && bms_num_members(input_rel->relids) == 2;
}

/* Does scope ord of the index summarize the pairs relation: one path, unique pairs, every row of the relation a pair */
static bool
scope_matches(ScopePlan *p, int ord)
{
	SiftScopeLeg legs[2];
	const int	nlegs = sift_scope_legs(p->irel, ord, legs);

	if (p->pairs_raw != NULL)
		p->pairs_query = nlegs == 2 ? sift_v6_match_two_paths(p->pairs_raw, legs) : NULL;
	if (nlegs != (p->pairs_raw != NULL ? 2 : 1) || (p->pairs_raw != NULL && p->pairs_query == NULL) || legs[0].rel != pairs_table(p) ||
		sift_scope_pairs_multiset(p->irel, sift_scope_specs(p->irel)->s[ord]) || !sift_f2_scope_layout(p->irel, ord, &p->layout))
		return false;
	p->leg = legs[0];
	p->spec.key_type = get_atttype(p->leg.rel, p->leg.key);
	return (sift_scope_word_type(p->spec.key_type) || sift_keymap_type(p->spec.key_type)) && joined_by_member(p, fact_attno(p, p->layout.member_col));
}

static Relation
scope_rel_open(const ScopePlan *p)
{
	return relation_open(p->index == NULL ? planner_rt_fetch(p->fact->relid, p->root)->relid : p->index->indexoid, AccessShareLock);
}

static void
reset_spec(ScopePlan *p, int ord)
{
	p->spec = (ScopeSpec) {.relid = RelationGetRelid(p->irel),.is_table = p->index == NULL,.ord = ord,.key_type = p->spec.key_type};
	p->cap_aggs = 0;
	p->key_expr = p->lo_expr = p->hi_expr = p->month_expr = NULL;
	p->scan_tlist = NIL;
}

/* What no cube tells (a qual on a leaf of the facts, a key of a document) is the scan of the facts that are the members of the key K of the pairs */
static bool
compile_scan(ScopePlan *p, GroupPathExtraData *extra, RelOptInfo *out)
{
	if (!compile_key_filter(p) || !p->spec.key_given || p->spec.key_array)
		return false;
	p->spec = (ScopeSpec) {.relid = p->spec.relid,.is_table = p->spec.is_table,.ord = p->spec.ord,.key_type = p->spec.key_type,.key_given = true,.scan = true,.member_col = p->layout.member_col};
	return sift_v6_compile_fact(p->root, p->fact, out, p->index, extra, &p->v6, &p->v6_tlist);
}

static bool
compile_on(ScopePlan *p, GroupPathExtraData *extra, RelOptInfo *out)
{
	bool		ok = false;

	p->irel = scope_rel_open(p);
	for (int ord = 0; ord < sift_f2_nscopes(p->irel) && !ok; ord++)
	{
		reset_spec(p, ord);
		ok = scope_matches(p, ord) && (compile_target(p, extra, out) || (p->index != NULL && (reset_spec(p, ord), compile_scan(p, extra, out)))) && semi_ok(p);
	}
	relation_close(p->irel, NoLock);
	return ok;
}

/* A sift_table of format major 2 that declares scopes: its own versions decide the rows, so nothing is asked of track_changes or of a stale flag */
static bool
table_usable(PlannerInfo *root, const RelOptInfo *fact)
{
	Oid			relid = planner_rt_fetch(fact->relid, root)->relid;
	Relation	rel;
	bool		ok = sift_tbl_is_f2_oid(relid);

	if (!ok)
		return false;
	rel = table_open(relid, AccessShareLock);
	ok = sift_f2_scoped(rel);
	table_close(rel, NoLock);
	return ok;
}

static bool
index_usable(const IndexOptInfo *ix)
{
	bool		ok = ix->indpred == NIL && !ix->hypothetical && sift_col_index_oid_engine(ix->indexoid) == sift_col_v2_ops.format_major;
	Relation	irel;

	if (!ok)
		return false;
	irel = index_open(ix->indexoid, AccessShareLock);
	ok = sift_f2_scoped(irel) && sift_col_rel(irel)->track && !sift_col_rel(irel)->stale;
	index_close(irel, NoLock);
	return ok;
}

/* The expressions the node evaluates when it starts, in this order when they are there: the key, the lower bound, the upper bound */
static List *
scope_exprs(const ScopePlan *p)
{
	List	   *l = NIL;

	if (p->key_expr != NULL)
		l = lappend(l, p->key_expr);
	if (p->lo_expr != NULL)
		l = lappend(l, p->lo_expr);
	if (p->hi_expr != NULL)
		l = lappend(l, p->hi_expr);
	return l;
}

#define SCOPE_TOPK_MAX	1000

static bool
topk_shape_ok(PlannerInfo *root, RelOptInfo *input_rel)
{
	const Query *q = root->parse;
	const bool	plain = !q->hasAggs && q->groupClause == NIL && q->groupingSets == NIL && !q->hasWindowFuncs && !q->hasDistinctOn && q->distinctClause == NIL && !q->hasTargetSRFs;
	const bool	limited = q->limitCount != NULL && IsA(q->limitCount, Const) && (q->limitOffset == NULL || IsA(q->limitOffset, Const)) && q->limitOption == LIMIT_OPTION_COUNT &&
		(list_length(q->sortClause) == 1 || list_length(q->sortClause) == 2);

	return sift_enable_agg && q->commandType == CMD_SELECT && plain && limited && !q->hasRowSecurity && root->qual_security_level == 0 && root->rowMarks == NIL && root->limit_tuples >= 1 &&
		root->limit_tuples <= SCOPE_TOPK_MAX && list_length(root->join_info_list) <= 1 && bms_num_members(input_rel->relids) == 2;
}

static bool
compile_topk(ScopePlan *p, const PathTarget *target)
{
	bool		ok = false;

	p->irel = scope_rel_open(p);
	for (int ord = 0; ord < sift_f2_nscopes(p->irel) && !ok; ord++)
	{
		reset_spec(p, ord);
		p->v6_tlist = NIL;
		ok = scope_matches(p, ord) && compile_key_filter(p) && p->spec.key_given && !p->spec.key_array && compile_fact_quals(p) && !p->spec.ranged &&
			sift_v6_scope_topk_order(p, linitial_node(SortGroupClause, p->root->parse->sortClause)) &&
			(list_length(p->root->parse->sortClause) == 1 || sift_v6_scope_topk_tie(p, lsecond_node(SortGroupClause, p->root->parse->sortClause))) && sift_v6_scope_topk_output(p, target) && semi_ok(p);
	}
	relation_close(p->irel, NoLock);
	p->spec.topk = ok;
	p->spec.limit = (int) p->root->limit_tuples;
	p->spec.ncols = p->v6.nkeys;
	return ok;
}

/* The cheapest sorted plan of the statement that is no plan of ours, out of the list of the ordered relation: add_path frees a path it rejects or drops from the list, and a path out of the list is no one's to free */
static Path *
take_sorted_join(RelOptInfo *out)
{
	Path	   *best = NULL;

	foreach_ptr(Path, p, out->pathlist)
		if (!IsA(p, CustomPath) && (best == NULL || p->total_cost < best->total_cost))
			best = p;
	if (best != NULL)
		out->pathlist = list_delete_ptr(out->pathlist, best);
	return best;
}

/*
 * The newest rows of a scope key (ORDER BY ring leaf DESC LIMIT n over the facts joined to the pairs of the key) are the ring of the key in the summary (col/scope_v6/topk.c); the join, sorted, is the
 * child of the node, for a snapshot the summary does not count exactly. The child is a sorted path of the ordered relation, which create_ordered_paths filled from the paths of the join: the input
 * relation lists paths that add_path freed there.
 */
void
sift_agg_v6_scope_topk_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *out)
{
	PathTarget *target = root->upper_targets[UPPERREL_ORDERED];
	int			a,
				b;

	if (!topk_shape_ok(root, input_rel) || target == NULL)
		return;
	a = bms_next_member(input_rel->relids, -1);
	b = bms_next_member(input_rel->relids, a);
	for (int swap = 0; swap < 2; swap++)
	{
		ScopePlan	p = {.root = root,.fact = root->simple_rel_array[swap ? b : a],.pairs = root->simple_rel_array[swap ? a : b]};

		if (p.fact == NULL || p.pairs == NULL || !plain_relation(root, p.fact) || !pairs_ok(&p) || !pairs_joined(&p))
			continue;
		foreach_ptr(IndexOptInfo, ix, p.fact->indexlist)
		{
			CustomPath *cp;
			Path	   *join;

			p.index = ix;
			if (!index_usable(ix) || !compile_topk(&p, target) || (join = take_sorted_join(out)) == NULL)
				continue;
			cp = makeNode(CustomPath);
			cp->path.pathtype = T_CustomScan;
			cp->path.parent = out;
			cp->path.pathtarget = target;
			cp->path.rows = root->limit_tuples;
			cp->path.startup_cost = 0;
			cp->path.total_cost = 2 * random_page_cost + cpu_tuple_cost * root->limit_tuples;
			cp->path.pathkeys = root->sort_pathkeys;
			cp->methods = &scope_path_methods;
			cp->custom_paths = list_make1(join);
			cp->custom_private = list_make5(sift_v6_scope_spec_pack(&p.spec), p.v6_tlist, NIL, scope_exprs(&p), sift_v6_spec_pack(&p.v6));
			add_path(out, &cp->path);
			if (!list_member_ptr(out->pathlist, cp))
				add_path(out, join);
			return;
		}
	}
}

List *
sift_v6_scope_spec_pack(const ScopeSpec *s)
{
	List	   *ints = list_make5_int((int) s->relid, s->ord, s->by_key, (int) s->key_type, s->key_given);

	ints = list_concat(ints, list_make5_int(s->ranged, s->lo_strat, s->hi_strat, (int) s->time_type, s->grouped_const));
	ints = list_concat(ints, list_make5_int(s->by_month, (int) s->month_type, s->excl_col, s->ncols, s->naggs));
	ints = lappend_int(ints, s->is_table);
	ints = list_concat(ints, list_make2_int(s->scan, s->member_col));
	ints = list_concat(ints, list_make5_int(s->topk, s->ring_col, s->nulls_first, s->leaf_not_null, s->limit));
	ints = list_concat(ints, list_make3_int(s->key_array, s->tie_col, s->tie_desc));
	for (int i = 0; i < s->ncols; i++)
		ints = lappend_int(ints, s->col[i]);
	for (int j = 0; j < s->naggs; j++)
		ints = list_concat(ints, list_make5_int(s->agg[j].kind, s->agg[j].arg.attno, (int) s->agg[j].arg.typid, (int) s->agg[j].restype, s->agg[j].arg.scale));
	return ints;
}

ScopeSpec *
sift_v6_scope_spec_unpack(const List *ints)
{
	ScopeSpec  *s = palloc0(sizeof(ScopeSpec));
	int			at = 26;

	s->relid = (Oid) list_nth_int(ints, 0);
	s->ord = list_nth_int(ints, 1);
	s->by_key = list_nth_int(ints, 2);
	s->key_type = (Oid) list_nth_int(ints, 3);
	s->key_given = list_nth_int(ints, 4);
	s->ranged = list_nth_int(ints, 5);
	s->lo_strat = list_nth_int(ints, 6);
	s->hi_strat = list_nth_int(ints, 7);
	s->time_type = (Oid) list_nth_int(ints, 8);
	s->grouped_const = list_nth_int(ints, 9);
	s->by_month = list_nth_int(ints, 10);
	s->month_type = (Oid) list_nth_int(ints, 11);
	s->excl_col = list_nth_int(ints, 12);
	s->ncols = list_nth_int(ints, 13);
	s->naggs = list_nth_int(ints, 14);
	s->is_table = list_nth_int(ints, 15);
	s->scan = list_nth_int(ints, 16);
	s->member_col = list_nth_int(ints, 17);
	s->topk = list_nth_int(ints, 18);
	s->ring_col = list_nth_int(ints, 19);
	s->nulls_first = list_nth_int(ints, 20);
	s->leaf_not_null = list_nth_int(ints, 21);
	s->limit = list_nth_int(ints, 22);
	s->key_array = list_nth_int(ints, 23);
	s->tie_col = list_nth_int(ints, 24);
	s->tie_desc = list_nth_int(ints, 25);
	for (int i = 0; i < s->ncols; i++)
		s->col[i] = list_nth_int(ints, at++);
	s->agg = palloc(Max(s->naggs, 1) * sizeof(SiftV6Agg));
	for (int j = 0; j < s->naggs; j++, at += 5)
		s->agg[j] = (SiftV6Agg) {.kind = list_nth_int(ints, at),.arg = {.attno = list_nth_int(ints, at + 1),.typid = (Oid) list_nth_int(ints, at + 2),.scale = list_nth_int(ints, at + 4)},
		.restype = (Oid) list_nth_int(ints, at + 3)};
	return s;
}

static Path *
fallback_path(const ScopePlan *p, RelOptInfo *input_rel, RelOptInfo *out, GroupPathExtraData *extra, double groups)
{
	AggClauseCosts costs = {0};
	AggStrategy strategy = p->root->processed_groupClause != NIL ? AGG_HASHED : p->root->parse->groupClause != NIL ? AGG_SORTED : AGG_PLAIN;
	PathTarget *scan = make_pathtarget_from_tlist(p->scan_tlist);

	get_agg_clause_costs(p->root, AGGSPLIT_SIMPLE, &costs);
	set_pathtarget_cost_width(p->root, scan);
	return (Path *) create_agg_path(p->root, out, input_rel->cheapest_total_path, scan, strategy, AGGSPLIT_SIMPLE,
									p->root->processed_groupClause, (List *) extra->havingQual, &costs, groups);
}

static void
add_scope_path(ScopePlan *p, RelOptInfo *input_rel, RelOptInfo *out, GroupPathExtraData *extra)
{
	CustomPath *cp = makeNode(CustomPath);
	const double groups = p->spec.scan ? (p->v6.nkeys == 0 ? 1.0 : clamp_row_est(Min(p->fact->rows, 1000.0))) :
		clamp_row_est((p->spec.key_array && p->spec.by_key ? 10.0 : p->spec.key_given ? 1.0 : Min(p->pairs->tuples, 1000.0)) * (p->spec.by_month ? 24 : 1));

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = out;
	cp->path.pathtarget = out->reltarget;
	cp->path.rows = groups;
	cp->path.startup_cost = 0;
	cp->path.total_cost = 2 * random_page_cost + cpu_tuple_cost * groups;
	if (p->spec.scan)
	{
		Cost		open;

		sift_v6_scan_price(p->root, p->fact, p->index, 0, &open, &cp->path.total_cost);
		cp->path.startup_cost = open;
		cp->path.total_cost += cpu_tuple_cost * groups + cpu_operator_cost * p->fact->rows;
	}
	cp->methods = &scope_path_methods;
	if (p->spec.scan)
		cp->custom_private = list_make5(sift_v6_scope_spec_pack(&p->spec), p->v6_tlist, extra->havingQual, scope_exprs(p), sift_v6_spec_pack(&p->v6));
	else
	{
		cp->custom_paths = list_make1(fallback_path(p, input_rel, out, extra, groups));
		cp->custom_private = list_make4(sift_v6_scope_spec_pack(&p->spec), p->scan_tlist, extra->havingQual, scope_exprs(p));
	}
	add_path(out, &cp->path);
}

bool
sift_v6_scope_one_key(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *pairs, IndexOptInfo *ix, AttrNumber fact_att, AttrNumber pairs_att, int *ord, Expr **key)
{
	ScopePlan	p = {.root = root,.fact = fact,.pairs = pairs,.index = ix};
	bool		ok = false;

	if (!plain_relation(root, fact) || !pairs_ok(&p) || !index_usable(ix))
		return false;
	p.irel = scope_rel_open(&p);
	for (int o = 0; o < sift_f2_nscopes(p.irel) && !ok; o++)
	{
		reset_spec(&p, o);
		ok = scope_matches(&p, o) && is_int(p.spec.key_type) && fact_attno(&p, p.layout.member_col) == fact_att && p.leg.member == pairs_att && compile_key_filter(&p) &&
			p.spec.key_given && !p.spec.key_array;
		if (ok)
		{
			*ord = o;
			*key = p.key_expr;
		}
	}
	relation_close(p.irel, NoLock);
	return ok;
}

void
sift_agg_v6_scope_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *out, void *extra)
{
	int			a = -1,
				b;

	if (!shape_ok(root, input_rel) || out->reltarget == NULL || input_rel->cheapest_total_path == NULL)
		return;
	a = bms_next_member(input_rel->relids, -1);
	b = bms_next_member(input_rel->relids, a);
	for (int swap = 0; swap < 2; swap++)
	{
		ScopePlan	p = {.root = root,.fact = root->simple_rel_array[swap ? b : a],.pairs = root->simple_rel_array[swap ? a : b]};

		if (p.fact == NULL || p.pairs == NULL || !plain_relation(root, p.fact) || !pairs_ok(&p) || !pairs_joined(&p))
			continue;
		if (table_usable(root, p.fact) && compile_on(&p, (GroupPathExtraData *) extra, out))
		{
			add_scope_path(&p, input_rel, out, (GroupPathExtraData *) extra);
			return;
		}
		foreach_ptr(IndexOptInfo, ix, p.fact->indexlist)
		{
			p.index = ix;
			if (index_usable(ix) && compile_on(&p, (GroupPathExtraData *) extra, out))
			{
				add_scope_path(&p, input_rel, out, (GroupPathExtraData *) extra);
				return;
			}
		}
	}
}

Plan *
sift_agg_v6_scope_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);

	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->flags = best->flags;
	cs->custom_plans = custom_plans;
	cs->custom_private = list_length(best->custom_private) > 4 ? list_make2(linitial(best->custom_private), list_nth(best->custom_private, 4)) : list_make1(linitial(best->custom_private));
	cs->custom_scan_tlist = lsecond(best->custom_private);
	cs->scan.plan.qual = (List *) lthird(best->custom_private);
	cs->custom_exprs = lfourth(best->custom_private);
	cs->methods = &sift_agg_v6_scope_plan_methods;
	root->glob->relationOids = lappend_oid(root->glob->relationOids, (Oid) list_nth_int(linitial(best->custom_private), 0));
	return &cs->scan.plan;
}
