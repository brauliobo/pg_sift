/*
 * agg/two/plan.c - the path of SiftAggV6Two (agg/two/two.h): the outer statement reads one sub-select, or a UNION ALL of them, whose arms PostgreSQL planned as SiftAggV6
 * aggregates of the same tables, each grouped by a unique row id. The outer keys and aggregates are compiled over inputs, the expressions of a cell the node computes for every
 * cell; the arms are the subquery scans of their SiftAggV6 paths.
 */
/* check-memory: allow-allocs every array here follows the arms, keys and aggregates of one statement, not the rows of the table */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

#include "agg/datum.h"
#include "agg/ht/generic.h"
#include "agg/ht/ht.h"
#include "agg/plan/int.h"
#include "agg/refuse.h"
#include "agg/settings.h"
#include "agg/two/int.h"
#include "agg/two/plan_int.h"
#include "agg/two/two.h"
#include "core/compat.h"

#define TWO_ARM		"two_level"

static Plan *two_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);

static const CustomPathMethods two_path_methods = {"SiftAggV6Two", two_plan_path, NULL};

static bool
refused(TwoPlan *t, char *why)
{
	if (t->why == NULL)
		t->why = why;
	return false;
}

bool
sift_two_has_phv(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, PlaceHolderVar))
		return true;
	return expression_tree_walker(n, sift_two_has_phv, context);
}

static bool
exec_param_walker(Node *n, void *context)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
		return true;
	return expression_tree_walker(n, exec_param_walker, context);
}

/* The path of the arm's statement that is its SiftAggV6 aggregate, under the projection of its final target */
static Path *
arm_v6_path(PlannerInfo *sub)
{
	foreach_ptr(Path, p, fetch_upper_rel(sub, UPPERREL_FINAL, NULL)->pathlist)
	{
		const Path *q = p;

		while (IsA(q, ProjectionPath))
			q = ((const ProjectionPath *) q)->subpath;
		if (IsA(q, CustomPath) && ((const CustomPath *) q)->methods == sift_v6_group_path_methods() && p->param_info == NULL && !p->parallel_aware)
			return p;
	}
	return NULL;
}

/* A NOT NULL column of a table of the arm that a unique index of its own keys (legacy plan_agg.c unique_row_column) */
static bool
row_id(PlannerInfo *sub, Node *e)
{
	const Var  *v;
	RelOptInfo *rel;

	while (IsA(e, RelabelType) || IsA(e, PlaceHolderVar))
		e = IsA(e, RelabelType) ? (Node *) ((RelabelType *) e)->arg : (Node *) ((PlaceHolderVar *) e)->phexpr;
	if (!IsA(e, Var) || (v = (const Var *) e)->varlevelsup != 0 || v->varattno <= 0 || v->varno >= (Index) sub->simple_rel_array_size ||
		(rel = sub->simple_rel_array[v->varno]) == NULL || rel->reloptkind != RELOPT_BASEREL || !bms_is_member(v->varattno, rel->notnullattnums))
		return false;
	foreach_node(IndexOptInfo, ix, rel->indexlist)
		if (ix->unique && ix->ncolumns == 1 && ix->indexkeys[0] == v->varattno && ix->indpred == NIL && !ix->hypothetical)
			return true;
	return false;
}

/* the relations the arm joins, sorted: a table by its Oid, a sub-select, function or VALUES list by its kind; the arms of one scan join the same */
static List *
arm_tables(PlannerInfo *sub)
{
	List	   *rels = NIL;

	for (int i = 1; i < sub->simple_rel_array_size; i++)
	{
		const RelOptInfo *r = sub->simple_rel_array[i];
		const RangeTblEntry *rte;

		if (r == NULL || r->reloptkind != RELOPT_BASEREL)
			continue;
		rte = planner_rt_fetch(i, sub);
		rels = lappend_oid(rels, rte->rtekind == RTE_RELATION ? rte->relid : (Oid) rte->rtekind);
	}
	list_sort(rels, list_oid_cmp);
	return rels;
}

