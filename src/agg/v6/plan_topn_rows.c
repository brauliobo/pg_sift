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
#include "agg/v6/v6_cost.h"
#include "agg/v6/v6_plan_rewrite.h"
#include "agg/v6/v6_rows_topn.h"
#include "agg/v6/v6_source_rel.h"
#include "agg/v6/v6_topn.h"
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

/* a sort key the scan can compare by the sort operator of the statement: a plain float or boolean column has no decoded vector of its values (NaN, the mask of a boolean), their expressions do */
static bool
sort_key_ok(V6Plan *p, Expr *e, SiftV6Col *k)
{
	return sift_v6_compile_sort_key(p, e, k) && (k->derive > 0 || (sift_v6_class(k->typid) != SV6C_F64 && sift_v6_class(k->typid) != SV6C_BOOL));
}

/* the sort expressions of the query as the keys of the spec: columns of the index and what PostgreSQL's own expression machinery computes from them */
static bool
rows_keys(V6Plan *p)
{
	const Query *q = p->root->parse;

	if (list_length(q->sortClause) > SIFT_V6_MAX_TOPK_ITEMS)
		return false;
	p->spec.key = palloc(SIFT_V6_MAX_KEYS * sizeof(SiftV6Col));
	foreach_node(SortGroupClause, sgc, q->sortClause)
	{
		Expr	   *e = (Expr *) get_sortgroupclause_expr(sgc, q->targetList);
		SiftV6Col  *k = &p->spec.key[p->spec.nkeys];

		if (IsA(e, Const) || !sort_key_ok(p, e, k))
			return false;
		p->spec.topk_item[p->spec.ntopk++] = (SiftV6TopKItem) {.pos = p->spec.nkeys++,.sortop = sgc->sortop,.collid = exprCollation((const Node *) e),.nulls_first = sgc->nulls_first};
	}
	return true;
}

/* what PostgreSQL's expression machinery costs for the sort expressions of every row the scan keeps */
static Cost
sort_exprs_cost(PlannerInfo *root, double rows)
{
	const Query *q = root->parse;
	Cost		cost = 0;

	foreach_node(SortGroupClause, sgc, q->sortClause)
	{
		QualCost	qc;
		const Node *e = (Node *) get_sortgroupclause_expr(sgc, q->targetList);

		if (IsA(e, Var))
			continue;
		cost_qual_eval_node(&qc, (Node *) e, root);
		cost += qc.startup + qc.per_tuple * rows;
	}
	return cost;
}

void
sift_v6_rows_explain(ExplainState *es, const SiftV6Spec *spec)
{
	sift_v6_topn_explain(es, spec->topn_desc, psprintf("%u", spec->topk));
	if (spec->agg_desc != NULL)
		ExplainPropertyText("Pushed Aggregates", spec->agg_desc, es);
	if (spec->join_desc != NULL)
		ExplainPropertyText("Lookup Joins", spec->join_desc, es);
}

/* the format keeps the best rows of the relation, and what the spec asks beyond them (the aggregates of the whole set, the keys computed from columns, the dimensions of a join, a text match) when it serves that */
static bool
rows_served(const V6Plan *p)
{
	const SiftOrderOps *order = p->ops->order;

	return order != NULL && order->scan_rows != NULL &&
		(order->rows_extras || (p->spec.naggs == 0 && p->spec.nderive == 0 && p->spec.ext == NULL && !OidIsValid(p->spec.text_indexoid)));
}

bool
sift_v6_rows_compile(V6Plan *p, uint32 k)
{
	p->spec = (SiftV6Spec) {.relid = p->relid,.is_table = p->is_table,.rows = true,.topk = k,.ext = p->xseed};
	p->freg = sift_freg_create(CurrentMemoryContext);
	p->cap_aggs = p->cap_funcs = 0;
	return p->ops != NULL && sift_v6_index_complete(p) && rows_keys(p) && sift_v6_rows_outputs(p) && sift_v6_compile_quals(p) && rows_served(p);
}

