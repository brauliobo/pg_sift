/*
 * agg/join/push.c - the path and the node of SiftAggV6Push (v6_join_push.h).
 */
/* check-memory: allow-allocs the arrays follow the statement and the values of the small side of the join, which the planner bounded */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_opfamily_d.h"
#include "catalog/pg_type_d.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "executor/nodeIndexscan.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "parser/parsetree.h"
#include "utils/selfuncs.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/safety.h"
#include "agg/agg.h"
#include "agg/plan/int.h"
#include "agg/engine.h"
#include "agg/explain.h"
#include "agg/cost/card.h"
#include "agg/dim_keys.h"
#include "agg/join/join.h"
#include "agg/join/lookup.h"
#include "agg/settings.h"
#include "agg/join/push.h"
#include "agg/qual/qual.h"
#include "agg/source_rel.h"
#include "agg/vexpr.h"
#include "col/am.h"
#include "col/col.h"
#include "core/store/ops.h"
#include "core/compat.h"
#include "core/cpu.h"
#include "core/memtrack.h"
#include "table/dispatch.h"
#include "core/cmp.h"

/* A small side of more rows than this stays with the join: the array is a key of the scan */
#define PUSH_ROWS_MAX	50000
#define PUSH_RUNTIME_CHUNK (PUSH_ROWS_MAX * 2)

typedef struct PushJoin
{
	RelOptInfo *fact;
	RelOptInfo *dim;
	Path	   *dim_path;		/* the plan the small side is read with */
	Var		   *dim_var;
	AttrNumber	fact_attno;
	Oid			opno;
	bool		semi;
	Relids		semi_rhs;		/* the relations under the EXISTS or IN of a semi join */
} PushJoin;

static bool
int_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

static bool
plain_table(PlannerInfo *root, RelOptInfo *rel)
{
	const RangeTblEntry *rte;

	if (rel->reloptkind != RELOPT_BASEREL || rel->lateral_relids != NULL)
		return false;
	rte = planner_rt_fetch(rel->relid, root);
	return sift_v6_rte_stored(rte) && !rte->inh && rte->tablesample == NULL && agg_rel_security_ok(root, rel);
}

static bool
join_vars(const Var *a, const Var *b, PushJoin *j)
{
	const bool	fact_first = (Index) a->varno == j->fact->relid;
	const Var  *f = fact_first ? a : b,
			   *d = fact_first ? b : a;

	if ((Index) f->varno != j->fact->relid || (Index) d->varno != j->dim->relid || !int_type(f->vartype) || !int_type(d->vartype))
		return false;
	j->opno = get_opfamily_member(INTEGER_BTREE_FAM_OID, f->vartype, d->vartype, BTEqualStrategyNumber);
	j->fact_attno = f->varattno;
	j->dim_var = copyObject((Var *) d);
	return OidIsValid(j->opno);
}

static bool
class_joins(const EquivalenceClass *ec, const PushJoin *j)
{
	return !ec->ec_has_const && bms_is_member(j->fact->relid, ec->ec_relids) && bms_is_member(j->dim->relid, ec->ec_relids);
}

static bool
class_vars(const EquivalenceClass *ec, PushJoin *j)
{
	const Expr *a,
			   *b;

	if (ec->ec_has_volatile || list_length(ec->ec_members) != 2)
		return false;
	a = ((EquivalenceMember *) linitial(ec->ec_members))->em_expr;
	b = ((EquivalenceMember *) lsecond(ec->ec_members))->em_expr;
	return IsA(a, Var) && IsA(b, Var) && join_vars((const Var *) a, (const Var *) b, j);
}

static bool
equal_vars(const OpExpr *op, PushJoin *j)
{
	return IsA(op, OpExpr) && list_length(op->args) == 2 && IsA(linitial(op->args), Var) && IsA(lsecond(op->args), Var) &&
		get_op_opfamily_strategy(op->opno, INTEGER_BTREE_FAM_OID) == BTEqualStrategyNumber && join_vars(linitial(op->args), lsecond(op->args), j);
}

