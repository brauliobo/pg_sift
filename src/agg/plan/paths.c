/*
 * agg/plan/paths.c - the paths SiftAgg adds to the planner
 * The upper paths hook, the choice between the scan and the normal plan, and the
 * children a correlated plan keeps.
 */
#include "postgres.h"

#include <math.h>

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/rows.h"
#include "agg/plan/int.h"


/* Does the grouping have a key that is not a constant? */
static bool
groupClause_has_nonconst(PlannerInfo *root, List *groupClause)
{
	foreach_node(SortGroupClause, sgc, groupClause)
		if (!IsA(get_sortgroupclause_expr(sgc, root->processed_tlist), Const))
			return true;
	return false;
}

/*
 * The rows of one scan grouped by `groupClause` (the GROUP BY of the query, or the columns of its SELECT DISTINCT, which is a grouping with no
 * aggregate): the SiftAgg path of output_rel, whose rows are those of `target` (the grouping target of the query, or its distinct target).
 */
static void
group_paths_skip(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, GroupPathExtraData *gextra, List *groupClause, int skip, int *others)
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

	if (skip == 0)
	{
		if (!agg_base_either(c, root, input_rel, &fs))
			return;
		/* the tables of an inner join may each be the scanned one, the others its lookups: the first in the FROM list is not the only way (group_paths) */
		*others = fs.skip == 0 && !fs.flippable && fs.lefts == NIL && !c->joinless && !fs.flip &&
			input_rel->cheapest_total_path->total_cost >= AGG_ORDER_MIN_COST ? fs.ntables - 1 : 0;
	}
	else
	{
		fs.skip = skip;
		if (!agg_base(c, root, input_rel, &fs))
			return;
	}
	s = agg_spec_new(c);
	if (c->nparts == 0)
		c->search = sift_search_extract(root, c->rel, &fs.quals, c->indexoid, c->heapoid, false);
	if (c->search)
	{
		double		scan_ops,
					key_ops;

		if (sift_search_key_first(root, c->rel, c->search, fs.quals, &scan_ops, &key_ops))
		{
			agg_refused_f(COST, "the other conditions keep few rows, which an index reads and checks the text of (about %.0f operators) for less than the scan walks the postings (about %.0f)", key_ops, scan_ops);
			if (!sift_agg_force)
				return;
		}
	}
	if (!agg_quals(c, s, &fs))
		return;

	/* keys */
	if (list_length(groupClause) + (parse->groupingSets != NIL) > sift_limit(group_keys, SIFT_AGG_MAX_KEYS))
	{
		agg_refused_limit(TOO_MANY_KEYS, group_keys, sift_limit(group_keys, SIFT_AGG_MAX_KEYS), "%d group keys (grouping sets count as one more)",
						  list_length(groupClause) + (parse->groupingSets != NIL));
		return;
	}
	s->key = palloc0((list_length(groupClause) + 1) * sizeof(SiftAggKey));
	foreach_node(SortGroupClause, sgc, groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);

		/* a constant adds no group: the output keeps it as the constant it is (not with grouping sets, whose empty set has no key, and not alone: an empty input has no group) */
		if (IsA(e, Const) && parse->groupingSets == NIL && groupClause_has_nonconst(root, groupClause))
			continue;
		if (!compile_key(c, sgc, e, &s->key[s->nkeys]))
		{
			agg_refused_f(GROUP_KEY, "the group key %s is not a leaf, or an expression of leaves, the scan reads", expr_text(c, e));
			return;
		}
		combo_set_key(c, &s->key[s->nkeys], s->nkeys);
		ow.keys = lappend(ow.keys, e);
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) e, list_length(scan_tlist) + 1, NULL, false));
		s->nkeys++;
	}
	if (parse->groupingSets != NIL)
		add_sets(parse, s, &scan_tlist);
	c->part_key = parse->groupingSets == NIL && !parse->hasDistinctOn && group_has_partition_key(c, ow.keys);

	/* DISTINCT ON: the other columns are the first row's */
	if (parse->hasDistinctOn)
	{
		if (!distinct_on_more(c, parse, &more) || !first_aggs(c, target, ow.keys, more, &firsts))
		{
			agg_refused(DISTINCT_ON);
			return;
		}
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
	{
		agg_refused(OUTPUT_EXPR);
		return;
	}
	s->agg = palloc0(Max(list_length(ow.aggs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(Node, a, ow.aggs)
	{
		bool		first = list_member_ptr(firsts, a);

		if (!(IsA(a, GroupingFunc) ? compile_grouping(parse, (GroupingFunc *) a, &s->agg[s->naggs]) :
			  compile_agg(c, (Aggref *) a, &s->agg[s->naggs], s->naggs)))
		{
			agg_refused_f(AGGREGATE, "%s is not an aggregate the scan computes over its arguments, filter and ordering", IsA(a, Aggref) ? get_func_name(((Aggref *) a)->aggfnoid) : "GROUPING()");
			return;
		}
		if (first)
		{
			first_mode(&s->agg[s->naggs], list_length(more), lead);
			lead = false;
		}
		scan_tlist = lappend(scan_tlist, makeTargetEntry(first ? (Expr *) linitial_node(TargetEntry, ((Aggref *) a)->args)->expr : (Expr *) a,
														 list_length(scan_tlist) + 1, NULL, false));
		s->naggs++;
	}
	group_topn(root, s, ow.aggs, groupClause, gextra);
	c->having_sel = gextra && gextra->havingQual ? Max(clauselist_selectivity(root, (List *) gextra->havingQual, 0, JOIN_INNER, NULL), 1e-9) : 0;
	agg_finish(c, s, root, input_rel, output_rel, target, ow.keys, scan_tlist);
}

/*
 * The spec of a path, as it is when the path is made (the next path of the pair, with workers, changes the one the planner holds): packed when the path
 * becomes a plan (agg_plan_path). A path that loses is most of them, and packing a spec costs more than building the path.
 */
SiftAggSpec *
agg_spec_keep(const SiftAggSpec *s)
{
	SiftAggSpec *keep = palloc(sizeof(SiftAggSpec));

	*keep = *s;
	return keep;
}

/*
 * The scan of the first table of the FROM list with the others as its lookups, and, for inner joins, the scan of each of the others in turn: PostgreSQL
 * does not depend on the order of the FROM list and the price of the scan does (a table read whole as a lookup against one read by its keys), so each
 * order gets its path and the cheaper wins. The paths of the other orders do not report: the arm tells why the first had none.
 */
static void
group_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, PathTarget *target, GroupPathExtraData *gextra, List *groupClause)
{
	int			others = 0;

	group_paths_skip(root, input_rel, output_rel, target, gextra, groupClause, 0, &others);
	for (int skip = 1; skip <= others; skip++)
	{
		bool		quiet = agg_quiet_reports(true);

		group_paths_skip(root, input_rel, output_rel, target, gextra, groupClause, skip, &others);
		agg_quiet_reports(quiet);
	}
}

static void
upper_paths_on(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	Query	   *parse = root->parse;

	if (stage == UPPERREL_ORDERED)
	{
		agg_arm_begin("topn", false);
		topn_paths(root, input_rel, output_rel);
		agg_arm_end();
		return;
	}
	if (!sift_enable_agg || parse->commandType != CMD_SELECT || parse->hasTargetSRFs || root->rowMarks != NIL || parse->hasRecursive)
	{
		if (stage == UPPERREL_GROUP_AGG && parse->commandType == CMD_SELECT)
		{
			agg_arm_begin("single", true);
			agg_refuse_note(sift_enable_agg ? AGG_REFUSE_QUERY_KIND : AGG_REFUSE_DISABLED, NULL);
			agg_arm_end();
		}
		return;
	}
	/* SELECT DISTINCT of a plain scan is a group-by with no aggregate; over a grouped or windowed query it is not the scan's */
	if (stage == UPPERREL_DISTINCT)
	{
		if (parse->distinctClause != NIL && root->upper_targets[UPPERREL_DISTINCT] != NULL &&
			!(parse->hasAggs || parse->groupClause || parse->groupingSets || parse->hasWindowFuncs || parse->havingQual))
		{
			agg_arm_begin("distinct", true);
			group_paths(root, input_rel, output_rel, root->upper_targets[UPPERREL_DISTINCT], NULL, parse->distinctClause);
			agg_arm_end();
		}
		return;
	}
	if (stage != UPPERREL_GROUP_AGG || parse->hasDistinctOn || !(parse->hasAggs || parse->groupClause || parse->groupingSets))
		return;
	/* a window function is planned above the grouping stage: this stage is the same with or without one */
	if (list_length(parse->rtable) > 0 && parse->jointree->fromlist != NIL && list_length(parse->jointree->fromlist) == 1 &&
		IsA(linitial(parse->jointree->fromlist), RangeTblRef) &&
		rt_fetch(((RangeTblRef *) linitial(parse->jointree->fromlist))->rtindex, parse->rtable)->rtekind == RTE_SUBQUERY)
	{
		if (!parse->hasWindowFuncs && sift_agg_two_level_on)
		{
			agg_arm_begin("two_level", false);
			sift_agg_two_level(root, input_rel, output_rel, (GroupPathExtraData *) extra);
			agg_arm_end();
		}
		return;
	}
	/* a join of a spine and the arms of a two level aggregate; any other join is the single level's */
	if (!parse->hasWindowFuncs && sift_agg_two_level_on && list_length(parse->jointree->fromlist) == 1 &&
		(IsA(linitial(parse->jointree->fromlist), JoinExpr) || IsA(linitial(parse->jointree->fromlist), FromExpr)))
	{
		agg_arm_begin("two_level", false);
		sift_agg_two_level(root, input_rel, output_rel, (GroupPathExtraData *) extra);
		agg_arm_end();
	}
	agg_arm_begin("single", true);
	group_paths(root, input_rel, output_rel, output_rel->reltarget, (GroupPathExtraData *) extra, parse->groupClause);
	agg_arm_end();
	agg_arm_begin("rows", false);
	sift_agg_rows_paths(root, input_rel, output_rel, extra);
	agg_arm_end();
}

/* does the input read a table with a column source: a plan the scan competes with */
static bool
scans_column_source(PlannerInfo *root, RelOptInfo *rel)
{
	bool		found = false;
	int			i = -1;

	while ((i = bms_next_member(rel->relids, i)) >= 0)
	{
		RangeTblEntry *rte = planner_rt_fetch(i, root);

		found |= rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_source(rte->relid);
	}
	return found;
}

static bool
has_subquery_rte(Query *parse)
{
	foreach_node(RangeTblEntry, rte, parse->rtable)
		if (rte->rtekind == RTE_SUBQUERY)
			return true;
	return false;
}

/*
 * The cheapest normal path of a stage, as the statement will pay for it: the rows a LIMIT takes of an ordered stage (the Limit node costs the same
 * fraction). Under half of agg_floor_cost no arm of the scan can win, so the arms are not planned (0.02 to 0.06 ms of a point lookup or a keyset page).
 * Only the top query without a CTE or sub-select: the price of those is not in the paths of the stage, and the inner levels are planned for the statement they join.
 */
static bool
normal_under_floor(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *output_rel)
{
	double		best = HUGE_VAL;

	if (sift_explain_refusals || sift_agg_force || root->query_level != 1 ||
		(stage != UPPERREL_GROUP_AGG && stage != UPPERREL_ORDERED && stage != UPPERREL_DISTINCT) ||
		root->parse->cteList != NIL || root->parse->hasSubLinks || root->init_plans != NIL || has_subquery_rte(root->parse) ||
		/* the scan prices the elements of a set parameter it assumes (sift.agg_param_set_size), not the normal plan's guess */
		query_tree_walker(root->parse, extern_param_walker, NULL, QTW_IGNORE_RT_SUBQUERIES))
		return false;
	foreach_ptr(Path, p, output_rel->pathlist)
	{
		double		cost = p->total_cost;

		if (stage == UPPERREL_ORDERED && root->limit_tuples > 0 && p->rows > root->limit_tuples)
			cost = p->startup_cost + (p->total_cost - p->startup_cost) * root->limit_tuples / p->rows;
#if PG_VERSION_NUM >= 180000
		/* a plan that uses a disabled path (enable_seqscan = off ..) is the one a user means the scan to replace */
		if (p->disabled_nodes > 0)
			continue;
#endif
		if (!(IsA(p, CustomPath) && ((CustomPath *) p)->methods == &agg_path_methods))
			best = Min(best, cost);
	}
	return best < agg_floor_cost() / 2.0;
}

#ifdef SIFT_TESTING
int			sift_test_fail_pick;	/* sift.test_fail_pick */
#endif

/* a table with several column indexes gets a path through each of them that gives a plan (each may hold other columns and paths, and cut its units differently): the cheapest wins */
void
sift_agg_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	int			outer_pick = agg_index_pick,
				outer_usable = agg_index_usable;
	double		outer_normal = agg_stage_normal;
	AggStage	st;

	agg_stage_begin(&st, root);
	agg_index_usable = 0;
	agg_stage_normal = HUGE_VAL;
	if ((stage == UPPERREL_GROUP_AGG || stage == UPPERREL_PARTIAL_GROUP_AGG) && scans_column_source(root, input_rel))
		foreach_ptr(Path, p, output_rel->pathlist)
			if (IsA(p, AggPath) && ((AggPath *) p)->aggstrategy == AGG_HASHED)
				p->total_cost += hash_agg_extra((AggPath *) p);
	foreach_ptr(Path, p, output_rel->pathlist)
		if (!(IsA(p, CustomPath) && ((CustomPath *) p)->methods == &agg_path_methods))
			agg_stage_normal = Min(agg_stage_normal, p->total_cost);
	if (normal_under_floor(root, stage, output_rel))
	{
		agg_stage_end(&st);
		agg_stage_normal = outer_normal;
		agg_index_pick = outer_pick;
		agg_index_usable = outer_usable;
		return;
	}
	for (agg_index_pick = 0;; agg_index_pick++)
	{
#ifdef SIFT_TESTING
		if (sift_test_fail_pick > 0 && agg_index_pick == sift_test_fail_pick)
			ereport(ERROR, (errcode(ERRCODE_QUERY_CANCELED), errmsg("canceling statement due to sift.test_fail_pick")));
#endif
		upper_paths_on(root, stage, input_rel, output_rel, extra);
		if (agg_index_pick + 1 >= agg_index_usable)
			break;
	}
	agg_stage_end(&st);
	if (stage == UPPERREL_FINAL)
		agg_final_paths(root, output_rel);
	agg_index_pick = outer_pick;
	agg_index_usable = outer_usable;
	agg_stage_normal = outer_normal;
}

/* the native plans of the correlated children, made now that the plan has its place in the statement; true when the spec holds their IDs */
static bool
plan_correlated_children(PlannerInfo *root, CustomPath *best, SiftAggSpec *spec)
{
	bool		planned = false;

	for (int i = 0; i < spec->njoin; i++)
	{
		Path	   *subpath = list_nth(((SiftAggPath *) best)->correlated, i);

		if (subpath != NULL)
		{
			PlannerInfo *subroot = list_nth(((SiftAggPath *) best)->subroots, i);
			List	   *tlist = subroot->processed_tlist;
			Plan       *plan;

			/* the plan of the rows of a plain child is labelled with its columns, not with those of the level's query */
			if (spec->join[i].params == NIL)
				subroot->processed_tlist = make_tlist_from_pathtarget(subpath->pathtarget);
			plan = create_plan(subroot, subpath);
			subroot->processed_tlist = tlist;
			root->glob->subplans = lappend(root->glob->subplans, plan);
			root->glob->subpaths = lappend(root->glob->subpaths, subpath);
			root->glob->subroots = lappend(root->glob->subroots, subroot);
			spec->join[i].subplan = list_length(root->glob->subplans);
			planned = true;
		}
	}
	return planned;
}

/*
 * A set without a constant key nulls it. PostgreSQL's Agg reads the key as a column of its input, found by its sort-group reference; the output of a
 * custom scan is matched by expression, and setrefs.c never swaps a bare Const for a column, so the output would keep the constant. A relabel to the
 * type it has is no Const: the output reads the column of the scan tuple, which the scan sets to NULL in the sets without the key. (The path keeps
 * its target: the plan above, built for the Const, needs no projection.)
 */
static void
const_keys_as_columns(PlannerInfo *root, CustomScan *cs)
{
	foreach_node(TargetEntry, te, cs->scan.plan.targetlist)
	{
		bool		key = false;

		if (!IsA(te->expr, Const) || te->ressortgroupref == 0)
			continue;
		foreach_node(SortGroupClause, sgc, root->parse->groupClause)
			key |= sgc->tleSortGroupRef == te->ressortgroupref;
		if (!key)
			continue;
		foreach_node(TargetEntry, st, cs->custom_scan_tlist)
			if (IsA(st->expr, Const) && equal(st->expr, te->expr))
			{
				Node	   *k = (Node *) te->expr;

				st->expr = (Expr *) makeRelabelType((Expr *) copyObject(k), exprType(k), exprTypmod(k), exprCollation(k), COERCE_IMPLICIT_CAST);
				te->expr = (Expr *) copyObject(st->expr);
				break;
			}
	}
}

Plan *
agg_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	List	   *pack = sift_aggspec_pack(linitial(best->custom_private));
	List	   *scan_tlist = lsecond(best->custom_private);
	SiftAggSpec *spec = sift_aggspec_unpack(pack);
	GroupPathExtraData *gextra = NULL;

	cs->scan.plan.targetlist = root->parse->hasWindowFuncs ? (List *) topn_window_aggs((Node *) tlist, NULL) : tlist;
	cs->scan.scanrelid = 0;
	cs->custom_plans = custom_plans;
	cs->custom_scan_tlist = copyObject(scan_tlist);
#if PG_VERSION_NUM < 180000
	if (root->parse->groupingSets != NIL)
		const_keys_as_columns(root, cs);
#endif
	cs->custom_private = plan_correlated_children(root, best, spec) ? sift_aggspec_pack(spec) : pack;
	/* declared, so that PostgreSQL knows the plan depends on them and rescans it when the enclosing level sets new values (agg_rescan) */
	if (list_length(best->custom_private) > 2)
		cs->custom_exprs = copyObject(lthird(best->custom_private));
	cs->methods = &sift_agg_plan_methods;
	if (root->hasHavingQual && root->parse->havingQual)
		cs->scan.plan.qual = (List *) copyObject(root->parse->havingQual);
	(void) gextra;
	if (OidIsValid(spec->indexoid))
		root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->indexoid);
	for (int i = 0; i < spec->nparts; i++)
	{
		root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->partheap[i]);
		if (OidIsValid(spec->partindex[i]))
			root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->partindex[i]);
	}
	for (int i = 0; i < spec->njoin; i++)
		if (OidIsValid(spec->join[i].relid))
			root->glob->relationOids = lappend_oid(root->glob->relationOids, spec->join[i].relid);
	return &cs->scan.plan;
}