/* The target of the subquery scan of an arm: the columns of the FROM item in order, as the arm's rel names them (cols) */
static PathTarget *
cell_target(PlannerInfo *root, List *vars)
{
	PathTarget *target = create_empty_pathtarget();

	foreach_ptr(Expr, v, vars)
		add_column_to_pathtarget(target, v, 0);
	return set_pathtarget_cost_width(root, target);
}

static bool
add_arm(TwoPlan *t, RelOptInfo *rel, List *vars)
{
	TwoArm	   *a = &t->arm[t->narms];
	const Query *q;
	Path	   *v6;
	int			ids = 0;

	if (t->narms == SIFT_V6_FUSE_MAX_ARMS || rel->subroot == NULL || IS_DUMMY_REL(rel) || rel->lateral_relids != NULL)
		return refused(t, psprintf("arm %d is not a sub-select the outer statement reads alone", t->narms + 1));
	a->rel = rel;
	a->sub = rel->subroot;
	q = a->sub->parse;
	if (q->groupClause == NIL || q->groupingSets != NIL || q->distinctClause != NIL || q->sortClause != NIL || q->limitCount != NULL || q->limitOffset != NULL ||
		q->hasWindowFuncs || q->hasTargetSRFs || q->setOperations != NULL)
		return refused(t, psprintf("arm %d is not a plain aggregate (it has DISTINCT, ORDER BY, LIMIT, windows, grouping sets or no GROUP BY)", t->narms + 1));
	if ((v6 = arm_v6_path(a->sub)) == NULL)
		return refused(t, psprintf("arm %d is not an aggregate of the scan", t->narms + 1));
	t->candidate = true;
	foreach_node(SortGroupClause, sgc, q->groupClause)
	{
		Node	   *g = (Node *) get_sortgroupclause_expr(sgc, q->targetList);

		if (row_id(a->sub, g) && ids++ == 0)
			a->id = g;
	}
	if (ids == 0)
		return refused(t, psprintf("arm %d groups by no unique row id", t->narms + 1));
	if (t->narms > 0 && !equal(arm_tables(a->sub), arm_tables(t->arm[0].sub)))
		return refused(t, psprintf("arm %d does not read the scan of arm 1: its tables or joins differ", t->narms + 1));
	foreach_ptr(Var, v, vars)
		a->cols = lappend(a->cols, get_tle_by_resno(q->targetList, v->varattno)->expr);
	a->path = &create_subqueryscan_path(t->root, rel, v6, false, NIL, NULL)->path;
	a->path->pathtarget = cell_target(t->root, vars);
	t->narms++;
	return true;
}

/* the Vars of rel that are the columns 1..ncols of the FROM item, translated to the arm (an appendrel child) */
static List *
arm_vars(TwoPlan *t, const AppendRelInfo *ai)
{
	List	   *vars = NIL;

	if (ai != NULL)
		return list_copy_head(ai->translated_vars, t->ncols);
	for (int c = 1; c <= t->ncols; c++)
	{
		const TargetEntry *te = get_tle_by_resno(t->rel->subroot->parse->targetList, c);

		vars = lappend(vars, makeVar(t->srti, c, exprType((Node *) te->expr), exprTypmod((Node *) te->expr), exprCollation((Node *) te->expr), 0));
	}
	return vars;
}

static bool
arms_of(TwoPlan *t)
{
	const RangeTblEntry *rte = planner_rt_fetch(t->srti, t->root);

	t->ncols = list_length(rte->eref->colnames);
	if (!rte->inh)
		return add_arm(t, t->rel, arm_vars(t, NULL));
	foreach_node(AppendRelInfo, ai, t->root->append_rel_list)
	{
		RelOptInfo *child;

		if (ai->parent_relid != t->srti)
			continue;
		child = find_base_rel(t->root, ai->child_relid);
		foreach_ptr(Node, v, ai->translated_vars)
			if (!IsA(v, Var))
				return refused(t, psprintf("arm %d is not a sub-select the outer statement reads alone", t->narms + 1));
		if (planner_rt_fetch(ai->child_relid, t->root)->rtekind != RTE_SUBQUERY || !add_arm(t, child, arm_vars(t, ai)))
			return refused(t, psprintf("arm %d is not a sub-select the outer statement reads alone", t->narms + 1));
	}
	return t->narms > 0;
}