/* The join of the two relations is one clause, fact.col = dim.col over integers, that the key of the scan decides: every other clause that holds a column of each (a class of an expression or a third column, an inequality, an OR) stays with PostgreSQL's join. The clause is the one equivalence class that holds both relations, or the one clause of the semi-join of an IN (sub-select) that no class holds */
static bool
find_join(PlannerInfo *root, PushJoin *j)
{
	const EquivalenceClass *joining = NULL;
	int			clauses = list_length(j->dim->joininfo);

	j->semi_rhs = root->join_info_list != NIL ? ((const SpecialJoinInfo *) linitial(root->join_info_list))->syn_righthand : NULL;
	j->semi = j->semi_rhs != NULL;
	foreach_node(EquivalenceClass, ec, root->eq_classes)
		if (class_joins(ec, j))
		{
			joining = ec;
			clauses++;
		}
	if (clauses != 1 || list_length(j->fact->joininfo) != list_length(j->dim->joininfo))
		return false;
	if (joining == NULL)
		return j->semi && equal_vars((const OpExpr *) ((RestrictInfo *) linitial(j->dim->joininfo))->clause, j);
	return class_vars(joining, j);
}

#define PUSH_PINNED_MAX 64

typedef struct Pinned
{
	int			n;
	Bitmapset  *atts;
	AttrNumber	attno[PUSH_PINNED_MAX];
	Oid			opno[PUSH_PINNED_MAX];
} Pinned;

/* The columns of the dim that the statement pins to a constant, with the operator that pins them: one pass over its restrictions */
static void
collect_pinned(const RelOptInfo *dim, Pinned *pin)
{
	pin->n = 0;
	pin->atts = NULL;
	foreach_node(RestrictInfo, ri, dim->baserestrictinfo)
	{
		const OpExpr *op = (const OpExpr *) ri->clause;
		const Var  *v;

		if (pin->n == PUSH_PINNED_MAX || !IsA(op, OpExpr) || list_length(op->args) != 2)
			continue;
		v = linitial(op->args);
		if (IsA(v, Var) && (Index) v->varno == dim->relid && v->varattno > 0 && IsA(lsecond(op->args), Const) && !((Const *) lsecond(op->args))->constisnull)
		{
			pin->attno[pin->n] = v->varattno;
			pin->opno[pin->n++] = op->opno;
			pin->atts = bms_add_member(pin->atts, v->varattno - FirstLowInvalidHeapAttributeNumber);
		}
	}
}

static bool
pinned_to_const(const Pinned *pin, AttrNumber attno, Oid opfamily)
{
	if (!bms_is_member(attno - FirstLowInvalidHeapAttributeNumber, pin->atts))
		return false;
	for (int i = 0; i < pin->n; i++)
		if (pin->attno[i] == attno && get_op_opfamily_strategy(pin->opno[i], opfamily) == BTEqualStrategyNumber)
			return true;
	return false;
}

/* Does the small side hold each value of the join column once: a unique index whose columns are the join column and columns the statement pins to a constant */
static bool
dim_unique(const PushJoin *j)
{
	Pinned		pin;

	collect_pinned(j->dim, &pin);
	foreach_ptr(IndexOptInfo, ix, j->dim->indexlist)
	{
		bool		all = ix->unique && ix->indpred == NIL && ix->indexprs == NIL && !ix->hypothetical && ix->nkeycolumns > 0;

		for (int k = 0; all && k < ix->nkeycolumns; k++)
			all = ix->indexkeys[k] == j->dim_var->varattno || pinned_to_const(&pin, ix->indexkeys[k], ix->opfamily[k]);
		if (all)
			return true;
	}
	return false;
}

/* The small side of a semi join is the relation under the EXISTS or IN: the aggregates read the rows of the other one */
static bool
semi_inner(const PushJoin *j)
{
	return bms_is_member(j->dim->relid, j->semi_rhs);
}

/* The position (0-based) in the index of the column attno, -1 when it is not a key column */
static int
index_pos(const IndexOptInfo *ix, AttrNumber attno)
{
	for (int i = 0; i < ix->ncolumns; i++)
		if (ix->indexkeys[i] == attno)
			return i;
	return -1;
}

/* Is the fact a sift_table of format major 2 whose join column the scan reads as integers */
static bool
table_holds_join_column(PlannerInfo *root, const PushJoin *j)
{
	const Oid	relid = planner_rt_fetch(j->fact->relid, root)->relid;
	const SiftStoreOps *ops = sift_store_ops_for_oid(relid, true);

	return ops != NULL && ops->zone != NULL && ops->leaf_holds(relid, true, j->fact_attno, SV6C_I64);
}

