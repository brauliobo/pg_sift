/*
 * bm25/planner_plan.c - the plans of the SiftScan and SiftCount paths: target lists, quals and private data of the
 * custom scans, and the path and plan methods.
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

#include "agg/agg.h"
#include "bm25/am.h"
#include "bm25/estimate.h"
#include "bm25/eval.h"
#include "bm25/planner.h"
#include "bm25/planner_int.h"
#include "bm25/pushdown.h"

static IndexOptInfo *
rel_index(RelOptInfo *rel, Oid indexoid)
{
	foreach_node(IndexOptInfo, ii, rel->indexlist)
		if (ii->indexoid == indexoid)
			return ii;
	elog(ERROR, "sift: index %u is not an index of the planned relation", indexoid);
	return NULL;
}

/* Vars of the rel the scan hands to its parent, its own quals and sort keys, then the sift.score expressions the query and the cutoffs read */
static List *
scan_tlist(PlannerInfo *root, RelOptInfo *rel, List *residual, List *keys, List *cuts)
{
	List	   *vars = pull_var_clause((Node *) list_concat(list_concat_copy(rel->reltarget->exprs, residual), list_copy(keys)),
									   PVC_RECURSE_PLACEHOLDERS),
			   *exprs = NIL,
			   *tlist = NIL;
	int			resno = 1;
	ScoreRefs	refs = {rel->relid, score_exprs(root, rel)};

	foreach_node(Var, v, vars)
		if (v->varno == rel->relid)
			exprs = list_append_unique(exprs, v);
	score_walker((Node *) cuts, &refs);
	exprs = list_concat(exprs, refs.found);
	foreach_ptr(Node, e, exprs)
		tlist = lappend(tlist, makeTargetEntry((Expr *) e, resno++, NULL, false));
	return tlist;
}

/*
 * The scan also outputs the scores the query reads: PG evaluates an expression above a join in the
 * join's own target unless a child already outputs it, and only the scan can compute sift.score
 */
static List *
score_outputs(PlannerInfo *root, RelOptInfo *rel, List *tlist)
{
	foreach_ptr(Node, e, score_exprs(root, rel))
		if (tlist_member((Expr *) e, tlist) == NULL)
			tlist = lappend(list_copy(tlist), makeTargetEntry((Expr *) e, list_length(tlist) + 1, NULL, true));
	return tlist;
}

/*
 * LIMIT $n [OFFSET $m] of a generic plan: its limit cannot be costed, but the scan reads the parameters when
 * it starts (the plan's k hint is what root->limit_tuples says, which for a parameter is nothing)
 */
static List *
limit_params(PlannerInfo *root, CustomPath *best, bool from_above)
{
	if (from_above || DatumGetInt64(list_nth_node(Const, best->custom_private, SIFT_PRIV_K)->constvalue) > 0 ||
		DatumGetInt32(list_nth_node(Const, best->custom_private, SIFT_PRIV_MODE)->constvalue) != SIFT_MODE_RANKED ||
		(best->path.pathkeys == NIL && root->query_pathkeys != NIL))
		return NIL;
	return param_limits(root);
}

static Plan *
sift_plan_scan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses,
			   List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	IndexOptInfo *ii = rel_index(rel, DatumGetObjectId(((Const *) linitial(best->custom_private))->constvalue));
	List	   *keys = list_nth_node(List, best->custom_private, SIFT_NPRIV + 1),
			   *residual = NIL,
			   *cuts = NIL,
			   *cut_exprs;
	bool		ranked = DatumGetInt32(list_nth_node(Const, best->custom_private, SIFT_PRIV_MODE)->constvalue) == SIFT_MODE_RANKED;

	/* the clauses come in the order PostgreSQL runs them in: by security level first, so a policy's quals are the residual's first */
	foreach_node(RestrictInfo, rinfo, clauses)
	{
		Expr	   *q;

		if (scan_match_clause(root, (Node *) rinfo, ii, ranked, &q))
			continue;
		if (score_cutoff((Node *) rinfo->clause, rel->relid, NULL))
			cuts = lappend(cuts, rinfo);
		else
			residual = lappend(residual, rinfo);
	}
	residual = extract_actual_clauses(residual, false);
	cut_exprs = extract_actual_clauses(cuts, false);
	cs->scan.plan.targetlist = score_outputs(root, rel, tlist);
	cs->scan.scanrelid = rel->relid;
	cs->flags = best->flags;
	cs->custom_scan_tlist = scan_tlist(root, rel, residual, keys, cut_exprs);
	cs->custom_private = list_copy_head(best->custom_private, SIFT_NPRIV);
	cs->custom_exprs = list_make5(copyObject(list_nth(best->custom_private, SIFT_NPRIV)), residual,
								  limit_params(root, best, cuts_from_above(cuts, rel->relid)), copyObject(keys), cut_exprs);
	if (rel_secured(root, rel))
		cs->custom_exprs = lappend(cs->custom_exprs, NIL);
	cs->methods = &sift_scan_plan_methods;
	root->glob->relationOids = lappend_oid(root->glob->relationOids, ii->indexoid);
	if (custom_plans != NIL)
	{
		/* the bitmap heap scan was planned for its bitmap: the child is the bitmap plan under it */
		cs->custom_plans = list_make1(outerPlan(linitial(custom_plans)));
		sift_filter_plan_deps(root, linitial(cs->custom_plans));
	}
	return &cs->scan.plan;
}

static Plan *
sift_plan_count(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses,
				List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);

	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->flags = best->flags;
	cs->custom_scan_tlist = tlist;
	cs->custom_private = list_copy_head(best->custom_private, SIFT_NPRIV);
	cs->custom_exprs = list_make5(copyObject(list_nth(best->custom_private, SIFT_NPRIV)), NIL, NIL, NIL, NIL);
	cs->methods = &sift_count_plan_methods;
	root->glob->relationOids = lappend_oid(root->glob->relationOids,
										   DatumGetObjectId(((Const *) linitial(best->custom_private))->constvalue));
	return &cs->scan.plan;
}

const CustomPathMethods sift_scan_path_methods = {"SiftScan", sift_plan_scan, NULL};
const CustomPathMethods sift_count_path_methods = {"SiftCount", sift_plan_count, NULL};
const CustomScanMethods sift_scan_plan_methods = {"SiftScan", sift_create_scan_state};
const CustomScanMethods sift_count_plan_methods = {"SiftCount", sift_create_scan_state};