void
sift_v6_rows_add(V6Plan *p, uint32 k, Cost scan, List *dims, Cost dims_startup, Cost dims_total)
{
	PlannerInfo *root = p->root;

	p->spec.topn_desc = sift_v6_topn_desc(root, p->rel->relid);
	if (dims != NIL)
		p->spec.join_desc = sift_v6_rows_dims_desc(p);
	scan += cpu_operator_cost * p->rel->rows * log2(k + 1.0) + sort_exprs_cost(root, p->rel->rows);
	sift_v6_add_path(p->out, sift_v6_group_path_methods(), scan + dims_startup, scan + cpu_tuple_cost * k + dims_total, Min((double) k, p->rel->rows), root->sort_pathkeys,
					 list_make3(sift_v6_spec_pack(&p->spec), p->scan_tlist, NIL), dims);
}

static bool
rows_table_path(V6Plan *p, Oid relid, uint32 k)
{
	Cost		open,
				scan;
	SiftV6CostIn in;

	if (!sift_v6_plan_table(p, relid) || !sift_v6_rows_compile(p, k))
		return false;
	in = (SiftV6CostIn) {.tuples = p->rel->tuples,.rows = p->rel->rows,.f_prune = sift_v6_clause_prune(p->root, p->rel->baserestrictinfo, p->ops->order != NULL ? p->ops->order->lead_att(p->relid) : 0),.groups = k,
	.ncols_read = sift_v6_spec_ncols_read(&p->spec)};
	sift_v6_scan_cost(&in, &open, &scan);
	sift_v6_rows_add(p, k, scan, NIL, 0, 0);
	return true;
}

static bool
rows_index_path(V6Plan *p, uint32 k)
{
	Relation	irel;
	Cost		open,
				scan;
	bool		ok;

	if (!sift_v6_index_owned(p->index))
		return false;
	irel = index_open(p->index->indexoid, AccessShareLock);
	sift_v6_plan_index(p, irel);
	ok = sift_v6_rows_compile(p, k);
	index_close(irel, NoLock);
	if (!ok)
		return false;
	sift_v6_scan_price(p->root, p->rel, p->index, 0, &open, &scan);
	sift_v6_rows_add(p, k, scan, NIL, 0, 0);
	return true;
}

/* the rows of the best K of the rows of one table, or of a fact table joined to its dimensions, with every qual pushed to the scan: the rows go to a heap of the best K (v6_rows_topn.h) */
void
sift_v6_add_rows_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	const Query *q = root->parse;
	const uint32 k = sift_v6_rows_k_of(q);
	RelOptInfo *rel;
	Oid			relid;

	if (k == 0 || q->hasAggs || q->groupClause != NIL || q->groupingSets != NIL || q->hasTargetSRFs || q->hasDistinctOn || q->setOperations != NULL || q->rowMarks != NIL)
		return;
	if (bms_membership(root->all_baserels) == BMS_MULTIPLE)
	{
		if (!q->hasWindowFuncs && input_rel->reloptkind == RELOPT_JOINREL)
		{
			sift_v6_stage_target(root, UPPERREL_ORDERED, output_rel);
			sift_v6_star_rows_paths(root, input_rel, output_rel, k);
		}
		return;
	}
	rel = find_base_rel(root, bms_singleton_member(root->all_baserels));
	if (proj_sort_offered(rel))
		return;
	if (sift_v6_partitioned_rel_ok(root, rel))
	{
		if (q->hasWindowFuncs)
			return;
		sift_v6_stage_target(root, UPPERREL_ORDERED, output_rel);
		sift_v6_add_rows_partition_paths(root, rel, output_rel, k);
		return;
	}
	if (!sift_v6_base_rel_ok(root, rel))
		return;
	sift_v6_stage_target(root, UPPERREL_ORDERED, output_rel);
	relid = planner_rt_fetch(rel->relid, root)->relid;
	if (sift_v6_rel_owned(relid, true))
	{
		V6Plan		p = {.root = root,.rel = rel,.out = output_rel,.relid = relid,.batch_ok = true};

		rows_table_path(&p, relid, k);
		return;
	}
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
	{
		V6Plan		p = {.root = root,.rel = rel,.out = output_rel,.relid = ix->indexoid,.index = ix,.batch_ok = true};

		if (rows_index_path(&p, k))
			return;
	}
}
