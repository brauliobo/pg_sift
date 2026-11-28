/*
 * agg/scope/plan.c - the planning of the scope aggregate of SiftAggV6: `facts f JOIN pairs s ON s.member = f.fk WHERE s.key = K` with count, sum, avg, min and max of the integer, float8 and
 * numeric leaves of the summary the relation that keeps the facts holds for the scope (an index of the facts, or the facts themselves when they are a table of a format that keeps one) is the cube
 * of the key K, read without a row of the facts: one tuple for the key, or for every key of the summary when the statement groups by the key without naming one, or for every month of it when it
 * groups by date_trunc('month') of the time leaf. The facts may be limited to a window of whole months of the time leaf and to the rows whose soft-delete leaf is NULL; the pairs may be the relation
 * itself or a single-table sub-select or materialized CTE over it (agg/scope/match.c, agg/scope/keys.c). What no cube tells is a scan of the facts of the members of the key, and the newest rows
 * of a key are the ring of the summary. The format that owns the relation answers what its summary keeps (SiftScopeOps) and runs the plan (SiftScopeOps.exec): it reads the summary when it
 * counts what the snapshot sees, and runs the join as the planner priced it, the child of the node, when it does not.
 */
/* check-memory: allow-allocs every array here follows the aggregates of one statement and the keys of one summary */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_namespace_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/cost/card.h"
#include "agg/cost/model.h"
#include "agg/explain.h"
#include "agg/refuse.h"
#include "agg/safety.h"
#include "agg/scope/scope.h"
#include "agg/settings.h"
#include "core/catname.h"

static Plan *scope_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);

static const CustomPathMethods scope_path_methods = {"SiftAggV6Scope", scope_plan_path, NULL};

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

AttrNumber
sift_v6_scope_fact_attno(const ScopePlan *p, int col)
{
	return p->index == NULL ? (AttrNumber) col : p->index->indexkeys[col - 1];
}

Oid
sift_v6_scope_opfamily(const ScopePlan *p, int col)
{
	return p->index == NULL ? p->table_opfamily[col] : p->index->opfamily[col - 1];
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
	if (sift_v6_scope_key_var(p, n) && (p->spec.by_key || p->spec.key_given))
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
	strat = get_op_opfamily_strategy(opno, sift_v6_scope_opfamily(p, p->layout.time_col));
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

	if (nt->nulltesttype != IS_NULL || col < 1 || p->spec.excl_col != 0 || !p->scope->excl_ok(p->irel, p->spec.ord, col))
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
	if (IsA(arg, FuncExpr) && (((FuncExpr *) arg)->funcid == F_TIMESTAMP_DATE || ((FuncExpr *) arg)->funcid == F_TIMESTAMPTZ_DATE))
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

		if (sift_v6_scope_key_var(p, g) && !p->spec.by_key)
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
	if (!(compile_groups(p) && sift_v6_scope_key_filter(p) && compile_fact_quals(p) && (!p->spec.grouped_const || p->spec.key_given) &&
		  ((p->spec.key_given && !p->spec.key_array) || p->spec.by_key || !p->spec.by_month) &&
		  compile_exprs(p, (Node *) out->reltarget->exprs) && (extra->havingQual == NULL || compile_exprs(p, extra->havingQual))))
		return false;
	order_like_target(p, out->reltarget);
	return true;
}

/* The tables a join reads, without the outer joins PostgreSQL numbers among its relids */
static Relids
base_relids(const PlannerInfo *root, const RelOptInfo *rel)
{
	return bms_intersect(rel->relids, root->all_baserels);
}

static bool
shape_ok(PlannerInfo *root)
{
	Query	   *parse = root->parse;

	return sift_enable_agg && parse->hasAggs && !parse->hasRowSecurity && root->qual_security_level == 0 && !parse->hasDistinctOn && !parse->hasTargetSRFs && !parse->hasWindowFuncs &&
		parse->groupingSets == NIL;
}

/* The facts and the pairs, joined once */
static bool
two_relations(PlannerInfo *root, RelOptInfo *input_rel)
{
	return list_length(root->join_info_list) <= 1 && bms_num_members(base_relids(root, input_rel)) == 2;
}

