/*
 * agg/hybrid/plan.c - the path of SiftHybridScan: a single heap relation with one text query (body @@> 'const' or $1) on a sift index and at least one structured qual, of which a sift_col
 * index of format major 2 (whole, or partial with a predicate the query implies) decides those agg/hybrid/quals.h names exactly: the others stay as quals of the scan for the heap (a query
 * of the text alone is the text index's own scan). A sift_table is its own store and takes the comparisons and NULL tests of a constant. Nothing else the scan could not serve (a row-level
 * policy, a volatile function, a score in a qual, a join). A parameter in the query or a qual leaves the direction to the executor (SIFT_HYB_ADAPTIVE) when both apply; sift_hybrid_choose
 * (hybrid.c) picks the direction, or none and the standard paths stay.
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "miscadmin.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"

#include "agg/agg.h"
#include "agg/hybrid/core.h"
#include "agg/hybrid/int.h"
#include "agg/hybrid/quals.h"
#include "agg/engine.h"
#include "agg/refuse.h"
#include "bm25/planner/planner.h"
#include "bm25/planner/int.h"
#include "col/am.h"
#include "agg/cost/model.h"
#include "agg/cost/card.h"
#include "agg/join/join.h"
#include "core/catalog.h"
#include "core/store/ops.h"
#include "table/dispatch.h"
#include "core/sift.h"

static set_rel_pathlist_hook_type prev_pathlist_hook;
static create_upper_paths_hook_type prev_upper_hook;

/* The columnar side of the relation the planner reads its columns and operators from: the sift_col index, or the store of the sift_table (NULL index) */
typedef struct SiftHybridStoreRef
{
	IndexOptInfo *index;
	bool		is_table;
	Oid			relid;			/* a sift_table: the table */
	SiftStoreSchema tsch;		/* and the schema of its store */
} SiftHybridStoreRef;

typedef struct HybridPlan
{
	PlannerInfo *root;
	RelOptInfo *rel;
	IndexOptInfo *text;
	SiftHybridStoreRef store;
	Relation	colrel;			/* the sift_col index, open */
	RestrictInfo *match;		/* the clause of the match, or the OR of it with the structured quals */
	Expr	   *match_expr;		/* the match itself */
	bool		disjunct;		/* match OR quals: quals are the OR's other branch */
	Expr	   *query;
	List	   *quals;			/* the quals the store decides, the column on the left */
	List	   *qinfo;			/* of each three integers: SiftHybQualKind, the column of the store's index, whether the pattern folds */
	List	   *clauses;		/* the clauses the scan keeps for the rows it returns: the pushed ones and the others */
	List	   *residuals;		/* the clauses the store does not decide: the heap does */
	bool		residual;		/* a qual the store does not decide */
	bool		params;			/* the query or a qual has a parameter: the selectivities are not known */
	PathKey    *pk;
	bool		ranked;
	int64		k;
	Expr	   *keyset;			/* the hint of hybrid_semi_plan.c, (col = ANY (set)) IS NOT UNKNOWN */
} HybridPlan;

/* The types of a constant a sift_table's store compares an integer-like column with, whatever the width of the column (the other types are compared with a column of their own type) */
static bool
table_const_type(Oid typid)
{
	switch (typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return true;
		default:
			return false;
	}
}

static int store_attr(const SiftHybridStoreRef *store, AttrNumber attno);

/* A column of a sift_table the store judges a comparison of: a number or a time held as it is (not LOSSY, not wide, not a string, whose order a collation may decide) */
static bool
table_col_exact(const SiftHybridStoreRef *store, AttrNumber attno)
{
	int			col = store_attr(store, attno);
	const SiftColDef *d;

	if (col < 0)
		return false;
	d = &store->tsch.leaves->cols[store->tsch.att_first[attno]];
	return !(d->flags & SIFT_COLD_LOSSY) && !sift_col_def_is_wide(d) && d->store != SIFT_COLS_STR && d->type != SIFT_COLT_VARLENA &&
		d->type != SIFT_COLT_TIMETZ;
}

/* The column of the store (0-based) a heap attribute is: the column of the index, or the attribute of the table; -1: the store does not hold it */
static int
store_attr(const SiftHybridStoreRef *store, AttrNumber attno)
{
	if (store->is_table)
		return attno > 0 && attno <= store->tsch.natts && store->tsch.att_first[attno] >= 0 ? attno - 1 : -1;
	for (int i = 0; i < store->index->ncolumns; i++)
		if (store->index->indexkeys[i] == attno)
			return i;
	return -1;
}

static Node *
strip_relabel(Node *e)
{
	while (e && IsA(e, RelabelType))
		e = (Node *) ((RelabelType *) e)->arg;
	return e;
}

static int
table_column(const SiftHybridStoreRef *store, Node *e)
{
	e = strip_relabel(e);
	if (e == NULL || !IsA(e, Var) || ((Var *) e)->varlevelsup != 0)
		return -1;
	return store_attr(store, ((Var *) e)->varattno);
}

