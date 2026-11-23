/*
 * bm25/planner.c - planner side of the index: amcostestimate, the matching of
 * expressions to index keys, and the hooks that add SiftScan (heap-order or
 * ranked) and SiftCount paths. Execution is in bm25/cscan.c.
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/agg.h"
#include "agg/plan_int.h"
#include "agg/v6/v6.h"
#include "agg/v6/v6_join_push.h"
#include "agg/v6/v6_join.h"
#include "agg/v6/v6_live.h"
#include "core/compat.h"
#include "core/visibility.h"
#include "bm25/am.h"
#include "bm25/planner.h"
#include "bm25/planner_int.h"
#include "bm25/score_fallback.h"
#include "bm25/pushdown.h"

static set_rel_pathlist_hook_type prev_pathlist_hook;
static set_join_pathlist_hook_type prev_join_hook;
static create_upper_paths_hook_type prev_upper_hook;
static planner_hook_type prev_planner_hook;

/* Splits the rel's restrictions for one sift index; false when none is a sift qual of it */
static bool
split_quals(PlannerInfo *root, RelOptInfo *rel, IndexOptInfo *ii, bool ranked, ScanCand *c)
{
	c->index = ii;
	c->queries = c->residual = NIL;
	c->late_k = c->late_rows = 0;
	c->attr = NIL;
	c->attr_sel = 1.0;
	c->attr_all = false;
	c->semi = NIL;
	c->semi_rels = NULL;
	if (ii->relam != sift_catalog()->am || ii->hypothetical || (ii->indpred != NIL && !ii->predOK))
		return false;
	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
	{
		Expr	   *q;

		if (scan_match_clause(root, (Node *) rinfo, ii, ranked, &q))
			c->queries = lappend(c->queries, q);
		else
			c->residual = lappend(c->residual, rinfo);
	}
	if (c->queries != NIL && ranked && ii->ncolumns > ii->nkeycolumns)
	{
		double		sel = 1.0;

		c->attr = scan_attr_tests(root, rel, ii, c->residual, &c->attr_sel, &c->attr_all);
		c->semi = scan_semi_tests(root, rel, ii, &c->semi_rels, &sel);
		c->attr_sel *= sel;
	}
	return c->queries != NIL;
}

/* The restrictions of the rel that read the score: a cutoff, or any other expression of it, which the scan evaluates on the row it scored */
static List *
score_cutoffs(RelOptInfo *rel)
{
	List	   *cuts = NIL;

	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
		if (reads_score_of((Node *) rinfo->clause, rel->relid))
			cuts = lappend(cuts, rinfo);
	return cuts;
}

/* Does the query read sift.max_score of the rel, the best score of all its matches (a keyed scan scores few of them)? */
static bool
reads_max_score(RelOptInfo *rel, List *scores)
{
	bool		is_max;

	foreach_ptr(Node, e, scores)
		if (sift_is_score_expr(e, rel->relid, &is_max) && is_max)
			return true;
	return false;
}

/*
 * The max_score of a query is one value for all its rows, but each partition's scan only knows its own
 * (and ranks with its own index statistics), so it is not available through a partitioned parent.
 */
static void
check_partition_scores(RelOptInfo *rel, List *scores)
{
	bool		is_max;

	foreach_ptr(Node, e, scores)
		if (sift_is_score_expr(e, rel->relid, &is_max) && is_max)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("sift.max_score() is not available through a partitioned table"),
					 errhint("Call it on one partition, or use sift.score().")));
}

/* Is the path a SiftScan, or an Append of them, projected or not? */
static bool
scans_only(Path *path)
{
	List	   *subpaths;

	while (IsA(path, ProjectionPath))
		path = ((ProjectionPath *) path)->subpath;
	if (IsA(path, CustomPath))
		return ((CustomPath *) path)->methods == &sift_scan_path_methods ||
			((CustomPath *) path)->methods == &sift_fallback_path_methods;
	if (IsA(path, AppendPath))
		subpaths = ((AppendPath *) path)->subpaths;
	else if (IsA(path, MergeAppendPath))
		subpaths = ((MergeAppendPath *) path)->subpaths;
	else
		return false;
	foreach_ptr(Path, sub, subpaths)
		if (!scans_only(sub))
			return false;
	return true;
}