static Relation
scope_rel_open(ScopePlan *p)
{
	const Oid	relid = p->index == NULL ? planner_rt_fetch(p->fact->relid, p->root)->relid : p->index->indexoid;
	SiftStoreSchema sch;

	if (p->index == NULL && p->table_opfamily == NULL && p->ops->schema(relid, true, &sch))
		p->table_opfamily = sch.att_opfamily;
	return relation_open(relid, AccessShareLock);
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
	if (!sift_v6_scope_key_filter(p) || !sift_v6_scope_key_bound(p))
		return false;
	p->spec = (ScopeSpec) {.relid = p->spec.relid,.is_table = p->spec.is_table,.ord = p->spec.ord,.key_type = p->spec.key_type,.key_given = true,.key_array = p->spec.key_array,.scan = true,
	.member_col = p->layout.member_col};
	return sift_v6_compile_fact(p->root, p->fact, out, p->index, extra, &p->v6, &p->v6_tlist);
}

/* The statement groups by no column of the pairs but the scope key: a pair's other columns are not in the summary or the scan of the members */
static bool
pairs_groups_ok(ScopePlan *p)
{
	foreach_node(SortGroupClause, sgc, p->root->processed_groupClause)
	{
		Node	   *g = get_sortgroupclause_expr(sgc, p->root->parse->targetList);

		if (!sift_v6_scope_key_var(p, g) && bms_is_member(p->pairs->relid, pull_varnos(p->root, g)))
		{
			if (p->why == NULL)
				p->why = psprintf("the statement reads a column of the scope relation other than its key as a group key: %s", sift_v6_deparse_planning(p->root, g, true));
			return false;
		}
	}
	return true;
}

static bool
compile_on(ScopePlan *p, GroupPathExtraData *extra, RelOptInfo *out)
{
	bool		ok = false;

	p->irel = scope_rel_open(p);
	for (int ord = 0; ord < p->scope->nscopes(p->irel) && !ok; ord++)
	{
		reset_spec(p, ord);
		ok = sift_v6_scope_matches(p, ord) && pairs_groups_ok(p) && ((p->scope->exec != NULL && compile_target(p, extra, out) && sift_v6_scope_semi_ok(p)) ||
											   (p->index != NULL && (reset_spec(p, ord), compile_scan(p, extra, out)) && sift_v6_scope_semi_ok(p)));
	}
	relation_close(p->irel, NoLock);
	return ok;
}

/* A table of the format whose summary is current: its own versions decide the rows, so nothing is asked of track_changes or of a stale flag */
static bool
table_usable(ScopePlan *p)
{
	const Oid	relid = planner_rt_fetch(p->fact->relid, p->root)->relid;
	Relation	rel;
	bool		ok;

	if ((p->ops = sift_store_ops_for_oid(relid, true)) == NULL || p->ops->scope != p->scope)
		return false;
	rel = table_open(relid, AccessShareLock);
	ok = p->scope->readable(rel);
	table_close(rel, NoLock);
	return ok;
}