/* The scan of the index ix of the fact (ix NULL: of the sift_table) that the statement's quals and aggregates compile to, and the join column as a column of the index the scan reads as integers */
static bool
compile_fact(PlannerInfo *root, const PushJoin *j, RelOptInfo *out, void *extra, IndexOptInfo *ix, int *int_col, SiftV6Spec *spec, List **scan_tlist)
{
	const SiftStoreOps *ops;

	if (ix == NULL)
	{
		*int_col = j->fact_attno - 1;
		return table_holds_join_column(root, j) && sift_v6_compile_fact(root, j->fact, out, NULL, extra, spec, scan_tlist);
	}
	if (!sift_v6_index_owned(ix))
		return false;
	*int_col = index_pos(ix, j->fact_attno);
	ops = sift_store_ops_for_oid(ix->indexoid, false);
	return *int_col >= 0 && ops->zone != NULL && ops->leaf_holds(ix->indexoid, false, *int_col + 1, SV6C_I64) &&
		sift_v6_compile_fact(root, j->fact, out, ix, extra, spec, scan_tlist);
}

static ScalarArrayOpExpr *
key_template(const PushJoin *j, int int_col, Oid fact_type)
{
	ScalarArrayOpExpr *q = makeNode(ScalarArrayOpExpr);
	Oid			arrtype = get_array_type(j->dim_var->vartype);

	q->opno = j->opno;
	q->opfuncid = get_opcode(j->opno);
	q->useOr = true;
	q->inputcollid = InvalidOid;
	q->location = -1;
	q->args = list_make2(makeVar(INDEX_VAR, int_col + 1, fact_type, -1, InvalidOid, 0), makeNullConst(arrtype, -1, InvalidOid));
	return q;
}

static Plan *push_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);

static const CustomPathMethods push_path_methods = {"SiftAggV6Push", push_plan_path, NULL};

/* The scope of the index whose summary keeps the scan to the granules of the one key the statement binds the small side to: (ordinal, key expression), NIL for none */
static List *
scope_route(PlannerInfo *root, const PushJoin *j, IndexOptInfo *ix)
{
	int			ord;
	Expr	   *key;

	const SiftStoreOps *ops = ix != NULL ? sift_store_ops_for_oid(ix->indexoid, false) : NULL;

	if (ops == NULL || ops->scope == NULL || ops->scope->one_key == NULL || !ops->scope->one_key(root, j->fact, j->dim, ix, j->fact_attno, j->dim_var->varattno, &ord, &key))
		return NIL;
	return list_make2(makeInteger(ord), copyObject(key));
}

static void
add_push_path(PlannerInfo *root, const PushJoin *j, RelOptInfo *out, void *extra, IndexOptInfo *ix)
{
	SiftV6Spec	spec;
	List	   *scan_tlist = NIL,
			   *group_exprs = NIL;
	int			int_col;
	CustomPath *cp;
	double		groups;
	Cost		open,
				scan;
	const Path *dim_path = j->dim_path;

	if (!compile_fact(root, j, out, extra, ix, &int_col, &spec, &scan_tlist) || OidIsValid(spec.text_indexoid))
		return;
	group_exprs = sift_v6_group_exprs_of(root);
	groups = spec.nkeys == 0 ? 1.0 : estimate_num_groups(root, group_exprs, j->fact->rows, NULL, NULL);
	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = out;
	cp->path.pathtarget = out->reltarget;
	cp->path.rows = clamp_row_est(groups);
	if (ix != NULL)
		sift_v6_agg_cost(root, j->fact, ix, &spec, groups, &open, &scan);
	else
		sift_v6_table_spec_cost(root, j->fact, planner_rt_fetch(j->fact->relid, root)->relid, &spec, groups, &open, &scan);
	cp->path.startup_cost = dim_path->total_cost + open;
	cp->path.total_cost = dim_path->total_cost + scan + cpu_tuple_cost * groups + cpu_operator_cost * (dim_path->rows + 2.0 * j->fact->rows);
	cp->methods = &push_path_methods;
	cp->custom_paths = list_make1((Path *) dim_path);
	cp->custom_private = list_make5(sift_v6_spec_pack(&spec), scan_tlist, ((GroupPathExtraData *) extra)->havingQual,
									list_make2(key_template(j, int_col, get_atttype(planner_rt_fetch(j->fact->relid, root)->relid, j->fact_attno)), copyObject(j->dim_var)),
									scope_route(root, j, ix));
	sift_v6_force_price(&cp->path);
	add_path(out, &cp->path);
}