/* e with each placeholder that no outer join nulls (one the grouping sets mark) as its expression: the node computes it for every cell and the sets null the keys themselves */
Node *
sift_two_phvs(Node *n, TwoPlan *t)
{
	const PlaceHolderVar *phv = (const PlaceHolderVar *) n;

	if (n == NULL)
		return NULL;
	if (IsA(n, PlaceHolderVar) && (!bms_overlap(phv->phnullingrels, t->root->outer_join_rels) ||
								   (!contain_nonstrict_functions((Node *) phv->phexpr) && contain_var_clause((Node *) phv->phexpr) && sift_two_side(t, (Node *) phv->phexpr) == 0)))
		return sift_two_phvs((Node *) phv->phexpr, t);
	return expression_tree_mutator(n, sift_two_phvs, t);
}

/* the input that e is (a new one when no other is equal): its column of the spec */
static int
input_of(TwoPlan *t, Expr *e)
{
	e = (Expr *) sift_two_phvs((Node *) e, t);
	foreach_ptr(Expr, in, t->inputs)
		if (equal(in, e))
			return foreach_current_index(in) + 1;
	t->inputs = lappend(t->inputs, e);
	return list_length(t->inputs);
}

/* an expression the node computes over a cell: columns of the FROM item, no aggregate, sub-plan, parameter of another node or volatile function */
bool
sift_two_cell_expr(TwoPlan *t, Node *e)
{
	e = sift_two_phvs(e, t);
	if (sift_two_has_phv(e, NULL))
		return false;
	foreach_node(Var, v, pull_var_clause(e, PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS))
		if (sift_two_side(t, (Node *) v) < 0)
			return false;
	return !contain_agg_clause(e) && !contain_subplans(e) && !exec_param_walker(e, NULL) && !contain_volatile_functions(e) && !contain_window_function(e);
}

static SiftV6Col
input_col(TwoPlan *t, Expr *e)
{
	e = (Expr *) sift_two_phvs((Node *) e, t);
	const Oid	typid = getBaseType(exprType((Node *) e));

	return (SiftV6Col) {.attno = input_of(t, e),.typid = typid,.scale = sift_v6_type_native(typid) ? 0 : SIFT_V6_SCALE_DATUM,.collid = exprCollation((Node *) e)};
}

/* a key: grouped by its bytes (a text whose collation tells no two strings of other bytes equal) or by the values a datum key interns */
static bool
compile_key(TwoPlan *t, Expr *e, SiftV6Col *k)
{
	if (!sift_two_cell_expr(t, (Node *) e))
		return false;
	*k = input_col(t, e);
	if (sift_v6_col_class(k) == SV6C_DATUM)
		return sift_genkey_supported(k->typid);
	return !(sift_v6_class(k->typid) == SV6C_TEXT && !sift_text_bytes_equal(k->collid));
}

static bool
compile_keys(TwoPlan *t)
{
	const bool	sets = t->root->parse->groupingSets != NIL;

	t->spec.key = palloc(Max(list_length(t->group_exprs), 1) * sizeof(SiftV6Col));
	t->sorted = !sets;
	foreach_ptr(Expr, e, t->group_exprs)
	{
		const SortGroupClause *sgc = foreach_current_index(e) < list_length(t->root->processed_groupClause) ?
			list_nth_node(SortGroupClause, t->root->processed_groupClause, foreach_current_index(e)) : NULL;

		if (IsA(e, Const) && !((Const *) e)->constisnull && !sets)
			continue;
		if (t->spec.nkeys == SIFT_V6_MAX_KEYS || !compile_key(t, e, &t->spec.key[t->spec.nkeys]))
			return refused(t, psprintf("the outer key %d is not one the node groups by", t->spec.nkeys + 1));
		t->spec.nkeys++;
		t->scan_tlist = lappend(t->scan_tlist, makeTargetEntry(e, list_length(t->scan_tlist) + 1, NULL, false));
		t->sorted &= sgc == NULL || OidIsValid(sgc->sortop);
		if (sgc != NULL)
			t->sort = lappend(t->sort, list_make4_int(t->spec.nkeys, (int) sgc->sortop, (int) exprCollation((Node *) e), sgc->nulls_first));
	}
	t->spec.constant_groups = t->spec.nkeys == 0 && t->root->parse->groupClause != NIL && !sets;
	return true;
}