static bool
is_append(Path *path)
{
	return IsA(path, AppendPath) || IsA(path, MergeAppendPath);
}

static bool
has_scan_path(RelOptInfo *rel)
{
	foreach_ptr(Path, p, rel->pathlist)
		if (scans_only(p))
			return true;
	return false;
}

/* The scores the query reads of the rels under a join that a SiftScan produces */
static List *
join_scores(PlannerInfo *root, RelOptInfo *joinrel)
{
	List	   *scores = NIL;
	int			relid = -1;

	while ((relid = bms_next_member(joinrel->relids, relid)) >= 0)
	{
		RelOptInfo *rel = root->simple_rel_array[relid];

		if (rel != NULL && rel->reloptkind == RELOPT_BASEREL && has_scan_path(rel))
			scores = list_concat_unique(scores, score_exprs(root, rel));
	}
	return scores;
}

/*
 * The path's own target also outputs the scores: the rel's target holds only Vars, so a join above would
 * call sift.score itself. A path of the rel that holds every base rel gets its target from the query's
 * scan/join target instead (and its sort and group references), which has the scores it needs.
 */
static void
carry_scores(Path *path, List *scores)
{
	foreach_ptr(Node, e, scores)
	{
		if (list_member(path->pathtarget->exprs, e))
			continue;
		if (path->pathtarget == path->parent->reltarget)
			path->pathtarget = copy_pathtarget(path->pathtarget);
		add_column_to_pathtarget(path->pathtarget, (Expr *) e, 0);
	}
}

static bool
holds_all_rels(PlannerInfo *root, RelOptInfo *rel)
{
	return bms_is_subset(root->all_baserels, rel->relids);
}

/*
 * The Append of a partitioned parent outputs the scores of its partitions' scans, which come last in
 * their target lists (score_outputs), so that the joins and aggregates above it find them. An Append
 * takes its columns by position: only one of scans is extended.
 */
static void
carry_parent_scores(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *scores = score_exprs(root, rel);

	foreach_ptr(Path, p, rel->pathlist)
		if (is_append(p) && scans_only(p))
			carry_scores(p, scores);
}

/* The Append of a partitionwise join takes the columns of the joins under it, which do not carry the scores */
static void
carry_join_scores(PlannerInfo *root, RelOptInfo *joinrel)
{
	List	   *scores = join_scores(root, joinrel);

	foreach_ptr(Path, p, joinrel->pathlist)
		if (!is_append(p))
			carry_scores(p, scores);
	foreach_ptr(Path, p, joinrel->partial_pathlist)
		if (!is_append(p))
			carry_scores(p, scores);
}

/*
 * A join passes on the columns the joins above it need, and those are Vars: the score a join higher up
 * reads is not one, and would be evaluated there from the ctid. The join paths pass the scan's score on.
 */
static void
sift_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel,
					   JoinType jointype, JoinPathExtraData *extra)
{
	if (prev_join_hook)
		prev_join_hook(root, joinrel, outerrel, innerrel, jointype, extra);
	if (sift_v6_rels_have_index(root, joinrel->relids))
	{
		sift_agg_v6_join_paths(root, joinrel, outerrel, innerrel, jointype, extra);
		sift_agg_v6_asof_paths(root, joinrel, outerrel, innerrel, jointype, extra);
	}
	if (!holds_all_rels(root, joinrel))
		carry_join_scores(root, joinrel);
}

/* A RETURNING list is computed from the new row version, which no scan scored */
static void
reject_returning_scores(PlannerInfo *root, RelOptInfo *rel)
{
	ScoreRefs	refs = {rel->relid, score_exprs(root, rel)};

	score_walker((Node *) root->parse->returningList, &refs);
	if (refs.found != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift.score() is not available in RETURNING"),
				 errhint("Select the score in a subquery or a WITH query of the statement, and change the rows it returns by key.")));
}