static Expr *
table_compare(const SiftHybridStoreRef *store, OpExpr *op)
{
	OpExpr	   *q = copyObject(op);
	Node	   *l = linitial(q->args),
			   *r = lsecond(q->args);
	int			col,
				strat;

	if (list_length(q->args) != 2)
		return NULL;
	if (IsA(r, Var) && IsA(l, Const))
	{
		q->opno = get_commutator(q->opno);
		if (!OidIsValid(q->opno))
			return NULL;
		q->opfuncid = get_opcode(q->opno);
		q->args = list_make2(r, l);
		l = r;
		r = lsecond(q->args);
	}
	col = table_column(store, l);
	if (col < 0 || !IsA(r, Const) || ((Const *) r)->constisnull || !table_col_exact(store, col + 1) || !(((Const *) r)->consttype == exprType(l) || (table_const_type(((Const *) r)->consttype) && table_const_type(exprType(l)))))
		return NULL;
	strat = get_op_opfamily_strategy(q->opno, store->tsch.att_opfamily[col + 1]);
	if (strat < BTLessStrategyNumber || strat > BTGreaterStrategyNumber)
		return NULL;
	linitial(q->args) = strip_relabel(l);
	return (Expr *) q;
}

/* An IN list of constants on a column of a sift_table: the store takes it as one array key of equalities */
static Expr *
table_in_list(const SiftHybridStoreRef *store, ScalarArrayOpExpr *sa)
{
	Node	   *l = linitial(sa->args),
			   *r = lsecond(sa->args);
	int			col = table_column(store, l);

	if (!sa->useOr || col < 0 || !table_col_exact(store, col + 1) || !IsA(r, Const) || ((Const *) r)->constisnull ||
		get_element_type(((Const *) r)->consttype) != exprType(l) ||
		get_op_opfamily_strategy(sa->opno, store->tsch.att_opfamily[col + 1]) != BTEqualStrategyNumber)
		return NULL;
	sa = copyObject(sa);
	linitial(sa->args) = strip_relabel(l);
	return (Expr *) sa;
}

/* The qual as the store of a sift_table decides it (a comparison with a constant, an IN list, a NULL test, the column on the left), NULL when it does not */
static Expr *
table_qual(const SiftHybridStoreRef *store, Expr *clause)
{
	NullTest   *nt;

	switch (nodeTag(clause))
	{
		case T_OpExpr:
			return table_compare(store, (OpExpr *) clause);
		case T_ScalarArrayOpExpr:
			return table_in_list(store, (ScalarArrayOpExpr *) clause);
		case T_NullTest:
			nt = (NullTest *) clause;
			if (nt->argisrow || table_column(store, (Node *) nt->arg) < 0 || !table_col_exact(store, ((Var *) strip_relabel((Node *) nt->arg))->varattno))
				return NULL;
			nt = copyObject(nt);
			nt->arg = (Expr *) strip_relabel((Node *) nt->arg);
			return (Expr *) nt;
		default:
			return NULL;
	}
}

static bool
scores_only(PlannerInfo *root, RelOptInfo *rel)
{
	bool		is_max;

	foreach_ptr(Node, e, score_exprs(root, rel))
		if (!sift_is_score_expr(e, rel->relid, &is_max) || is_max)
			return false;
	return true;
}

static bool
shape_ok(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	return sift_enable_scan && rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && !rte->inh && rte->tablesample == NULL &&
		rte->securityQuals == NIL && rel->reloptkind == RELOPT_BASEREL && rel->lateral_relids == NULL && rel->indexlist != NIL &&
		root->parse->commandType == CMD_SELECT && root->rowMarks == NIL && bms_membership(root->all_baserels) == BMS_SINGLETON && !rel_secured(root, rel) && scores_only(root, rel);
}

/* A sift_col index of format major 2 over plain columns: the whole table, or the rows of a predicate the query implies */
static IndexOptInfo *
col_index(RelOptInfo *rel)
{
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
		if ((ix->indpred == NIL || ix->predOK) && !ix->hypothetical && ix->nkeycolumns == ix->ncolumns && sift_store_ops_may_own(ix->relam, false) &&
			sift_store_ops_serves_hybrid(sift_store_ops_for_oid(ix->indexoid, false)))
			return ix;
	return NULL;
}

/* The columnar side: the store of the sift_table, or the sift_col index of the heap (opened as colrel) */
static bool
pick_store(HybridPlan *h, RangeTblEntry *rte)
{
	if (sift_relid_is_sift_table(rte->relid))
	{
		const SiftStoreOps *ops = sift_store_ops_for_oid(rte->relid, true);

		h->store.relid = rte->relid;
		h->store.is_table = sift_store_ops_serves_hybrid(ops) && ops->zone != NULL && ops->schema(rte->relid, true, &h->store.tsch);
	}
	else
		h->store.index = col_index(h->rel);
	if (h->store.index != NULL)
		h->colrel = index_open(h->store.index->indexoid, AccessShareLock);
	return h->store.is_table || h->store.index != NULL;
}

