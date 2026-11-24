/*
 * agg/v6/v6_unnest_exists.c - the planning of an EXISTS over the elements of an array column as the aggregate scan of an unnest (agg/v6/v6_unnest_exists.h).
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"

#include "agg/v6/plan_int.h"
#include "agg/v6/v6_ext.h"
#include "agg/v6/v6_unnest_exists.h"
#include "agg/v6/v6_unnest_jsonb.h"

/* the cost the scan of the table has for each row of it, in operators: the leaves of the elements are read for every granule of the index (the pull prunes none by the quals of the table) */
#define EXISTS_ROW_OPERATORS	50

/* the sub-plan of an EXISTS whose sub-select is a single function scan, and the root it was planned under; NULL for any other clause */
static const SubPlan *
exists_subplan(const PlannerInfo *root, const RestrictInfo *ri, const PlannerInfo **sub)
{
	const SubPlan *sp = (const SubPlan *) ri->clause;
	const Query *q;

	if (!IsA(ri->clause, SubPlan) || sp->subLinkType != EXISTS_SUBLINK || sp->useHashTable || sp->testexpr != NULL || list_length(sp->args) != 1 || list_length(sp->parParam) != 1)
		return NULL;
	*sub = list_nth(root->glob->subroots, sp->plan_id - 1);
	q = (*sub)->parse;
	if (list_length(q->rtable) != 1 || q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->hasSubLinks || q->groupClause != NIL || q->havingQual != NULL || q->limitCount != NULL ||
		q->setOperations != NULL || q->cteList != NIL)
		return NULL;
	return sp;
}

/* the one EXISTS of the quals of the table, which no other EXISTS or sub-plan accompanies: the scan cannot decide a sub-plan it has not planned */
static const RestrictInfo *
the_exists(const PlannerInfo *root, const RelOptInfo *rel, const SubPlan **sp, const PlannerInfo **sub)
{
	const RestrictInfo *found = NULL;

	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
	{
		if (!contain_subplans((Node *) ri->clause))
			continue;
		if (found != NULL || (*sp = exists_subplan(root, ri, sub)) == NULL)
			return NULL;
		found = ri;
	}
	return found;
}

static Cost
exists_cost(const RelOptInfo *rel)
{
	return rel->tuples * cpu_operator_cost * EXISTS_ROW_OPERATORS;
}

static void
try_index(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, GroupPathExtraData *extra, List *group_exprs, IndexOptInfo *ix, const RestrictInfo *sub_ri, const SiftV6ElemRte *rte,
		  RelOptInfo *elem_rel)
{
	V6Plan		p = {.root = root,.rel = rel,.out = out,.relid = ix->indexoid,.index = ix,.sub_qual = sub_ri};
	Relation	irel = index_open(ix->indexoid, AccessShareLock);
	bool		ok;

	sift_v6_plan_index(&p, irel);
	ok = p.ops != NULL && sift_v6_unnest_seed(&p, elem_rel, rte, false);
	if (ok)
	{
		p.xseed->elem.semi = true;
		p.elem_rel = NULL;
		ok = sift_v6_unnest_compile(&p, extra, group_exprs);
		p.elem_rel = elem_rel;
		ok = ok && sift_v6_unnest_elem_quals(&p) && sift_v6_unnest_where(&p);
		p.elem_rel = NULL;
	}
	if (ok)
		sift_v6_add_join_path(&p, group_exprs, (List *) extra->havingQual, NIL, 0, exists_cost(rel), rel->rows);
	index_close(irel, NoLock);
}

void
sift_v6_unnest_exists_paths(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, GroupPathExtraData *extra, List *group_exprs)
{
	const SubPlan *sp = NULL;
	const PlannerInfo *sub = NULL;
	const RestrictInfo *sub_ri;
	SiftV6ElemRte rte;
	RelOptInfo *elem_rel;

	if (!sift_v6_plan_fact_ok(root, rel) || (sub_ri = the_exists(root, rel, &sp, &sub)) == NULL)
		return;
	elem_rel = sub->simple_rel_array[1];
	if (elem_rel == NULL || !sift_v6_unnest_exists_rte(planner_rt_fetch(1, (PlannerInfo *) sub), sp, &rte) || !sift_v6_unnest_array_ok(root, rel, rte.array))
		return;
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
		if (sift_v6_index_owned(ix))
			try_index(root, rel, out, extra, group_exprs, ix, sub_ri, &rte, elem_rel);
}