/* what the paths of a relation are chosen from: the shape of the query, the filter bitmap, and the best path of each kind so far */
typedef struct PathSearch
{
	PlannerInfo *root;
	RelOptInfo *rel;
	PathKey    *pk;				/* the score order, when ranked */
	List	   *cuts;
	KeyedOrder	ko;
	bool		ranked,
				only_best,
				keyed,
				filter_looked;
	BitmapHeapPath *filter;
	CustomPath *best,
			   *best_partial,
			   *best_keyed;
} PathSearch;

/* true when the relation is one a sift scan can serve; an inheritance parent only records its scores */
static bool
scannable(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	if (rte->rtekind != RTE_RELATION || rte->tablesample ||
		(rel->reloptkind != RELOPT_BASEREL && rel->reloptkind != RELOPT_OTHER_MEMBER_REL) ||
		sift_catalog()->am == InvalidOid)
		return false;
	if (root->parse->returningList != NIL)
		reject_returning_scores(root, rel);
	if (rte->inh)
	{
		carry_parent_scores(root, rel);
		return false;
	}
	return rte->relkind == RELKIND_RELATION || rte->relkind == RELKIND_MATVIEW;
}

/* the paths one index offers, kept when they beat the best so far */
static void
consider_index(PathSearch *ps, IndexOptInfo *ii, Index rti)
{
	PlannerInfo *root = ps->root;
	RelOptInfo *rel = ps->rel;
	ScanCand	c;
	ScanEst		est;
	int64		k = cuts_from_above(ps->cuts, rti) ? 0 : scan_limit(root, ps->ranked),
				k0 = k;
	SiftScanMode mode = ps->only_best ? SIFT_MODE_RANKED : SIFT_MODE_UNORDERED;
	List	   *pathkeys = ps->ranked ? list_make1(ps->pk) : NIL;
	/*
	 * A ranked scan shares its threshold, which needs the number of rows it is asked for, and
	 * only the scan can compute the score, so nothing may sit between it and the query's
	 * target list: not a Gather below a join, which would not pass the score on
	 */
	bool		parallel = (mode == SIFT_MODE_UNORDERED ||
							(ps->ranked && k > 0 && bms_membership(root->all_baserels) == BMS_SINGLETON)) &&
		root->parse->commandType == CMD_SELECT && root->rowMarks == NIL;

	if (!split_quals(root, rel, ii, ps->only_best, &c))
		return;
	if (mode == SIFT_MODE_RANKED)
		k = joined_limit(root, rel, k, 1.0, c.semi_rels);
	if (mode == SIFT_MODE_RANKED && !ps->filter_looked)
	{
		ps->filter = sift_filter_bitmap_path(root, rel);
		ps->filter_looked = true;
	}
	scan_est(root, rel, &c, mode, ps->filter, &est);
	if (mode == SIFT_MODE_RANKED && late_quals(root, rel, &c, &est.sc, k0))
	{
		int64		asked = joined_limit(root, rel, k0, est.sc.pass, c.semi_rels);

		/* asked for a quarter of the matches or more, the search is the same one */
		if (asked > k && est.sc.docs > 4 * asked)
		{
			c.late_k = asked;
			c.late_rows = k0;
		}
	}
	if (mode == SIFT_MODE_UNORDERED && any_list_too_long(root, &c, &est.sc))
		return;
	if (ps->keyed)
	{
		CustomPath *cp = keyed_path(root, rel, &c, &est, &ps->ko, query_limit(root));

		if (cp && cheaper(cp, ps->best_keyed))
			ps->best_keyed = cp;
	}
	if (mode == SIFT_MODE_RANKED && !est.sc.idx.known && sift_filter_pushdown != SIFT_PUSHDOWN_FORCE)
		unknown_query_path(root, rel, &c, &est, k, pathkeys, ps->filter, parallel, &ps->best, &ps->best_partial);
	else
	{
		if (ps->filter == NULL || sift_filter_pushdown != SIFT_PUSHDOWN_FORCE)
			add_scan_paths(root, rel, &c, &est, mode, k, pathkeys, ps->filter, false, parallel, ps->only_best, &ps->best, &ps->best_partial);
		if (ps->filter)
			add_scan_paths(root, rel, &c, &est, mode, k, pathkeys, ps->filter, true, parallel, ps->only_best, &ps->best, &ps->best_partial);
	}
}