static void
close_store(HybridPlan *h)
{
	if (h->colrel != NULL)
		index_close(h->colrel, AccessShareLock);
	h->colrel = NULL;
}

static bool
runtime_query(Expr *q)
{
	return !contain_var_clause((Node *) q) && !contain_volatile_functions((Node *) q) && !(IsA(q, Const) && ((Const *) q)->constisnull);
}

static bool
find_match(HybridPlan *h)
{
	foreach_ptr(IndexOptInfo, ix, h->rel->indexlist)
	{
		RestrictInfo *found = NULL;
		Expr	   *q;

		if (!sift_v6_text_index(ix))
			continue;
		foreach_node(RestrictInfo, ri, h->rel->baserestrictinfo)
			if (sift_match_clause((Node *) ri->clause, ix, &q))
			{
				if (found || !runtime_query(q))
					return false;
				found = ri;
				h->query = q;
				h->match_expr = ri->clause;
			}
		if (found)
		{
			h->text = ix;
			h->match = found;
			return true;
		}
	}
	return false;
}

static bool
no_score(HybridPlan *h, Expr *clause)
{
	ScoreRefs	refs = {h->rel->relid, NIL};

	score_walker((Node *) clause, &refs);
	return refs.found == NIL;
}

static bool
has_param(Node *node, void *context)
{
	return node != NULL && (IsA(node, Param) || expression_tree_walker(node, has_param, context));
}

/* (col = ANY (set)) IS NOT UNKNOWN: the hint of hybrid_semi_plan.c, which no row fails */
static bool
is_key_set(Expr *clause)
{
	return IsA(clause, BooleanTest) && ((BooleanTest *) clause)->booltesttype == IS_NOT_UNKNOWN && IsA(((BooleanTest *) clause)->arg, ScalarArrayOpExpr);
}

/* A sift_table decides the comparisons with a constant and the NULL tests, all of them: no residual, no parameter, no key set */
static bool
classify_table_quals(HybridPlan *h)
{
	foreach_node(RestrictInfo, ri, h->rel->baserestrictinfo)
	{
		Expr	   *q;

		if (ri == h->match)
			continue;
		if (ri->pseudoconstant || contain_volatile_functions((Node *) ri->clause) || !no_score(h, ri->clause) || (q = table_qual(&h->store, ri->clause)) == NULL)
			return false;
		h->quals = lappend(h->quals, q);
		h->qinfo = lappend_int(lappend_int(lappend_int(h->qinfo, SHQ_STD), 0), 0);
		h->clauses = lappend(h->clauses, ri->clause);
	}
	return true;
}

static bool
classify_quals(HybridPlan *h)
{
	h->params = has_param((Node *) h->query, NULL);
	if (h->store.is_table)
		return !h->params && classify_table_quals(h);
	foreach_node(RestrictInfo, ri, h->rel->baserestrictinfo)
	{
		SiftHybQual q;
		Expr	   *norm;

		if (ri == h->match)
			continue;
		if (ri->pseudoconstant || contain_volatile_functions((Node *) ri->clause) || !no_score(h, ri->clause))
			return false;
		if (is_key_set(ri->clause))
		{
			h->keyset = ri->clause;
			if (sift_hybrid_qual_pushable(h->store.index, h->colrel, ((BooleanTest *) ri->clause)->arg, &q, &norm) && q.kind == SHQ_STD)
			{
				h->quals = lappend(h->quals, norm);
				h->qinfo = lappend_int(lappend_int(lappend_int(h->qinfo, SHQ_KEYSET), q.col), 0);
			}
			continue;
		}
		h->clauses = lappend(h->clauses, ri->clause);
		if (sift_hybrid_qual_pushable(h->store.index, h->colrel, ri->clause, &q, &norm))
		{
			h->quals = lappend(h->quals, norm);
			h->qinfo = lappend_int(lappend_int(lappend_int(h->qinfo, q.kind), q.col), q.fold);
			h->params |= has_param((Node *) norm, NULL);
		}
		else
		{
			h->residual = true;
			h->residuals = lappend(h->residuals, ri->clause);
		}
	}
	return true;
}

/* One branch of the OR of the match: its conjuncts, each a qual the store decides and none reading a score; the quals and their qinfo are added */
static bool
or_branch(HybridPlan *h, Expr *branch, List **quals, List **qinfo)
{
	foreach_ptr(Expr, c, is_andclause((Node *) branch) ? ((BoolExpr *) branch)->args : list_make1(branch))
	{
		SiftHybQual q = {SHQ_STD, 0, false};
		Expr	   *norm;

		if (contain_mutable_functions((Node *) c) || !no_score(h, c))
			return false;
		if (h->store.is_table)
			norm = table_qual(&h->store, c);
		else if (!sift_hybrid_qual_pushable(h->store.index, h->colrel, c, &q, &norm))
			norm = NULL;
		if (norm == NULL)
			return false;
		*quals = lappend(*quals, norm);
		*qinfo = lappend_int(lappend_int(lappend_int(*qinfo, q.kind), q.col), q.fold);
	}
	return true;
}

