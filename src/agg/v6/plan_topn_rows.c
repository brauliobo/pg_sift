/*
 * agg/v6/plan_topn_rows.c - the path of SiftAggV6 for ORDER BY columns LIMIT K of the rows of one table: the rows of the winners come from the scan of the index.
 */
/* check-memory: allow-allocs every array here follows the sort keys of one statement, not the rows of the table */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"

#include "agg/v6/plan_int.h"
#include "agg/v6/v6.h"
#include "agg/v6/v6_card.h"
#include "agg/v6/v6_plan_rewrite.h"
#include "agg/v6/v6_vexpr.h"
#include "core/store_ops.h"

/* a ready projection of a format serves the row query of the relation: its own path stands among those of the relation */
static bool
proj_sort_offered(const RelOptInfo *rel)
{
	for (int i = 0; i < sift_store_ops_count(); i++)
		if (sift_store_ops_at(i)->proj != NULL && sift_store_ops_at(i)->proj->sort_offered(rel))
			return true;
	return false;
}

/* the sort columns of the query as the keys of the spec: columns of the index the scan reads whose values PostgreSQL's own comparison orders */
static bool
rows_keys(V6Plan *p)
{
	const Query *q = p->root->parse;

	if (list_length(q->sortClause) > SIFT_V6_MAX_TOPK_ITEMS)
		return false;
	p->spec.key = palloc(list_length(q->sortClause) * sizeof(SiftV6Col));
	foreach_node(SortGroupClause, sgc, q->sortClause)
	{
		const Expr *e = (const Expr *) get_sortgroupclause_expr(sgc, q->targetList);
		SiftV6Col  *k = &p->spec.key[p->spec.nkeys];
		const SiftV6Class cls = e != NULL && IsA(e, Var) ? sift_v6_class(((const Var *) e)->vartype) : SV6C_NONE;

		if (cls == SV6C_NONE || cls == SV6C_F64 || cls == SV6C_BOOL || !sift_v6_plan_column(p, e, false, k))
			return false;
		p->spec.topk_item[p->spec.ntopk++] = (SiftV6TopKItem) {.pos = p->spec.nkeys++,.sortop = sgc->sortop,.collid = exprCollation((const Node *) e),.nulls_first = sgc->nulls_first};
	}
	return true;
}

/* the columns the statement reads, which the node takes from the heap rows of the winners: plain columns of the table */
static bool
rows_outputs(V6Plan *p)
{
	p->scan_tlist = sift_v6_scan_tlist(p->out->reltarget->exprs, p->rel->relid, T_Invalid);
	foreach_node(TargetEntry, te, p->scan_tlist)
		if (((Var *) te->expr)->varattno <= 0)
			return false;
	return p->scan_tlist != NIL;
}

/* ORDER BY columns LIMIT K of the rows of one table with every qual pushed to the scan: the rows go to a heap of the best K (v6_rows_topn.h) */
void
sift_v6_add_rows_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	const Query *q = root->parse;
	const uint32 k = sift_v6_topk_of(q);

	if (k == 0 || q->hasAggs || q->groupClause != NIL || q->groupingSets != NIL || q->hasWindowFuncs || q->hasTargetSRFs || q->hasDistinctOn || q->setOperations != NULL ||
		q->rowMarks != NIL || !sift_v6_base_rel_ok(root, input_rel) || bms_membership(root->all_baserels) != BMS_SINGLETON ||
		proj_sort_offered(input_rel))
		return;
	sift_v6_stage_target(root, UPPERREL_ORDERED, output_rel);
	foreach_ptr(IndexOptInfo, ix, input_rel->indexlist)
	{
		V6Plan		p = {.root = root,.rel = input_rel,.out = output_rel,.relid = ix->indexoid,.index = ix};
		Relation	irel;
		Cost		open,
					scan;
		bool		ok = false;

		if (!sift_v6_index_owned(ix))
			continue;
		irel = index_open(ix->indexoid, AccessShareLock);
		sift_v6_plan_index(&p, irel);
		p.spec = (SiftV6Spec) {.relid = ix->indexoid,.rows = true,.topk = k};
		ok = p.ops != NULL && sift_v6_index_complete(&p) && rows_keys(&p) && rows_outputs(&p) && sift_v6_compile_quals(&p);
		index_close(irel, NoLock);
		if (!ok)
			continue;
		sift_v6_scan_price(root, input_rel, ix, 0, &open, &scan);
		scan += cpu_operator_cost * input_rel->rows * log2(k + 1.0);
		sift_v6_add_path(output_rel, sift_v6_group_path_methods(), scan, scan + cpu_tuple_cost * k, Min((double) k, input_rel->rows), root->sort_pathkeys,
						 list_make3(sift_v6_spec_pack(&p.spec), p.scan_tlist, NIL));
		return;
	}
}