/* the best paths replace the others when a score is read; the scan that scores the heap rows stands in where no index scan serves it */
static void
publish_paths(PathSearch *ps)
{
	RelOptInfo *rel = ps->rel;

	if (ps->best && ps->only_best)
	{
		rel->pathlist = list_make1(ps->best);
		if (ps->best_keyed)
			rel->pathlist = lappend(rel->pathlist, ps->best_keyed);
		rel->partial_pathlist = ps->best_partial ? list_make1(ps->best_partial) : NIL;
	}
	else if (ps->only_best)
		sift_fallback_publish(ps->root, rel);
}

/*
 * A score the query reads can only come from a ranked scan, so that path
 * replaces the others; without one the unordered scan competes with them.
 */
static void
sift_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	PathSearch	ps = {.root = root, .rel = rel};
	List	   *scores;

	if (prev_pathlist_hook)
		prev_pathlist_hook(root, rel, rti, rte);
	if (rte->rtekind == RTE_SUBQUERY || rte->rtekind == RTE_CTE)
		derived_rel_rows(root, rel);
	if (rte->rtekind == RTE_SUBQUERY)
		agg_subquery_ordered(root, rel, rti);
	if (rte->rtekind == RTE_RELATION)
	{
		agg_range_join_rows(root, rel, rte);
		sift_agg_minmax_paths(root, rel);
		if (sift_v6_rels_have_index(root, rel->relids))
			sift_agg_v6_rel_paths(root, rel, rti, rte);
	}
	if (rte->rtekind == RTE_CTE)
		derived_cte_rows(root, rel);
	if (!scannable(root, rel, rte))
		return;
	/* a relation the planner proved empty is left out of root->total_table_pages, which the page costs of a scan divide by */
	if (IS_DUMMY_REL(rel))
		return;
	scores = score_exprs(root, rel);
	if (is_partition(rel))
		check_partition_scores(rel, scores);
	ps.cuts = score_cutoffs(rel);
	ps.ranked = sorted_by_score(root, rel, &ps.pk);
	ps.only_best = ps.ranked || scores != NIL || ps.cuts != NIL;
	/*
	 * the rows a query with a limit takes from a scan that sorts by its leading keys and then by the score; not on a rel with
	 * security quals, as such a scan rechecks a match on the row's text before it runs the quals (next_row)
	 */
	ps.keyed = ps.only_best && !ps.ranked && ps.cuts == NIL && !is_partition(rel) && !reads_max_score(rel, scores) &&
		(query_limit(root) > 0 || param_limits(root) != NIL) && !rel_secured(root, rel) && keyed_order(root, rel, &ps.ko);
	/* a statement that changes or locks the rows it reads is served only where it reads a score: its other scans stay the planner's */
	if (!ps.only_best && (root->parse->commandType == CMD_UPDATE || root->parse->commandType == CMD_DELETE ||
						  root->parse->commandType == CMD_MERGE || root->rowMarks != NIL))
		return;
	if (sift_rls_scores && reads_max_score(rel, scores) && rel_secured(root, rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift.max_score() is not available on \"%s\" while row-level security or a security barrier view applies", get_rel_name(rte->relid)),
				 errdetail("It is the best score of all the matches, the rows that the policy hides included."),
				 errhint("Use sift.score(), which is that of a row the role may see.")));
	if (sift_enable_scan)
		foreach_node(IndexOptInfo, ii, rel->indexlist)
			consider_index(&ps, ii, rti);
	publish_paths(&ps);
}