/* A text match ORed with structured quals the store decides (the match is a constant query): the OR is the scan's match, the other branch its quals */
static bool
find_or_match(HybridPlan *h)
{
	foreach_ptr(IndexOptInfo, ix, h->rel->indexlist)
	{
		if (!sift_v6_text_index(ix))
			continue;
		foreach_node(RestrictInfo, ri, h->rel->baserestrictinfo)
		{
			List	   *args = is_orclause((Node *) ri->clause) ? ((BoolExpr *) ri->clause)->args : NIL;

			if (list_length(args) != 2 || ri->pseudoconstant || contain_volatile_functions((Node *) ri->clause))
				continue;
			for (int side = 0; side < 2; side++)
			{
				Expr	   *m = list_nth(args, side),
						   *q;
				List	   *quals = NIL,
						   *qinfo = NIL;

				if (!sift_match_clause((Node *) m, ix, &q) || !IsA(q, Const) || ((Const *) q)->constisnull || !or_branch(h, list_nth(args, 1 - side), &quals, &qinfo))
					continue;
				*h = (HybridPlan) {.root = h->root,.rel = h->rel,.text = ix,.store = h->store,.colrel = h->colrel,.match = ri,.match_expr = m,.disjunct = true,
					.query = q,.quals = quals,.qinfo = qinfo};
				return true;
			}
		}
	}
	return false;
}

/* The store gives a column of the query's rows back: of a type it decodes, or an array whose image it keeps */
static bool
store_holds(HybridPlan *h, Var *v)
{
	if (v->varattno == SelfItemPointerAttributeNumber)
		return true;
	if (store_attr(&h->store, v->varattno) < 0)
		return false;
	if (h->store.is_table)
		return table_col_exact(&h->store, v->varattno);
	for (int i = 0; i < h->store.index->ncolumns; i++)
		if (h->store.index->indexkeys[i] == v->varattno)
		{
			const SiftColDef *d = &sift_col_rel(h->colrel)->schema->cols[sift_col_rel(h->colrel)->first[i + 1]];

			return sift_hybrid_type_usable(v->vartype) || (sift_col_def_arrlen_head(d) && !(d->flags & SIFT_COLD_NOIMAGE) &&
														   (v->vartype == TEXTARRAYOID || v->vartype == INT4ARRAYOID || v->vartype == INT8ARRAYOID));
		}
	return false;
}

static bool
store_covers(HybridPlan *h)
{
	List	   *vars = pull_var_clause((Node *) list_concat_copy(h->rel->reltarget->exprs, h->clauses), PVC_RECURSE_PLACEHOLDERS);

	foreach_node(Var, v, vars)
		if ((Index) v->varno == h->rel->relid && !store_holds(h, v))
			return false;
	return true;
}

/* The keys of the set of an array an init plan makes, as the planner estimates them (the plan's LIMIT bounds them) */
static double
init_plan_rows(PlannerInfo *root, Param *param)
{
	if (param->paramkind == PARAM_EXEC)
		foreach_node(SubPlan, sp, root->init_plans)
			if (list_member_int(sp->setParam, param->paramid))
				return ((Plan *) list_nth(root->glob->subplans, sp->plan_id - 1))->plan_rows;
	return 0;
}

/* The keys a plan that probes the table by the tenant's key set reads, when a btree starts on the column the set names and the set is known (not a parameter's value); else 0. A query that reads the score gets it from the text index, so no probe serves it */
static double
probe_rows(HybridPlan *h)
{
	ScalarArrayOpExpr *sa;
	Node	   *col,
			   *set;

	if (h->keyset == NULL || h->params || h->ranked || score_exprs(h->root, h->rel) != NIL || !enable_nestloop || !enable_indexscan)
		return 0;
	sa = (ScalarArrayOpExpr *) ((BooleanTest *) h->keyset)->arg;
	col = linitial(sa->args);
	set = lsecond(sa->args);
	while (IsA(col, RelabelType))
		col = (Node *) ((RelabelType *) col)->arg;
	if (!IsA(col, Var) || !IsA(set, Param))
		return 0;
	foreach_ptr(IndexOptInfo, ix, h->rel->indexlist)
		if (ix->relam == BTREE_AM_OID && ix->indpred == NIL && ix->indexkeys[0] == ((Var *) col)->varattno)
			return init_plan_rows(h->root, (Param *) set);
	return 0;
}