static int
key_index_of(const TwoPlan *t, Index ref)
{
	foreach_node(SortGroupClause, sgc, t->root->processed_groupClause)
		if (sgc->tleSortGroupRef == ref)
			return foreach_current_index(sgc);
	return -1;
}

static bool
compile_sets(TwoPlan *t)
{
	List	   *sets = t->root->parse->groupingSets;

	t->spec.set_mask = palloc(Max(list_length(sets), 1) * sizeof(uint32));
	foreach_ptr(List, set, sets)
	{
		uint32		mask = 0;

		foreach_int(ref, set)
		{
			const int	k = key_index_of(t, ref);

			if (k < 0)
				return refused(t, pstrdup("a grouping set of the outer statement is not over its keys"));
			mask |= ((uint32) 1) << k;
		}
		t->spec.set_mask[t->spec.nsets++] = mask;
	}
	return true;
}

/* The argument of the aggregate as an input: a FILTER is the CASE that gives its argument where the clause holds (a strict aggregate skips the NULL of the others) */
static Expr *
agg_input(const Aggref *a, SiftV6Kind *kind)
{
	Expr	   *arg = *kind == SV6_COUNT_STAR ? (Expr *) makeBoolConst(true, false) : linitial_node(TargetEntry, a->args)->expr;

	if (a->aggfilter == NULL)
		return *kind == SV6_COUNT_STAR ? NULL : arg;
	if (*kind == SV6_COUNT_STAR)
		*kind = SV6_COUNT;
	return (Expr *) sift_v6_case_when((Node *) a->aggfilter, (Node *) arg);
}

static bool
compile_agg(TwoPlan *t, Aggref *a)
{
	Aggref		plain = *a;
	SiftV6Agg	g = {.restype = a->aggtype};
	Expr	   *arg;

	plain.aggfilter = NULL;
	if (a->agglevelsup != 0 || !sift_v6_agg_kind(&plain, &g.kind) || g.kind == SV6_ARG_MAX || g.kind == SV6_ARG_MIN ||
		(g.kind != SV6_COUNT_STAR && (list_length(a->args) != 1 || IsA(linitial_node(TargetEntry, a->args)->expr, RowExpr))))
		return false;
	if ((arg = agg_input(a, &g.kind)) != NULL)
	{
		const Oid	coll = exprCollation((Node *) arg);

		if (!sift_two_cell_expr(t, (Node *) arg))
			return false;
		g.arg = input_col(t, arg);
		if (!sift_v6_type_native(g.arg.typid) || !sift_v6_kind_takes(g.kind, g.arg.typid))
			return false;
		if (sift_v6_class(g.arg.typid) == SV6C_TEXT && !((g.kind == SV6_MIN || g.kind == SV6_MAX) ? sift_collate_is_c(coll) : sift_text_bytes_equal(coll)))
			return false;
	}
	t->spec.agg[t->spec.naggs++] = g;
	return true;
}

static bool
compile_grouping(TwoPlan *t, GroupingFunc *gf)
{
	SiftV6Func	f = {.nargs = list_length(gf->refs)};

	if (f.nargs > SIFT_V6_MAX_KEYS || gf->agglevelsup != 0)
		return false;
	foreach_int(ref, gf->refs)
		if ((f.key[foreach_current_index(ref)] = key_index_of(t, ref)) < 0)
			return false;
	t->spec.func[t->spec.nfuncs] = f;
	t->spec.agg[t->spec.naggs++] = (SiftV6Agg) {.kind = SV6_GROUPING,.arg = {.attno = t->spec.nfuncs++},.restype = INT4OID};
	return true;
}