/* count(*) and count(<constant>) without DISTINCT, FILTER or ORDER BY */
static bool
plain_count(Node *expr)
{
	Aggref	   *agg;

	if (!IsA(expr, Aggref))
		return false;
	agg = (Aggref *) expr;
	if (agg->aggdistinct != NIL || agg->aggorder != NIL || agg->aggfilter != NULL || agg->aggkind != AGGKIND_NORMAL ||
		agg->agglevelsup != 0 || agg->aggsplit != AGGSPLIT_SIMPLE)
		return false;
	if (agg->aggfnoid == F_COUNT_)
		return true;
	if (agg->aggfnoid != F_COUNT_ANY || list_length(agg->args) != 1)
		return false;
	expr = (Node *) linitial_node(TargetEntry, agg->args)->expr;
	return IsA(expr, Const) && !((Const *) expr)->constisnull;
}

/* The index every restriction of the rel is a sift qual of */
static bool
count_cand(PlannerInfo *root, RelOptInfo *rel, ScanCand *c)
{
	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (split_quals(root, rel, ii, false, c) && c->residual == NIL)
			return true;
	return false;
}

/* A Sort over a plain Gather of a parallel ranked scan: it drains the scan and breaks its tie order */
static bool
sorted_gather_of_ranked(Path *path)
{
	Path	   *sub;

	if (!IsA(path, SortPath) || !IsA(((SortPath *) path)->subpath, GatherPath))
		return false;
	sub = ((GatherPath *) ((SortPath *) path)->subpath)->subpath;
	while (IsA(sub, ProjectionPath))
		sub = ((ProjectionPath *) sub)->subpath;
	return IsA(sub, CustomPath) && ((CustomPath *) sub)->methods == &sift_scan_path_methods && sub->parallel_aware &&
		DatumGetInt32(lsecond_node(Const, ((CustomPath *) sub)->custom_private)->constvalue) == SIFT_MODE_RANKED;
}

/* a bare count of a search is a path of its own, offered before the aggregate scan so that the scan's report (sift.explain_why) weighs it as a rival */
static void
count_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	Query	   *parse = root->parse;
	ScanCand	c;
	CustomPath *cp;
	RangeTblEntry *rte;

	if (stage != UPPERREL_GROUP_AGG || !sift_enable_scan || sift_catalog()->am == InvalidOid ||
		!parse->hasAggs || parse->groupClause != NIL || parse->groupingSets != NIL || parse->hasWindowFuncs ||
		parse->hasTargetSRFs || parse->havingQual != NULL || parse->commandType != CMD_SELECT ||
		root->rowMarks != NIL || input_rel->reloptkind != RELOPT_BASEREL ||
		bms_membership(input_rel->relids) != BMS_SINGLETON)
		return;
	rte = planner_rt_fetch(input_rel->relid, root);
	if (rte->rtekind != RTE_RELATION || (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW) || rte->tablesample || rte->inh)
		return;
	foreach_ptr(Node, e, output_rel->reltarget->exprs)
		if (!plain_count(e))
			return;
	if (!count_cand(root, input_rel, &c))
		return;
	cp = count_path(root, input_rel, output_rel, &c, rte->relid, 0);
	add_path(output_rel, &cp->path);
	add_parallel_count(root, input_rel, output_rel, &c, rte->relid);
}

static void
sift_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel,
				 void *extra)
{
	if (prev_upper_hook)
		prev_upper_hook(root, stage, input_rel, output_rel, extra);
	count_upper_paths(root, stage, input_rel, output_rel);
	sift_agg_upper_paths(root, stage, input_rel, output_rel, extra);
	sift_agg_v6_live_upper_paths(root, stage, input_rel, output_rel, extra);
	/* the aggregates read the scores out of the scan/join target, which holds only the Vars they use */
	if (stage == UPPERREL_GROUP_AGG && input_rel->reloptkind == RELOPT_JOINREL)
		carry_join_scores(root, input_rel);
	else if (stage == UPPERREL_GROUP_AGG && input_rel->reloptkind == RELOPT_BASEREL &&
			 planner_rt_fetch(input_rel->relid, root)->inh)
		carry_parent_scores(root, input_rel);
	if (stage == UPPERREL_ORDERED)
		foreach_ptr(Path, p, list_copy(output_rel->pathlist))
			if (sorted_gather_of_ranked(p))
				output_rel->pathlist = list_delete_ptr(output_rel->pathlist, p);
}