static void
estimate(HybridPlan *h, SiftHybridEst *e)
{
	RelOptInfo *rel = h->rel;
	double		rows = Max(rel->tuples, 1.0),
				granules = Max(1.0, ceil(rows / SIFT_GRANULE_ROWS)),
				sel_col = h->quals == NIL ? 1.0 : clauselist_selectivity(h->root, h->quals, rel->relid, JOIN_INNER, NULL),
				cold = 1.0 - Min(1.0, (double) effective_cache_size / Max((double) rel->pages, 1.0));

	*e = (SiftHybridEst) {
		.host = h->store.is_table ? SIFT_HYB_HOST_TABLE : SIFT_HYB_HOST_COL,.rows = rows,.granules = granules,
		.col_granules = granules * sift_hybrid_granule_hit(sel_col, rows / granules),
		.heap_fetch = h->store.is_table ? SIFT_TABLE_VET_OPS * cpu_operator_cost : cpu_tuple_cost + random_page_cost * cold,.row_fetch = sift_v6_fetch_cost(h->store.is_table) + random_page_cost * cold,.sel_col = sel_col,
		.sel_bm25 = clause_selectivity(h->root, (Node *) h->match_expr, rel->relid, JOIN_INNER, NULL),
		.limit = h->k,.hit_budget = work_mem * 1024.0,.ranked = h->ranked,.has_col_quals = h->quals != NIL,.covered = store_covers(h),
		.sel_acl = 1.0,.has_residual = h->residual,.bm25_dict_cost = sift_dict_cost(h->text, list_make1(h->query)),.allvisfrac = rel->allvisfrac
	};
	e->probe_rows = probe_rows(h);
	e->probeable = e->probe_rows > 0;
}

/* The numbers the executor needs to choose a direction once the query is bound (SIFT_HYB_ADAPTIVE) */
static List *
estimate_consts(const SiftHybridEst *e)
{
	const double v[] = {e->rows, e->granules, e->col_granules, e->heap_fetch, e->sel_col, e->limit, e->allvisfrac};
	List	   *l = NIL;

	for (int i = 0; i < SIFT_NHYBE; i++)
		l = lappend(l, makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8), Float8GetDatum(v[i]), false, FLOAT8PASSBYVAL));
	return l;
}

static void
add_path_of(HybridPlan *h, const SiftHybridEst *e, SiftHybridDir dir, double cost)
{
	CustomPath *cp = makeNode(CustomPath);
	List	   *ints = list_make5_int(dir, h->store.is_table ? SIFT_HYB_HOST_TABLE : SIFT_HYB_HOST_COL, (int) h->text->indexoid,
									   (int) (h->store.is_table ? h->store.relid : h->store.index->indexoid), (int) h->k);

	ints = lappend_int(ints, h->ranked);
	ints = lappend_int(ints, 0);
	ints = lappend_int(ints, e->covered);
	ints = lappend_int(ints, h->residual);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = h->rel;
	cp->path.pathtarget = h->rel->reltarget;
	cp->path.rows = clamp_row_est(e->rows * (dir == SIFT_HYB_OR ? e->sel_bm25 + e->sel_col - e->sel_bm25 * e->sel_col : e->sel_bm25 * e->sel_col));
	cp->path.startup_cost = dir == SIFT_HYB_COL_TO_BM25 || dir == SIFT_HYB_OR ? cost : cost * 0.5;
	cp->path.total_cost = cost;
	cp->path.pathkeys = h->ranked ? list_make1(h->pk) : NIL;
	cp->path.parallel_safe = false;
	cp->path.parallel_aware = false;
	cp->methods = &sift_hybrid_path_methods;
	cp->custom_private = list_make5(ints, h->qinfo, lcons(h->query, h->quals), h->clauses, estimate_consts(e));
	add_path(h->rel, &cp->path);
}

/* The cost of a bound query is known only at execution: the adaptive scan is priced just under the best path the planner has for a guessed one */
static double
cheapest_total(RelOptInfo *rel)
{
	double		best = DBL_MAX;

	foreach_ptr(Path, p, rel->pathlist)
		best = Min(best, p->total_cost);
	return best;
}

/* match OR quals: a constant query, the quals that are all the OR's other branch and no other, no score read, no order wanted */
static bool
or_ok(HybridPlan *h)
{
	foreach_node(RestrictInfo, ri, h->rel->baserestrictinfo)
		if (ri != h->match && is_orclause((Node *) ri->clause))
			return false;
	return classify_quals(h) && h->keyset == NULL && !h->params && score_exprs(h->root, h->rel) == NIL;
}

static bool
has_or_clause(RelOptInfo *rel)
{
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		if (is_orclause((Node *) ri->clause))
			return true;
	return false;
}

static void
add_or_path(HybridPlan *h)
{
	SiftHybridEst e;

	estimate(h, &e);
	add_path_of(h, &e, SIFT_HYB_OR, sift_hybrid_or_cost(&e));
}