static bool
in_scan_tlist(const TwoPlan *t, const Node *n)
{
	foreach_node(TargetEntry, te, t->scan_tlist)
		if (equal(te->expr, n))
			return true;
	return false;
}

static bool
compile_aggs(TwoPlan *t, List *exprs)
{
	List	   *aggs = NIL;

	foreach_ptr(Node, n, pull_var_clause((Node *) exprs, PVC_INCLUDE_AGGREGATES | PVC_RECURSE_PLACEHOLDERS))
		if ((IsA(n, Aggref) || IsA(n, GroupingFunc)) && !list_member(aggs, n))
			aggs = lappend(aggs, n);
	t->spec.agg = palloc0(Max(list_length(aggs), 1) * sizeof(SiftV6Agg));
	t->spec.func = palloc0(Max(list_length(aggs), 1) * sizeof(SiftV6Func));
	foreach_ptr(Node, n, aggs)
	{
		if (in_scan_tlist(t, n))
			continue;
		if (IsA(n, Aggref) ? !compile_agg(t, (Aggref *) n) : !compile_grouping(t, (GroupingFunc *) n))
			return refused(t, psprintf("the outer aggregate %d is not one the node folds the cells into", foreach_current_index(n) + 1));
		t->scan_tlist = lappend(t->scan_tlist, makeTargetEntry((Expr *) n, list_length(t->scan_tlist) + 1, NULL, false));
	}
	return true;
}

static bool
compile_outer(TwoPlan *t, RelOptInfo *output_rel, List *having)
{
	t->group_exprs = sift_v6_group_exprs_of(t->root);
	return compile_keys(t) && compile_sets(t) && compile_aggs(t, list_concat_copy(output_rel->reltarget->exprs, having));
}

static double
cheapest_total(const RelOptInfo *rel, double *rows)
{
	double		c = -1;

	foreach_ptr(Path, path, rel->pathlist)
		if (c < 0 || path->total_cost < c)
		{
			c = path->total_cost;
			*rows = path->rows;
		}
	return c;
}

/* the spine of the plan (agg/two/int.h), NIL for none */
static List *
spine_private(const TwoPlan *t)
{
	const TwoSpine *sp = &t->spine;

	if (sp->rel == NULL)
		return NIL;
	return list_make5(list_make2_int(sp->left, sp->rel->relid), sp->cell_keys, sp->spine_keys, list_make3(sp->ops, sp->colls, sp->null_safe),
					  list_make2(sp->match_quals, sp->post_quals));
}

/*
 * The arms as they cost, and the fold of their cells into the outer groups: a hash lookup and a fold for each key and aggregate, as PostgreSQL's grouping prices them, without
 * the Append and the grouping node above the arms
 */