bool
sift_v6_push_small_side(PlannerInfo *root, RelOptInfo *rel)
{
	return plain_table(root, rel) && rel->cheapest_total_path != NULL && rel->rows <= PUSH_ROWS_MAX;
}

/* The keys a query passes own this many times the mean rows of a key and no fewer than twice it: a statistics sample and a mild unevenness of a column are not skew */
#define PUSH_SKEW_FLOOR	2.0

/* The scan of the fact a path probes once for each key of a join (a nested loop, under aggregation, sorts and projections): NULL for any other */
static const Path *
fact_probe(const Path *p, const RelOptInfo *fact)
{
	for (;;)
		switch (nodeTag(p))
		{
			case T_AggPath:
				p = ((const AggPath *) p)->subpath;
				break;
			case T_GroupPath:
				p = ((const GroupPath *) p)->subpath;
				break;
			case T_SortPath:
				p = ((const SortPath *) p)->subpath;
				break;
			case T_IncrementalSortPath:
				p = ((const IncrementalSortPath *) p)->spath.subpath;
				break;
			case T_ProjectionPath:
				p = ((const ProjectionPath *) p)->subpath;
				break;
			case T_NestPath:
				{
					const Path *inner = ((const JoinPath *) p)->innerjoinpath;

					if (IsA(inner, MemoizePath))
						inner = ((const MemoizePath *) inner)->subpath;
					return (IsA(inner, IndexPath) || IsA(inner, BitmapHeapPath)) && inner->param_info != NULL && inner->parent == fact ? inner : NULL;
				}
			default:
				return NULL;
		}
}

void
sift_v6_charge_probes(const RelOptInfo *fact, double g, double *charged, RelOptInfo *out)
{
	const double step = g / *charged;

	if (step <= 1.0)
		return;
	*charged = g;
	foreach_ptr(Path, p, out->pathlist)
		if (fact_probe(p, fact) != NULL)
		{
			p->startup_cost *= step;
			p->total_cost *= step;
		}
}

/*
 * The plans that probe the fact once for each key of the small side are priced by PostgreSQL for the mean rows of a key; a small side it has statistics of passes the keys its rows hold,
 * which own the skew of the column, so those plans cost that many times more, up to the rows the fact's own filters keep. One it guesses is charged by the star (sift_v6_lookup_keys_guess).
 */
static void
price_probes(PlannerInfo *root, const PushJoin *j, RelOptInfo *input_rel, RelOptInfo *out)
{
	double		charged = 1.0;

	if (sift_v6_lookup_keys_guess(root, j->fact, j->fact_attno, j->dim) > 1.0)
		return;
	sift_v6_charge_probes(j->fact, Min(Max(sift_v6_key_skew(root, j->fact, j->fact_attno) / PUSH_SKEW_FLOOR, 1.0), Max(j->fact->rows / Max(input_rel->rows, 1.0), 1.0)), &charged, out);
}

void
sift_agg_v6_join_push_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *out, void *extra)
{
	const Query *parse = root->parse;
	const int	a = bms_next_member(input_rel->relids, -1),
				b = bms_next_member(input_rel->relids, a);

	if (!sift_enable_agg || !parse->hasAggs || agg_query_has_rls(root) || parse->hasDistinctOn || parse->hasTargetSRFs || parse->hasWindowFuncs || parse->groupingSets != NIL ||
		bms_num_members(input_rel->relids) != 2 || out->reltarget == NULL ||
		(root->join_info_list != NIL && !(list_length(root->join_info_list) == 1 && ((SpecialJoinInfo *) linitial(root->join_info_list))->jointype == JOIN_SEMI)))
		return;
	for (int swap = 0; swap < 2; swap++)
	{
		PushJoin	j = {.fact = root->simple_rel_array[swap ? b : a],.dim = root->simple_rel_array[swap ? a : b]};

		if (j.fact == NULL || j.dim == NULL || !plain_table(root, j.fact) || !sift_v6_push_small_side(root, j.dim) || !find_join(root, &j) || !(j.semi ? semi_inner(&j) : dim_unique(&j)))
			continue;
		j.dim_path = sift_v6_dim_path(root, j.fact, j.dim, j.semi ? SV6J_SEMI_EQUI : SV6J_INNER_EQUI, 1, &j.fact_attno, (const Var *const *) &j.dim_var);
		price_probes(root, &j, input_rel, out);
		add_push_path(root, &j, out, extra, NULL);
		foreach_ptr(IndexOptInfo, ix, j.fact->indexlist)
			add_push_path(root, &j, out, extra, ix);
	}
}