static void
add_choice_paths(HybridPlan *h)
{
	SiftHybridEst e;
	SiftHybridCosts costs;
	SiftHybridDir dir;

	h->ranked = sorted_by_score(h->root, h->rel, &h->pk);
	h->k = h->ranked && !h->residual ? scan_limit(h->root, true) : 0;
	estimate(h, &e);
	dir = sift_hybrid_choose(&e, &costs);
	if (h->params && sift_hybrid_both_apply(&e))
		add_path_of(h, &e, SIFT_HYB_ADAPTIVE, Min(Min(costs.col_to_bm25, costs.bm25_to_col), cheapest_total(h->rel) * SIFT_HYB_WIN));
	else if (dir != SIFT_HYB_NONE)
		add_path_of(h, &e, !h->store.is_table && sift_hybrid_both_apply(&e) ? SIFT_HYB_ADAPTIVE : dir, dir == SIFT_HYB_COL_TO_BM25 ? costs.col_to_bm25 : costs.bm25_to_col);
}

static void
sift_hybrid_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	HybridPlan	h = {.root = root,.rel = rel};
	bool		anded;

	if (prev_pathlist_hook)
		prev_pathlist_hook(root, rel, rti, rte);
	if (!sift_query_touches_sift(root) || !shape_ok(root, rel, rte))
		return;
	anded = find_match(&h);
	if (!(anded ? pick_store(&h, rte) : has_or_clause(rel) && pick_store(&h, rte) && find_or_match(&h)))
	{
		close_store(&h);
		return;
	}
	if (h.disjunct)
	{
		if (or_ok(&h))
			add_or_path(&h);
	}
	else if (classify_quals(&h) && h.quals != NIL)
		add_choice_paths(&h);
	close_store(&h);
}

/* Vars of the rel the scan hands to its parent and the quals read, then the sift.score expressions the query reads */
static List *
scan_tlist(PlannerInfo *root, RelOptInfo *rel, List *quals)
{
	List	   *vars = pull_var_clause((Node *) list_concat_copy(rel->reltarget->exprs, quals), PVC_RECURSE_PLACEHOLDERS),
			   *exprs = NIL,
			   *tlist = NIL;
	int			resno = 1;

	foreach_node(Var, v, vars)
		if ((Index) v->varno == rel->relid)
			exprs = list_append_unique(exprs, v);
	exprs = list_concat(exprs, list_copy(score_exprs(root, rel)));
	foreach_ptr(Node, e, exprs)
		tlist = lappend(tlist, makeTargetEntry((Expr *) e, resno++, NULL, false));
	return tlist;
}

static List *
score_outputs(PlannerInfo *root, RelOptInfo *rel, List *tlist)
{
	foreach_ptr(Node, e, score_exprs(root, rel))
		if (tlist_member((Expr *) e, tlist) == NULL)
			tlist = lappend(list_copy(tlist), makeTargetEntry((Expr *) e, list_length(tlist) + 1, NULL, true));
	return tlist;
}

static Plan *
sift_hybrid_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	List	   *ints = linitial(best->custom_private),
			   *exprs = lthird(best->custom_private),
			   *all = lfourth(best->custom_private),
			   *kept = NIL;

	foreach_node(RestrictInfo, ri, clauses)
		if (list_member(all, ri->clause))
			kept = lappend(kept, ri->clause);
	cs->scan.plan.targetlist = score_outputs(root, rel, tlist);
	cs->scan.plan.qual = kept;
	cs->scan.scanrelid = rel->relid;
	cs->flags = best->flags;
	cs->custom_scan_tlist = scan_tlist(root, rel, list_concat_copy(list_copy(all), exprs));
	cs->custom_exprs = exprs;
	cs->custom_private = list_make2(ints, lsecond(best->custom_private));
	cs->custom_private = lappend(cs->custom_private, list_nth(best->custom_private, 4));
	cs->methods = &sift_hybrid_plan_methods;
	root->glob->relationOids = lappend_oid(root->glob->relationOids, (Oid) list_nth_int(ints, SIFT_HYBP_TEXT_INDEX));
	root->glob->relationOids = lappend_oid(root->glob->relationOids, (Oid) list_nth_int(ints, SIFT_HYBP_STORE_OID));
	return &cs->scan.plan;
}

const CustomPathMethods sift_hybrid_path_methods = {"SiftHybridScan", sift_hybrid_plan_path, NULL};

/* ---- SiftHybridAgg: the groups folded from the rows of the store, where the statement is one the hybrid scan serves and its aggregates are those of agg/ ---- */

static bool
is_exists_subplan(Node *n)
{
	if (IsA(n, SubPlan))
		return ((SubPlan *) n)->subLinkType == EXISTS_SUBLINK;
	return IsA(n, AlternativeSubPlan) && is_exists_subplan(linitial(((AlternativeSubPlan *) n)->subplans));
}

