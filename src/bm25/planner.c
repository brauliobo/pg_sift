/*
 * bm25/planner.c - planner side of the index: amcostestimate, the matching of
 * expressions to index keys, and the hooks that add SiftScan (heap-order or
 * ranked) and SiftCount paths (DESIGN.md (f)). Execution is in bm25/cscan.c.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/stratnum.h"
#include "access/sysattr.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"

#include "sift_agg.h"
#include "bm25/am.h"
#include "bm25/estimate.h"
#include "bm25/eval.h"
#include "bm25/planner.h"
#include "bm25/planner_int.h"
#include "bm25/pushdown.h"

static set_rel_pathlist_hook_type prev_pathlist_hook;
static set_join_pathlist_hook_type prev_join_hook;
static create_upper_paths_hook_type prev_upper_hook;

/* Splits the rel's restrictions for one sift index; false when none is a sift qual of it */
static bool
split_quals(RelOptInfo *rel, IndexOptInfo *ii, ScanCand *c)
{
	c->index = ii;
	c->queries = c->residual = NIL;
	if (ii->relam != sift_catalog()->am || ii->ncolumns != 1 || ii->hypothetical || (ii->indpred != NIL && !ii->predOK))
		return false;
	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
	{
		Expr	   *q;

		if (scan_match_clause((Node *) rinfo, ii, &q))
			c->queries = lappend(c->queries, q);
		else
			c->residual = lappend(c->residual, rinfo);
	}
	return c->queries != NIL;
}

/* The restrictions of the rel that are cutoffs of the score */
static List *
score_cutoffs(RelOptInfo *rel)
{
	List	   *cuts = NIL;

	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
		if (score_cutoff((Node *) rinfo->clause, rel->relid, NULL))
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

/* Does the rel have a restriction that is a sift match, of whichever index? */
static bool
has_match_clause(RelOptInfo *rel)
{
	const SiftCatalog *cat = sift_catalog();

	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
	{
		Node	   *clause = (Node *) rinfo->clause;

		if ((IsA(clause, OpExpr) && ((OpExpr *) clause)->opno == cat->match_op) ||
			(IsA(clause, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) clause)->opno == cat->match_op) ||
			(IsA(clause, FuncExpr) && (((FuncExpr *) clause)->funcid == cat->match_func ||
									   ((FuncExpr *) clause)->funcid == cat->match_idx_func)))
			return true;
	}
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
		return ((CustomPath *) path)->methods == &sift_scan_path_methods;
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
	if (!holds_all_rels(root, joinrel))
		carry_join_scores(root, joinrel);
}

/* The statement that has no SiftScan to rank its rows: the ones that lock or change them re-check each row, which a scan cannot */
static const char *
unranked_statement(PlannerInfo *root)
{
	switch (root->parse->commandType)
	{
		case CMD_UPDATE:
			return "UPDATE";
		case CMD_DELETE:
			return "DELETE";
		case CMD_MERGE:
			return "MERGE";
		default:
			return "FOR UPDATE and FOR SHARE";
	}
}

static void
reject_scores(PlannerInfo *root, RelOptInfo *rel)
{
	ScoreRefs	refs = {rel->relid, score_exprs(root, rel)};

	score_walker((Node *) root->parse->returningList, &refs);
	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
		score_walker((Node *) rinfo->clause, &refs);
	if (refs.found != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift.score() is not available in %s", unranked_statement(root)),
				 errhint("Rank the rows in a SELECT, and change or lock them by key in the statement that uses its result.")));
}

/*
 * A score the query reads can only come from a ranked scan, so that path
 * replaces the others; without one the unordered scan competes with them.
 */