static bool
reads_score(Node *node, void *context)
{
	const SiftCatalog *cat = sift_catalog();

	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr) && (((FuncExpr *) node)->funcid == cat->score_func || ((FuncExpr *) node)->funcid == cat->max_score_func))
		return true;
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, reads_score, context, 0);
	return expression_tree_walker(node, reads_score, context);
}

static bool
collect_scores(Node *node, List **found)
{
	const SiftCatalog *cat = sift_catalog();

	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr) && (((FuncExpr *) node)->funcid == cat->score_func || ((FuncExpr *) node)->funcid == cat->max_score_func))
	{
		*found = list_append_unique(*found, node);
		return false;
	}
	return expression_tree_walker(node, collect_scores, found);
}

/*
 * A MERGE action is evaluated by the ModifyTable node from the row its plan outputs, which holds no score: the scores the actions read are
 * entered in the target list of the statement as junk columns, so that the scans compute them and the action finds them there
 */
static void
merge_score_outputs(Query *parse)
{
	List	   *found = NIL;

	collect_scores((Node *) parse->mergeActionList, &found);
	foreach_ptr(Node, e, found)
		parse->targetList = lappend(parse->targetList, makeTargetEntry((Expr *) copyObject(e), list_length(parse->targetList) + 1, NULL, true));
}

/*
 * A subquery or CTE of a statement that changes or locks rows that reads the score is planned apart, as written with OFFSET 0 or
 * MATERIALIZED (PostgreSQL pulls up neither), so that its scan ranks the rows it returns and not the statement's
 */
static PlannedStmt *
sift_score_planner(SIFT_PLANNER_PARAMS)
{
	PlannedStmt *stmt;

	if ((parse->commandType == CMD_UPDATE || parse->commandType == CMD_DELETE || parse->commandType == CMD_MERGE || parse->rowMarks != NIL) &&
		sift_enable_scan && sift_catalog()->am != InvalidOid)
	{
		foreach_node(RangeTblEntry, rte, parse->rtable)
			if (rte->rtekind == RTE_SUBQUERY && rte->subquery->limitOffset == NULL && rte->subquery->limitCount == NULL &&
				reads_score((Node *) rte->subquery, NULL))
				rte->subquery->limitOffset = (Node *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(0), false, FLOAT8PASSBYVAL);
		foreach_node(CommonTableExpr, cte, parse->cteList)
			if (cte->ctematerialized == CTEMaterializeDefault && reads_score(cte->ctequery, NULL))
				cte->ctematerialized = CTEMaterializeAlways;
		if (parse->commandType == CMD_MERGE)
			merge_score_outputs(parse);
	}
	sift_agg_walks_forget();
	sift_vis_scope(true);
	PG_TRY();
	{
		stmt = prev_planner_hook ? prev_planner_hook(SIFT_PLANNER_ARGS(parse)) : standard_planner(SIFT_PLANNER_ARGS(parse));
	}
	PG_FINALLY();
	{
		sift_agg_walks_forget();
		sift_vis_scope(false);
	}
	PG_END_TRY();
	sift_finish_plan(stmt, sift_enable_scan);
	return stmt;
}

void
sift_planner_init(void)
{
	RegisterCustomScanMethods(&sift_scan_plan_methods);
	RegisterCustomScanMethods(&sift_count_plan_methods);
	RegisterCustomScanMethods(&sift_fallback_plan_methods);
	RegisterCustomScanMethods(&sift_agg_v6_plan_methods);
	RegisterCustomScanMethods(&sift_agg_v6_join_plan_methods);
	RegisterCustomScanMethods(&sift_agg_v6_asof_plan_methods);
	RegisterCustomScanMethods(&sift_agg_v6_push_plan_methods);
	sift_agg_v6_live_register();
	prev_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = sift_set_rel_pathlist;
	prev_join_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = sift_set_join_pathlist;
	prev_upper_hook = create_upper_paths_hook;
	create_upper_paths_hook = sift_upper_paths;
	prev_planner_hook = planner_hook;
	planner_hook = sift_score_planner;
}