/* [(] EXISTS (..) [) IS TRUE]: the sub-select of the planner's key set (agg/hybrid/semi_plan.c) says it as a SubPlan, or the choice between its plans */
static bool
is_exists_clause(Expr *clause)
{
	Node	   *n = (Node *) clause;

	if (IsA(n, BooleanTest) && ((BooleanTest *) n)->booltesttype == IS_TRUE)
		n = (Node *) ((BooleanTest *) n)->arg;
	return is_exists_subplan(n);
}

/* The keys and aggregates of the groups, then the Vars of the rel that the quals read: the executor finds the columns of its quals in the entries of the scan's tlist */
static List *
agg_scan_tlist(RelOptInfo *rel, List *scan_tlist, List *exprs)
{
	List	   *tlist = list_copy(scan_tlist);

	foreach_node(Var, v, pull_var_clause((Node *) exprs, PVC_RECURSE_PLACEHOLDERS))
		if ((Index) v->varno == rel->relid && tlist_member((Expr *) v, tlist) == NULL)
			tlist = lappend(tlist, makeTargetEntry((Expr *) copyObject(v), list_length(tlist) + 1, NULL, false));
	return tlist;
}

static const CustomPathMethods sift_hybrid_agg_path_methods;

/* A sift.score the statement reads outside an aggregate belongs to a row the folded scan does not return */
static bool
score_outside_aggs(Node *n, Index *relid)
{
	if (n == NULL || IsA(n, Aggref))
		return false;
	return sift_is_score_expr(n, *relid, NULL) || expression_tree_walker(n, score_outside_aggs, relid);
}

static void
add_agg_path(HybridPlan *h, const SiftHybridEst *e, SiftHybridDir dir, double cost, RelOptInfo *out, const SiftV6Spec *spec, List *scan_tlist, List *group_exprs, List *having)
{
	CustomPath *cp = makeNode(CustomPath);
	List	   *ints = list_make5_int(dir, h->store.is_table ? SIFT_HYB_HOST_TABLE : SIFT_HYB_HOST_COL, (int) h->text->indexoid,
									   (int) (h->store.is_table ? h->store.relid : h->store.index->indexoid), 0),
			   *qinfo = list_copy(h->qinfo),
			   *exprs = lcons(h->query, list_copy(h->quals));
	const double groups = clamp_row_est((spec->nkeys == 0 ? 1.0 : estimate_num_groups(h->root, group_exprs, e->rows * e->sel_bm25 * e->sel_col, NULL, NULL)) * Max(spec->nsets, 1));

	ints = lappend_int(lappend_int(lappend_int(lappend_int(ints, false), 0), true), h->residual);
	foreach_ptr(Expr, c, h->residuals)
	{
		exprs = lappend(exprs, c);
		qinfo = lappend_int(lappend_int(lappend_int(qinfo, is_exists_clause(c) ? SHQ_EXISTS : SHQ_RESIDUAL), 0), 0);
	}
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = out;
	cp->path.pathtarget = out->reltarget;
	cp->path.rows = groups;
	cp->path.startup_cost = cp->path.total_cost = cost + cpu_tuple_cost * groups;
	cp->methods = &sift_hybrid_agg_path_methods;
	cp->custom_private = list_make5(list_make3(ints, qinfo, estimate_consts(e)), exprs, sift_v6_spec_pack(spec), agg_scan_tlist(h->rel, scan_tlist, exprs), list_make2(having, makeInteger(list_length(scan_tlist))));
	add_path(out, &cp->path);
}

/* The matches of an OR are folded from the heap's rows: every column the aggregate reads is of a type those rows are read in */
static bool
or_columns_readable(HybridPlan *h, List *scan_tlist)
{
	foreach_node(Var, v, pull_var_clause((Node *) scan_tlist, PVC_RECURSE_AGGREGATES | PVC_RECURSE_PLACEHOLDERS))
		if ((Index) v->varno == h->rel->relid && v->varattno != SelfItemPointerAttributeNumber && !sift_hybrid_point_type(v->vartype))
			return false;
	return true;
}

/* A text match ORed with structured quals: the OR alone in the restrictions of the rel, the quals' rows folded by the store and the other matches by the heap */
static void
sift_hybrid_or_agg_path(HybridPlan *h, RelOptInfo *rel, RelOptInfo *out, void *extra)
{
	SiftV6Spec	spec;
	List	   *scan_tlist,
			   *group_exprs;
	SiftHybridEst e;

	if (list_length(rel->baserestrictinfo) != 1 || !or_ok(h) ||
		!sift_v6_compile_aggregate(h->root, rel, out, h->store.index, h->colrel, h->store.is_table ? &h->store.tsch : NULL, extra, &spec, &scan_tlist, &group_exprs) ||
		sift_v6_spec_reads_score(&spec) || !or_columns_readable(h, scan_tlist) || score_outside_aggs((Node *) h->root->processed_tlist, &rel->relid))
		return;
	estimate(h, &e);
	e.covered = true;
	add_agg_path(h, &e, SIFT_HYB_OR, sift_hybrid_or_agg_cost(&e), out, &spec, scan_tlist, group_exprs, (List *) ((GroupPathExtraData *) extra)->havingQual);
}