/* An index of the format over every row of the facts whose summary is current */
static bool
index_usable(ScopePlan *p, const IndexOptInfo *ix)
{
	Relation	irel;
	bool		ok;

	if (ix->indpred != NIL || ix->hypothetical || !sift_store_ops_may_own(ix->relam, false) || (p->ops = sift_store_ops_for_oid(ix->indexoid, false)) == NULL || p->ops->scope != p->scope)
		return false;
	irel = index_open(ix->indexoid, AccessShareLock);
	ok = p->scope->readable(irel);
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

/* A read of the summary reads two pages of the relation, as cold as a scan of it finds them (agg/cost/model.h), and a record for each of the rows it leaves (a cube of a key or a month, a row of a ring) */
static void
summary_price(const ScopePlan *p, double rows, Path *path)
{
	path->startup_cost = 0;
	path->total_cost = 2 * random_page_cost * sift_agg_cold_share(p->index != NULL ? p->index->pages : p->fact->pages) + cpu_tuple_cost * rows;
}

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

/* ORDER BY leaf DESC of a leaf of the index that can be a ring, and whether the ring (NULLS LAST) serves the way the statement puts the NULLs */
static bool
topk_order(ScopePlan *p, SortGroupClause *sgc)
{
	const Var  *v = (const Var *) get_sortgroupclause_expr(sgc, p->root->parse->targetList);
	int			col = IsA(v, Var) ? sift_v6_scope_index_col(p, v) : -1;

	if (col < 1 || !p->scope->ring_col(p->irel, col) || get_op_opfamily_strategy(sgc->sortop, sift_v6_scope_opfamily(p, col)) != BTGreaterStrategyNumber)
		return false;
	p->spec.ring_col = col;
	p->spec.nulls_first = sgc->nulls_first;
	p->spec.leaf_not_null = bms_is_member(v->varattno, p->fact->notnullattnums);
	return true;
}

/* The second ORDER BY leaf: an integer or a time that is never NULL, in either direction: the rows the ring leaf does not tell apart are ordered by it */
static bool
topk_tie(ScopePlan *p, SortGroupClause *sgc)
{
	const Var  *v = (const Var *) get_sortgroupclause_expr(sgc, p->root->parse->targetList);
	const int	col = IsA(v, Var) ? sift_v6_scope_index_col(p, v) : -1;
	const int	strat = col > 0 ? get_op_opfamily_strategy(sgc->sortop, sift_v6_scope_opfamily(p, col)) : 0;

	if (col < 1 || !bms_is_member(v->varattno, p->fact->notnullattnums) || (strat != BTLessStrategyNumber && strat != BTGreaterStrategyNumber))
		return false;
	switch (v->vartype)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			break;
		default:
			return false;
	}
	p->spec.tie_col = col;
	p->spec.tie_desc = strat == BTGreaterStrategyNumber;
	return true;
}

/* The columns of the rows are columns of the index the scan decodes as they are stored (SiftV6Spec.key of the spec that follows the scope spec in the plan) */
static bool
topk_output(ScopePlan *p, const PathTarget *target)
{
	const Oid	relid = RelationGetRelid(p->irel);

	p->v6 = (SiftV6Spec) {.relid = relid,.is_table = p->index == NULL};
	p->v6.key = palloc(Max(list_length(target->exprs), 1) * sizeof(SiftV6Col));
	foreach_ptr(Expr, e, target->exprs)
	{
		const Var  *v = (const Var *) e;
		const int	col = IsA(e, Var) ? sift_v6_scope_index_col(p, v) : -1;

		if (col < 1 || p->v6.nkeys == SIFT_V6_MAX_KEYS || !p->ops->leaf_holds(relid, p->index == NULL, col, sift_v6_class(v->vartype)))
			return false;
		p->v6.key[p->v6.nkeys++] = (SiftV6Col) {.attno = col,.typid = v->vartype};
		p->v6_tlist = lappend(p->v6_tlist, makeTargetEntry(copyObject(e), list_length(p->v6_tlist) + 1, NULL, false));
	}
	return true;
}