static Plan *
push_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	SiftV6Spec *spec = sift_v6_spec_unpack(linitial(best->custom_private), NULL);
	List	   *join = lfourth(best->custom_private);
	List	   *scope = list_nth(best->custom_private, 4);
	const Var  *dim_var = lsecond(join);
	const int	resno = sift_v6_child_resno(((Plan *) linitial(custom_plans))->targetlist, dim_var);

	SIFT_INVARIANT(resno != 0, "sift: the small side of the join does not give its join column");
	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->flags = best->flags;
	cs->custom_plans = custom_plans;
	cs->custom_scan_tlist = lsecond(best->custom_private);
	cs->custom_private = list_make2(linitial(best->custom_private),
									list_make5(linitial(join), makeInteger(resno), makeInteger(dim_var->vartype), makeInteger(scope == NIL ? 0 : intVal(linitial(scope))),
											   makeBoolean(scope != NIL)));
	cs->custom_exprs = scope == NIL ? NIL : list_make1(lsecond(scope));
	sift_v6_collect_params((Node *) spec->indexqual, &cs->custom_exprs);
	sift_v6_collect_params((Node *) spec->vqual, &cs->custom_exprs);
	cs->methods = &sift_agg_v6_push_plan_methods;
	cs->scan.plan.qual = lthird(best->custom_private);
	root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->relid);
	return &cs->scan.plan;
}

/* ---------------------------------------------------------------------------
 * Execution
 * ------------------------------------------------------------------------- */

typedef struct PushState
{
	CustomScanState css;
	SiftV6Spec *spec;
	ScalarArrayOpExpr *key;
	int			resno;
	Oid			elemtype;
	int			scope_ord;		/* with scope_key: the scope whose summary routes the scan */
	ExprState  *scope_key;		/* the one key the statement binds the small side to, NULL when the scan is not routed */
	Oid			scope_type;
	const SiftStoreOps *ops;
	const struct SiftScopeRouter *router;
	Relation	heap;
	Relation	index;
	MemoryContext gcx;
	SiftV6Group *group;
	SiftV6ScanStats stats;
	int			nvalues;
	bool		ran;
} PushState;

static void
push_begin(CustomScanState *node, EState *estate, int eflags)
{
	PushState  *st = (PushState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	List	   *join = lsecond(cs->custom_private);

	st->spec = sift_v6_spec_unpack(linitial(cs->custom_private), estate->es_param_list_info);
	st->key = linitial(join);
	st->resno = intVal(lsecond(join));
	st->elemtype = (Oid) intVal(lthird(join));
	st->scope_ord = intVal(lfourth(join));
	if (boolVal(list_nth(join, 4)))
	{
		st->scope_key = ExecInitExpr(linitial(cs->custom_exprs), &node->ss.ps);
		st->scope_type = exprType(linitial(cs->custom_exprs));
	}
	st->ops = sift_store_ops_for_oid(st->spec->relid, st->spec->is_table);
	st->index = st->spec->is_table ? NULL : index_open(st->spec->relid, AccessShareLock);
	st->heap = table_open(st->spec->is_table ? st->spec->relid : st->index->rd_index->indrelid, AccessShareLock);
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		sift_v6_source_enter(st->spec->is_table ? st->heap : st->index, st->spec->is_table, estate->es_snapshot);
	foreach_ptr(Plan, child, cs->custom_plans)
		node->custom_ps = lappend(node->custom_ps, ExecInitNode(child, estate, eflags));
}

static int64
value_of(Oid typid, Datum d)
{
	return typid == INT2OID ? (int64) DatumGetInt16(d) : typid == INT4OID ? (int64) DatumGetInt32(d) : DatumGetInt64(d);
}

static Datum
datum_of(Oid typid, int64 v)
{
	return typid == INT2OID ? Int16GetDatum((int16) v) : typid == INT4OID ? Int32GetDatum((int32) v) : Int64GetDatum(v);
}

/* The sorted distinct values of the join column of the small side (n of them, in cx) whatever the plan estimated: the values held are bounded by work_mem */
static int64 *
small_side(PushState *st, MemoryContext cx, int *n)
{
	PlanState  *child = linitial(st->css.custom_ps);
	Size		cap = 256 * sizeof(int64);
	int64	   *v = MemoryContextAlloc(cx, cap);
	int			u = 0;

	*n = 0;
	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(child);
		bool		isnull;
		Datum		x;

		if (TupIsNull(slot))
			break;
		x = slot_getattr(slot, st->resno, &isnull);
		if (isnull)
			continue;
		if ((Size) (*n + 1) * sizeof(int64) > cap)
		{
			cap = sift_alloc_grow(cap, (Size) (*n + 1) * sizeof(int64), "the join keys of the small side of a push");
			sift_budget_check(cap, work_mem, "the join keys of the small side of a push", "work_mem", NULL);
			v = repalloc(v, cap);
		}
		v[(*n)++] = value_of(st->elemtype, x);
	}
	qsort(v, *n, sizeof(int64), sift_cmp_i64);
	for (int i = 0; i < *n; i++)
		if (u == 0 || v[i] != v[u - 1])
			v[u++] = v[i];
	*n = u;
	return v;
}