static void
sift_hybrid_agg_paths(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, void *extra)
{
	HybridPlan	h = {.root = root,.rel = rel};
	SiftV6Spec	spec;
	List	   *scan_tlist,
			   *group_exprs;
	SiftHybridEst e;
	SiftHybridCosts costs;
	SiftHybridDir dir;
	bool		anded;

	if (!sift_v6_plan_shape(root, rel, out) || !shape_ok(root, rel, planner_rt_fetch(rel->relid, root)))
		return;
	anded = find_match(&h);
	if (!(anded ? pick_store(&h, planner_rt_fetch(rel->relid, root)) : has_or_clause(rel) && pick_store(&h, planner_rt_fetch(rel->relid, root)) && find_or_match(&h)))
	{
		close_store(&h);
		return;
	}
	if (h.disjunct)
		sift_hybrid_or_agg_path(&h, rel, out, extra);
	else if (classify_quals(&h) && sift_v6_compile_aggregate(root, rel, out, h.store.index, h.colrel, h.store.is_table ? &h.store.tsch : NULL, extra, &spec, &scan_tlist, &group_exprs) &&
			 !score_outside_aggs((Node *) root->processed_tlist, &rel->relid) && !score_outside_aggs((Node *) ((GroupPathExtraData *) extra)->havingQual, &rel->relid))
	{
		List	   *having = (List *) ((GroupPathExtraData *) extra)->havingQual;

		estimate(&h, &e);
		if (sift_v6_search_key_first(root, rel, e.rows * e.sel_bm25))
		{
			sift_agg_refuse("search", AGG_REFUSE_COST, "a scan of a btree on the other conditions reads fewer rows than the scan walks postings", 0, -1);
			close_store(&h);
			return;
		}
		e.covered = true;
		e.reads_score = sift_v6_spec_reads_score(&spec);
		dir = sift_hybrid_agg_choose(&e, &costs);
		if (h.params && sift_hybrid_both_apply(&e))
			add_agg_path(&h, &e, SIFT_HYB_ADAPTIVE, Min(Min(costs.col_to_bm25, costs.bm25_to_col), cheapest_total(rel) * SIFT_HYB_WIN), out, &spec, scan_tlist, group_exprs, having);
		else if (dir != SIFT_HYB_NONE)
			add_agg_path(&h, &e, !h.store.is_table && sift_hybrid_both_apply(&e) ? SIFT_HYB_ADAPTIVE : dir, dir == SIFT_HYB_COL_TO_BM25 ? costs.col_to_bm25 : costs.bm25_to_col, out, &spec, scan_tlist, group_exprs, having);
	}
	close_store(&h);
}

static Plan *
sift_hybrid_agg_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	List	   *priv = linitial(best->custom_private),
			   *ints = linitial(priv);

	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = linitial(list_nth_node(List, best->custom_private, 4));
	cs->scan.scanrelid = 0;
	cs->flags = best->flags;
	cs->custom_exprs = lsecond(best->custom_private);
	cs->custom_scan_tlist = lfourth(best->custom_private);
	cs->custom_private = list_make5(ints, lsecond(priv), lthird(priv), lthird(best->custom_private), lsecond(list_nth_node(List, best->custom_private, 4)));
	cs->methods = &sift_hybrid_agg_plan_methods;
	root->glob->relationOids = lappend_oid(root->glob->relationOids, (Oid) list_nth_int(ints, SIFT_HYBP_TEXT_INDEX));
	root->glob->relationOids = lappend_oid(root->glob->relationOids, (Oid) list_nth_int(ints, SIFT_HYBP_STORE_OID));
	return &cs->scan.plan;
}

static const CustomPathMethods sift_hybrid_agg_path_methods = {"SiftHybridAgg", sift_hybrid_agg_plan_path, NULL};

static void
sift_hybrid_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	if (prev_upper_hook)
		prev_upper_hook(root, stage, input_rel, output_rel, extra);
	if (stage == UPPERREL_GROUP_AGG && sift_query_touches_sift(root) && input_rel->reloptkind == RELOPT_BASEREL)
		sift_hybrid_agg_paths(root, input_rel, output_rel, extra);
}

void
sift_hybrid_init(void)
{
	RegisterCustomScanMethods(&sift_hybrid_plan_methods);
	RegisterCustomScanMethods(&sift_hybrid_agg_plan_methods);
	sift_hybrid_semi_init();
	prev_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = sift_hybrid_pathlist;
	prev_upper_hook = create_upper_paths_hook;
	create_upper_paths_hook = sift_hybrid_upper_paths;
}