static bool
compile_topk(ScopePlan *p, const PathTarget *target)
{
	bool		ok = false;

	p->irel = scope_rel_open(p);
	for (int ord = 0; ord < p->scope->nscopes(p->irel) && !ok; ord++)
	{
		reset_spec(p, ord);
		p->v6_tlist = NIL;
		ok = sift_v6_scope_matches(p, ord) && sift_v6_scope_key_filter(p) && sift_v6_scope_key_bound(p) && !p->spec.key_array && compile_fact_quals(p) && !p->spec.ranged &&
			topk_order(p, linitial_node(SortGroupClause, p->root->parse->sortClause)) &&
			(list_length(p->root->parse->sortClause) == 1 || topk_tie(p, lsecond_node(SortGroupClause, p->root->parse->sortClause))) && topk_output(p, target) && sift_v6_scope_semi_ok(p);
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
 * The newest rows of a scope key (ORDER BY ring leaf DESC LIMIT n over the facts joined to the pairs of the key) are the ring of the key in the summary (col/f2/cube/topk.c); the join, sorted, is the
 * child of the node, for a snapshot the summary does not count exactly. The child is a sorted path of the ordered relation, which create_ordered_paths filled from the paths of the join: the input
 * relation lists paths that add_path freed there.
 */
void
sift_v6_scope_topk_paths(const SiftScopeOps *scope, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *out)
{
	PathTarget *target = root->upper_targets[UPPERREL_ORDERED];
	int			a,
				b;

	if (!sift_agg_scope || !topk_shape_ok(root, input_rel) || target == NULL)
		return;
	a = bms_next_member(input_rel->relids, -1);
	b = bms_next_member(input_rel->relids, a);
	for (int swap = 0; swap < 2; swap++)
	{
		ScopePlan	p = {.scope = scope,.root = root,.fact = root->simple_rel_array[swap ? b : a],.pairs = root->simple_rel_array[swap ? a : b]};

		if (p.fact == NULL || p.pairs == NULL || !sift_v6_scope_plain_relation(root, p.fact) || !sift_v6_scope_pairs_ok(&p) || !sift_v6_scope_pairs_joined(&p))
			continue;
		foreach_ptr(IndexOptInfo, ix, p.fact->indexlist)
		{
			CustomPath *cp;
			Path	   *join;

			p.index = ix;
			if (!index_usable(&p, ix) || !compile_topk(&p, target) || (join = take_sorted_join(out)) == NULL)
				continue;
			cp = makeNode(CustomPath);
			cp->path.pathtype = T_CustomScan;
			cp->path.parent = out;
			cp->path.pathtarget = target;
			cp->path.rows = root->limit_tuples;
			summary_price(&p, root->limit_tuples, &cp->path);
			cp->path.pathkeys = root->sort_pathkeys;
			cp->methods = &scope_path_methods;
			p.spec.once = p.once;
			cp->custom_paths = list_make1(join);
			cp->custom_private = list_make5(sift_v6_scope_spec_pack(&p.spec), p.v6_tlist, NIL, scope_exprs(&p), sift_v6_spec_pack(&p.v6));
			sift_v6_force_price(&cp->path);
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
	ints = list_concat(ints, list_make4_int(s->key_array, s->tie_col, s->tie_desc, s->once));
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
	int			at = 27;

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
	s->once = list_nth_int(ints, 26);
	for (int i = 0; i < s->ncols; i++)
		s->col[i] = list_nth_int(ints, at++);
	s->agg = palloc(Max(s->naggs, 1) * sizeof(SiftV6Agg));
	for (int j = 0; j < s->naggs; j++, at += 5)
		s->agg[j] = (SiftV6Agg) {.kind = list_nth_int(ints, at),.arg = {.attno = list_nth_int(ints, at + 1),.typid = (Oid) list_nth_int(ints, at + 2),.scale = list_nth_int(ints, at + 4)},
		.restype = (Oid) list_nth_int(ints, at + 3)};
	return s;
}

/*
 * The aggregate over the join as written, with the columns of the tuple the node leaves (tlist): what the node runs when no read of the summary or scan of the members answers. It is the plan
 * PostgreSQL makes: hashed only without ordered aggregates, else over the join sorted by the group pathkeys, which hold the order of the aggregates PostgreSQL marked presorted (DISTINCT, ORDER BY)
 */
static Path *
fallback_path(const ScopePlan *p, List *tlist, RelOptInfo *input_rel, RelOptInfo *out, GroupPathExtraData *extra, double groups)
{
	PlannerInfo *root = p->root;
	AggClauseCosts costs = {0};
	const bool	hashed = root->processed_groupClause != NIL && root->numOrderedAggs == 0 && grouping_is_hashable(root->processed_groupClause);
	AggStrategy strategy = hashed ? AGG_HASHED : root->parse->groupClause != NIL ? AGG_SORTED : AGG_PLAIN;
	PathTarget *scan = make_pathtarget_from_tlist(tlist);
	Path	   *in = input_rel->cheapest_total_path;

	if (!hashed && !pathkeys_contained_in(root->group_pathkeys, in->pathkeys))
		in = (Path *) create_sort_path(root, input_rel, in, root->group_pathkeys, -1.0);
	get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &costs);
	set_pathtarget_cost_width(root, scan);
	return (Path *) create_agg_path(root, out, in, scan, strategy, AGGSPLIT_SIMPLE, root->processed_groupClause, (List *) extra->havingQual, &costs, groups);
}

/* The share of the facts the scan of the members of the key reads: the router keeps the granules of the key, which are the share of the facts the join keeps when the parts are sorted by the
 * member, and nearly all of them when the members are spread */
static double
scan_kept(const ScopePlan *p, const RelOptInfo *input_rel)
{
	const SiftOrderOps *order = p->ops->order;
	const Oid	relid = p->index == NULL ? planner_rt_fetch(p->fact->relid, p->root)->relid : p->index->indexoid;

	if (order == NULL || order->lead_att(relid, p->index == NULL) != sift_v6_scope_fact_attno(p, p->layout.member_col))
		return 1.0;
	return Min(input_rel->rows / Max(p->fact->rows, 1.0), 1.0);
}

/* The scan of the members of the keys reads the granules of its keys: the plan keeps its price over every granule, which the node scales by the share the keys of its run route to (scan.c) */
static void
scan_price(ScopePlan *p, RelOptInfo *input_rel, double groups, Path *path, List **prices)
{
	Cost		open,
				scan;

	sift_v6_scan_price(p->root, p->fact, p->index, 0, &open, &scan);
	scan += cpu_operator_cost * p->fact->rows - open;
	path->startup_cost = open;
	path->total_cost = open + scan_kept(p, input_rel) * scan + cpu_tuple_cost * groups;
	*prices = list_make3(makeFloat(psprintf("%.17g", open)), makeFloat(psprintf("%.17g", scan)), makeFloat(psprintf("%.17g", cpu_tuple_cost * groups)));
}

static void
add_scope_path(ScopePlan *p, RelOptInfo *input_rel, RelOptInfo *out, GroupPathExtraData *extra)
{
	CustomPath *cp = makeNode(CustomPath);
	List	   *prices = NIL;
	const double groups = p->spec.scan ? (p->v6.nkeys == 0 ? 1.0 : clamp_row_est(Min(p->fact->rows, 1000.0))) :
		clamp_row_est((!p->spec.by_key || (p->spec.key_given && !p->spec.key_array) ? 1.0 : p->spec.key_given ? 10.0 : Min(p->pairs->tuples, 1000.0)) * (p->spec.by_month ? 24 : 1));

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = out;
	cp->path.pathtarget = out->reltarget;
	cp->path.rows = groups;
	if (p->spec.scan)
		scan_price(p, input_rel, groups, &cp->path, &prices);
	else
		summary_price(p, groups, &cp->path);
	cp->methods = &scope_path_methods;
	p->spec.once = p->once;
	cp->custom_paths = list_make1(fallback_path(p, p->spec.scan ? p->v6_tlist : p->scan_tlist, input_rel, out, extra, groups));
	if (p->spec.scan)
		cp->custom_private = lappend(list_make5(sift_v6_scope_spec_pack(&p->spec), p->v6_tlist, extra->havingQual, scope_exprs(p), sift_v6_spec_pack(&p->v6)), prices);
	else
		cp->custom_private = list_make4(sift_v6_scope_spec_pack(&p->spec), p->scan_tlist, extra->havingQual, scope_exprs(p));
	sift_v6_force_price(&cp->path);
	sift_agg_arm_priced("scope", cp->path.total_cost, -1);
	add_path(out, &cp->path);
}

bool
sift_v6_scope_one_key(const SiftScopeOps *scope, PlannerInfo *root, RelOptInfo *fact, RelOptInfo *pairs, IndexOptInfo *ix, AttrNumber fact_att, AttrNumber pairs_att, int *ord, Expr **key)
{
	ScopePlan	p = {.scope = scope,.root = root,.fact = fact,.pairs = pairs,.index = ix};
	bool		ok = false;

	if (!sift_agg_scope || !sift_v6_scope_plain_relation(root, fact) || !sift_v6_scope_pairs_ok(&p) || !index_usable(&p, ix))
		return false;
	p.irel = scope_rel_open(&p);
	for (int o = 0; o < p.scope->nscopes(p.irel) && !ok; o++)
	{
		reset_spec(&p, o);
		ok = sift_v6_scope_matches(&p, o) && is_int(p.spec.key_type) && sift_v6_scope_fact_attno(&p, p.layout.member_col) == fact_att && p.leg.member == pairs_att && sift_v6_scope_key_filter(&p) &&
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

/* The facts, a plain table: what else joins them to the pairs is a condition on the pairs (sift_v6_scope_key_filter), and an outer or anti join is refused by the scope that matches */
static bool
fact_ok(PlannerInfo *root, RelOptInfo *rel)
{
	return rel != NULL && rel->reloptkind == RELOPT_BASEREL && sift_v6_plain_table(planner_rt_fetch(rel->relid, root)) && rel->lateral_relids == NULL;
}

static void
scope_refuse(const char *why, bool of_shape)
{
	if (why != NULL && sift_agg_arms_noted())
		(of_shape ? sift_agg_arm_refused_by_shape : sift_agg_arm_refused_by) ("scope", AGG_REFUSE_SCOPE, why);
	else if (why != NULL)
		sift_agg_refuse("scope", AGG_REFUSE_SCOPE, why, 0, -1);
}

void
sift_v6_scope_upper_paths(const SiftScopeOps *scope, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *out, void *extra)
{
	int			a = -1,
				b;
	const char *why = NULL;

	if (!sift_agg_scope || !shape_ok(root) || out->reltarget == NULL || input_rel->cheapest_total_path == NULL)
		return;
	if (!two_relations(root, input_rel))
		return scope_refuse(sift_v6_scope_shape_why(root, base_relids(root, input_rel)), true);
	a = bms_next_member(base_relids(root, input_rel), -1);
	b = bms_next_member(base_relids(root, input_rel), a);
	for (int swap = 0; swap < 2; swap++)
	{
		ScopePlan	p = {.scope = scope,.root = root,.fact = root->simple_rel_array[swap ? b : a],.pairs = root->simple_rel_array[swap ? a : b],.outer = sift_v6_scope_outer_join(root)};

		if (p.fact == NULL || p.pairs == NULL || !fact_ok(root, p.fact) || !sift_v6_scope_pairs_ok(&p) || !sift_v6_scope_pairs_joined(&p))
			continue;
		if (table_usable(&p) && compile_on(&p, (GroupPathExtraData *) extra, out))
		{
			add_scope_path(&p, input_rel, out, (GroupPathExtraData *) extra);
			return;
		}
		foreach_ptr(IndexOptInfo, ix, p.fact->indexlist)
		{
			p.index = ix;
			if (index_usable(&p, ix) && compile_on(&p, (GroupPathExtraData *) extra, out))
			{
				add_scope_path(&p, input_rel, out, (GroupPathExtraData *) extra);
				return;
			}
		}
		why = why != NULL ? why : p.why;
	}
	scope_refuse(why, false);
}

static Plan *
scope_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	const ScopeSpec *spec = sift_v6_scope_spec_unpack(linitial(best->custom_private));

	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->flags = best->flags;
	cs->custom_plans = custom_plans;
	cs->custom_private = list_concat(list_make1(linitial(best->custom_private)), list_copy_tail(best->custom_private, 4));
	cs->custom_scan_tlist = lsecond(best->custom_private);
	cs->scan.plan.qual = (List *) lthird(best->custom_private);
	cs->custom_exprs = lfourth(best->custom_private);
	cs->methods = spec->scan ? &sift_v6_scope_scan_methods : sift_store_ops_for_oid(spec->relid, spec->is_table)->scope->exec;
	root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->relid);
	return &cs->scan.plan;
}