static void
add_two_path(TwoPlan *t, RelOptInfo *output_rel, List *having)
{
	CustomPath *cp = makeNode(CustomPath);
	double		cells = 0,
				groups = 1;
	const double normal = cheapest_total(output_rel, &groups);
	List	   *arms = NIL;
	const bool	sorted = t->sorted && t->sort != NIL;

	cp->path.parallel_safe = true;
	for (int i = 0; i < t->narms; i++)
	{
		cells += t->arm[i].path->rows;
		cp->path.startup_cost += t->arm[i].path->total_cost;
		cp->path.parallel_safe &= t->arm[i].path->parallel_safe;
		arms = lappend(arms, t->arm[i].path);
	}
	if (t->spine.rel != NULL)
	{
		const int	npairs = list_length(t->spine.cell_keys);

		cp->path.startup_cost += t->spine.path->total_cost + (t->spine.path->rows + cells) * cpu_operator_cost * npairs;
		cp->path.parallel_safe &= t->spine.path->parallel_safe;
		arms = lappend(arms, t->spine.path);
		cells = t->input->rows;
	}
	cp->path.startup_cost += cells * cpu_operator_cost * (t->spec.nkeys + t->spec.naggs);
	if (sorted && groups > 1)
		cp->path.startup_cost += 2.0 * cpu_operator_cost * groups * log2(groups);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = output_rel->reltarget;
	cp->path.rows = groups;
	cp->path.total_cost = cp->path.startup_cost + groups * cpu_tuple_cost;
	cp->path.pathkeys = sorted ? make_pathkeys_for_sortclauses(t->root, t->root->processed_groupClause, t->root->processed_tlist) : NIL;
	cp->methods = &two_path_methods;
	cp->custom_paths = arms;
	t->spec.groups_est = (uint32) Min(groups, (double) INT_MAX);
	cp->custom_private = list_make5(sift_v6_spec_pack(&t->spec), t->scan_tlist, having, t->inputs, list_make3(sift_two_labels(t), sorted ? t->sort : NIL, spine_private(t)));
	sift_agg_arm_priced(TWO_ARM, cp->path.total_cost, normal);
	sift_v6_force_price(&cp->path);
	add_path(output_rel, &cp->path);
}

void
sift_agg_v6_two_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	TwoPlan		t = {.root = root,.input = input_rel};
	List	   *having;

	if (stage != UPPERREL_GROUP_AGG || !sift_enable_agg || !root->parse->hasAggs || IS_DUMMY_REL(input_rel) || !sift_two_from(&t))
		return;
	having = extra != NULL ? (List *) ((GroupPathExtraData *) extra)->havingQual : NIL;
	if (!arms_of(&t))
	{
		if (t.candidate)
			sift_agg_refuse(TWO_ARM, AGG_REFUSE_TWO_ARMS, t.why, 0, -1);
		return;
	}
	if (!compile_outer(&t, output_rel, having))
	{
		sift_agg_arm_refused_by(TWO_ARM, AGG_REFUSE_AGGREGATE, t.why);
		return;
	}
	add_two_path(&t, output_rel, having);
}

typedef struct OuterVars
{
	Index		spine;
	List	   *tlist;
} OuterVars;

/* a column of the spine as the column of the spine's tuple it is (OUTER_VAR) */
static Node *
outer_vars(Node *n, OuterVars *o)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((Var *) n)->varno == o->spine)
		foreach_node(TargetEntry, te, o->tlist)
		{
			const Node *e = (Node *) te->expr;

			while (IsA(e, PlaceHolderVar))
				e = (Node *) ((const PlaceHolderVar *) e)->phexpr;
			if (IsA(e, Var) && ((const Var *) e)->varno == o->spine && ((const Var *) e)->varattno == ((Var *) n)->varattno)
			{
				Var		   *v = copyObject((Var *) n);

				v->varno = OUTER_VAR;
				v->varattno = te->resno;
				v->varnullingrels = NULL;
				return (Node *) v;
			}
		}
	return expression_tree_mutator(n, outer_vars, o);
}

static Plan *
two_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	List	   *rest = list_nth(best->custom_private, 4);
	List	   *spine = lthird(rest);
	List	   *inputs = lfourth(best->custom_private);

	sift_agg_arm_used(TWO_ARM);
	if (spine != NIL)
	{
		OuterVars	o = {lsecond_int(linitial(spine)), ((Plan *) llast(custom_plans))->targetlist};

		inputs = (List *) outer_vars((Node *) inputs, &o);
		spine = list_make5(linitial(spine), lsecond(spine), outer_vars(lthird(spine), &o), lfourth(spine), outer_vars(list_nth(spine, 4), &o));
	}
	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->flags = best->flags;
	cs->custom_scan_tlist = lsecond(best->custom_private);
	cs->custom_plans = custom_plans;
	cs->custom_private = list_make5(linitial(best->custom_private), inputs, linitial(rest), lsecond(rest), spine);
	cs->scan.plan.qual = lthird(best->custom_private);
	cs->methods = &sift_agg_v6_two_plan_methods;
	return &cs->scan.plan;
}