/* The values v[0 .. n - 1] as the array of the list among the keys of the scan of the fact */
static ArrayType *
values_array(const PushState *st, const int64 *v, int n)
{
	Datum	   *d = palloc(n * sizeof(Datum));
	int16		len;
	bool		byval;
	char		align;

	for (int i = 0; i < n; i++)
		d[i] = datum_of(st->elemtype, v[i]);
	get_typlenbyvalalign(st->elemtype, &len, &byval, &align);
	return construct_array(d, n, st->elemtype, len, byval, align);
}

/* The granules of the key that the summary of the scope names; NULL when the scan is not routed, the key is NULL, or the summary cannot route */
static const struct SiftScopeRouter *
scope_router(PushState *st, EState *estate)
{
	bool		isnull;
	Datum		d;
	int64		key;

	if (st->scope_key == NULL)
		return NULL;
	d = ExecEvalExprSwitchContext(st->scope_key, st->css.ss.ps.ps_ExprContext, &isnull);
	if (isnull)
		return NULL;
	key = value_of(st->scope_type, d);
	return st->ops->scope->build_router(st->index, st->scope_ord, estate->es_snapshot, &key, 1, st->gcx);
}

/* One scan of the fact for the values of ids: each value is in one batch only, so the rows of the batches are the rows of the join */
static void
push_scan(PushState *st, EState *estate, MemoryContext cx, ArrayType *ids)
{
	ScalarArrayOpExpr *q = copyObject(st->key);
	List	   *quals = list_copy(st->spec->indexqual);
	IndexRuntimeKeyInfo *rtkeys = NULL;
	ScanKey		keys;
	int			nkeys,
				nrt = 0;

	lsecond(q->args) = makeConst(get_array_type(st->elemtype), -1, InvalidOid, -1, PointerGetDatum(ids), false, false);
	quals = lappend(quals, q);
	if (st->spec->is_table)
	{
		struct SiftStoreQuals *sq = st->ops->zone->compile_quals(st->heap, quals, st->spec->jsonqual, cx);

		st->ops->zone->bind_quals(st->heap, sq, st->css.ss.ps.ps_ExprContext);
		st->ops->zone->scan_table(st->spec, st->group, st->heap, sq, estate->es_snapshot, NULL, 0, &st->stats, NULL, NULL, NULL);
	}
	else
	{
		sift_v6_qual_scankeys(&st->css.ss.ps, st->spec->jsonqual, st->index, quals, &keys, &nkeys, &rtkeys, &nrt);
		if (nrt > 0)
			ExecIndexEvalRuntimeKeys(st->css.ss.ps.ps_ExprContext, rtkeys, nrt);
		st->ops->zone->scan_run(st->spec, st->group, st->heap, st->index, keys, nkeys, estate->es_snapshot, NULL, 0, st->router, &st->stats, NULL, NULL, NULL);
	}
}