static void
sift_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	ScanCand	c;
	ScanEst		est;
	PathKey    *pk = NULL;
	List	   *scores,
			   *cuts;
	KeyedOrder	ko;
	bool		ranked,
				only_best,
				keyed,
				filter_looked = false;
	BitmapHeapPath *filter = NULL;
	CustomPath *best = NULL,
			   *best_partial = NULL,
			   *best_keyed = NULL;

	if (prev_pathlist_hook)
		prev_pathlist_hook(root, rel, rti, rte);
	if (!sift_enable_scan || rte->rtekind != RTE_RELATION || rte->tablesample ||
		(rel->reloptkind != RELOPT_BASEREL && rel->reloptkind != RELOPT_OTHER_MEMBER_REL) ||
		sift_catalog()->am == InvalidOid)
		return;
	if (root->parse->commandType != CMD_SELECT || root->rowMarks != NIL)
	{
		reject_scores(root, rel);
		return;
	}
	if (rte->inh)
	{
		if (!holds_all_rels(root, rel))
			carry_parent_scores(root, rel);
		return;
	}
	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
		return;
	scores = score_exprs(root, rel);
	if (is_partition(rel))
		check_partition_scores(rel, scores);
	cuts = score_cutoffs(rel);
	ranked = sorted_by_score(root, rel, &pk);
	only_best = ranked || scores != NIL || cuts != NIL;
	/* the rows a query with a limit takes from a scan that sorts by its leading keys and then by the score */
	keyed = only_best && !ranked && cuts == NIL && !is_partition(rel) && !reads_max_score(rel, scores) &&
		(query_limit(root) > 0 || param_limits(root) != NIL) && keyed_order(root, rel, &ko);
	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
	{
		ScoreRefs	refs = {rti, NIL};

		score_walker((Node *) rinfo->clause, &refs);
		if (refs.found != NIL && !list_member_ptr(cuts, rinfo))
			return;
	}
	foreach_node(IndexOptInfo, ii, rel->indexlist)
	{
		int64		k = cuts_from_above(cuts, rti) ? 0 : scan_limit(root, ranked);
		SiftScanMode mode = only_best ? SIFT_MODE_RANKED : SIFT_MODE_UNORDERED;
		List	   *pathkeys = ranked ? list_make1(pk) : NIL;
		/*
		 * A ranked scan shares its threshold, which needs the number of rows it is asked for, and
		 * only the scan can compute the score, so nothing may sit between it and the query's
		 * target list: not a Gather below a join, which would not pass the score on
		 */
		bool		parallel = mode == SIFT_MODE_UNORDERED ||
			(ranked && k > 0 && bms_membership(root->all_baserels) == BMS_SINGLETON);

		if (!split_quals(rel, ii, &c))
			continue;
		if (mode == SIFT_MODE_RANKED && !filter_looked)
		{
			filter = sift_filter_bitmap_path(root, rel);
			filter_looked = true;
		}
		scan_est(root, rel, &c, mode, k, filter, &est);
		if (mode == SIFT_MODE_UNORDERED && any_list_too_long(root, &c, &est.sc))
			continue;
		if (keyed)
		{
			CustomPath *cp = keyed_path(root, rel, &c, &est, &ko, query_limit(root));

			if (cheaper(cp, best_keyed))
				best_keyed = cp;
		}
		if (mode == SIFT_MODE_RANKED && !est.sc.idx.known && sift_filter_pushdown != SIFT_PUSHDOWN_FORCE)
		{
			unknown_query_path(root, rel, &c, &est, k, pathkeys, filter, parallel, &best, &best_partial);
			continue;
		}
		if (filter == NULL || sift_filter_pushdown != SIFT_PUSHDOWN_FORCE)
			add_scan_paths(root, rel, &c, &est, mode, k, pathkeys, filter, false, parallel, only_best, &best, &best_partial);
		if (filter)
			add_scan_paths(root, rel, &c, &est, mode, k, pathkeys, filter, true, false, only_best, &best, &best_partial);
	}
	if (best && only_best)
	{
		rel->pathlist = list_make1(best);
		if (best_keyed)
			rel->pathlist = lappend(rel->pathlist, best_keyed);
		rel->partial_pathlist = best_partial ? list_make1(best_partial) : NIL;
	}
	else if (only_best && rte->securityQuals != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift.score() is not available on \"%s\" while row-level security or a security barrier view applies", get_rel_name(rte->relid)),
				 errdetail("A ranked scan counts the rows that the policy hides in the scores.")));
	else if (only_best && is_partition(rel) && has_match_clause(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift.score() needs a sift index on every partition the query reads"),
				 errdetail("\"%s\" has no sift index that answers the @@> condition.", get_rel_name(rte->relid))));
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
count_cand(RelOptInfo *rel, ScanCand *c)
{
	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (split_quals(rel, ii, c) && c->residual == NIL)
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

static void
sift_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel,
				 void *extra)
{
	Query	   *parse = root->parse;
	ScanCand	c;
	CustomPath *cp;
	RangeTblEntry *rte;

	if (prev_upper_hook)
		prev_upper_hook(root, stage, input_rel, output_rel, extra);
	sift_agg_upper_paths(root, stage, input_rel, output_rel, extra);
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
	if (stage != UPPERREL_GROUP_AGG || !sift_enable_count || sift_catalog()->am == InvalidOid ||
		!parse->hasAggs || parse->groupClause != NIL || parse->groupingSets != NIL || parse->hasWindowFuncs ||
		parse->hasTargetSRFs || parse->havingQual != NULL || parse->commandType != CMD_SELECT ||
		root->rowMarks != NIL || input_rel->reloptkind != RELOPT_BASEREL ||
		bms_membership(input_rel->relids) != BMS_SINGLETON)
		return;
	rte = root->simple_rte_array[input_rel->relid];
	if (rte->rtekind != RTE_RELATION || (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW) || rte->tablesample || rte->inh)
		return;
	foreach_ptr(Node, e, output_rel->reltarget->exprs)
		if (!plain_count(e))
			return;
	if (!count_cand(input_rel, &c))
		return;
	cp = count_path(root, input_rel, output_rel, &c, rte->relid, 0);
	add_path(output_rel, &cp->path);
	add_parallel_count(root, input_rel, output_rel, &c, rte->relid);
}

void
sift_planner_init(void)
{
	RegisterCustomScanMethods(&sift_scan_plan_methods);
	RegisterCustomScanMethods(&sift_count_plan_methods);
	prev_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = sift_set_rel_pathlist;
	prev_join_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = sift_set_join_pathlist;
	prev_upper_hook = create_upper_paths_hook;
	create_upper_paths_hook = sift_upper_paths;
}