static void
push_run(PushState *st, EState *estate)
{
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	TupleDesc	desc = st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	MemoryContext chunk_cx;
	SiftMemTracker mt;
	int64	   *v;
	int			n;

	st->gcx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 push groups");
	st->ran = true;
	st->router = scope_router(st, estate);
	sift_mem_init(&mt, "agg_lookup", estate->es_query_cxt, true, SIFT_BUDGET_HASH_MEM, 1, 2);
	v = small_side(st, mt.root_cx, &n);
	sift_mem_assert_within(&mt);
	st->nvalues = n;
	st->group = sift_v6_group_new(st->spec, st->gcx, desc);
	chunk_cx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 push chunk");
	MemoryContextSwitchTo(chunk_cx);
	for (int from = 0; from < n; from += PUSH_RUNTIME_CHUNK)
	{
		MemoryContextReset(chunk_cx);
		push_scan(st, estate, chunk_cx, values_array(st, v + from, Min(PUSH_RUNTIME_CHUNK, n - from)));
	}
	MemoryContextSwitchTo(estate->es_query_cxt);
	MemoryContextDelete(chunk_cx);
	sift_mem_end(&mt);
	sift_v6_group_finish(st->group);
	MemoryContextSwitchTo(old);
}

static TupleTableSlot *
push_next(ScanState *ss)
{
	PushState  *st = (PushState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (!st->ran)
		push_run(st, ss->ps.state);
	ExecClearTuple(slot);
	MemoryContextSwitchTo(ss->ps.ps_ExprContext->ecxt_per_tuple_memory);
	if (!sift_v6_group_next(st->group, slot->tts_values, slot->tts_isnull))
		return NULL;
	return ExecStoreVirtualTuple(slot);
}

static bool
push_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
push_exec(CustomScanState *node)
{
	MemoryContext old = CurrentMemoryContext;
	TupleTableSlot *slot = ExecScan(&node->ss, push_next, push_recheck);

	MemoryContextSwitchTo(old);
	return slot;
}

static void
push_release(PushState *st)
{
	if (st->group != NULL)
		sift_v6_group_free(st->group);
	if (st->gcx != NULL)
		MemoryContextDelete(st->gcx);
	st->group = NULL;
	st->gcx = NULL;
	st->router = NULL;
}

static void
push_end(CustomScanState *node)
{
	PushState  *st = (PushState *) node;

	push_release(st);
	foreach_ptr(PlanState, child, node->custom_ps)
		ExecEndNode(child);
	table_close(st->heap, AccessShareLock);
	if (st->index != NULL)
		index_close(st->index, AccessShareLock);
}

static void
push_rescan(CustomScanState *node)
{
	PushState  *st = (PushState *) node;

	push_release(st);
	st->ran = false;
	st->nvalues = 0;
	st->stats = (SiftV6ScanStats) {0};
	foreach_ptr(PlanState, child, node->custom_ps)
		ExecReScan(child);
}

static void
push_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	PushState  *st = (PushState *) node;

	sift_v6_explain_index(es, RelationGetRelid(st->spec->is_table ? st->heap : st->index), st->spec->is_table);
	ExplainPropertyText("Join", "pushed into the scan as a key", es);
	if (st->ran)
		ExplainPropertyInteger("Join Keys Pushed", NULL, st->nvalues, es);
	sift_v6_explain_lookup_reads(node->custom_ps, es);
	if (st->router != NULL)
	{
		int64		kept,
					total;

		st->ops->scope->router_counts(st->router, &kept, &total);
		ExplainPropertyInteger("Scope Granules Kept", NULL, kept, es);
		ExplainPropertyInteger("Scope Granules Total", NULL, total, es);
	}
	if (es->analyze && st->ran)
	{
		SiftExplainStatsV6 ex = {0};

		sift_v6_explain_scan_stats(&st->stats, &ex);
		sift_agg_explain_v6(es, &ex, sift_simd_level_name(sift_cpu.active), 0, 0);
	}
}

static const CustomExecMethods push_exec_methods = {
	.CustomName = "SiftAggV6Push",
	.BeginCustomScan = push_begin,
	.ExecCustomScan = push_exec,
	.EndCustomScan = push_end,
	.ReScanCustomScan = push_rescan,
	.ExplainCustomScan = push_explain,
};

static Node *
push_create_state(CustomScan *cscan)
{
	PushState  *st = (PushState *) newNode(sizeof(PushState), T_CustomScanState);

	st->css.methods = &push_exec_methods;
	return (Node *) st;
}

const CustomScanMethods sift_agg_v6_push_plan_methods = {"SiftAggV6Push", push_create_state};
