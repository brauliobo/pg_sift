/*
 * agg/plan/topn_rows.c - the path of SiftAggV6 for ORDER BY columns LIMIT K of the rows of one table: the rows of the winners come from the scan of the index.
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
#include "utils/selfuncs.h"
#include "parser/parsetree.h"

#include "agg/plan/int.h"
#include "agg/engine.h"
#include "agg/cost/card.h"
#include "agg/cost/cost.h"
#include "agg/join/plan.h"
#include "agg/plan/rewrite.h"
#include "agg/par/pg.h"
#include "agg/topn/rows.h"
#include "agg/source_rel.h"
#include "agg/topn/topn.h"
#include "agg/settings.h"
#include "agg/vexpr.h"
#include "core/store/ops.h"

/* a ready projection of a format serves the row query of the relation: its own path stands among those of the relation */
static bool
proj_sort_offered(const RelOptInfo *rel)
{
	for (int i = 0; i < sift_store_ops_count(); i++)
		if (sift_store_ops_at(i)->proj != NULL && sift_store_ops_at(i)->proj->sort_offered(rel))
			return true;
	return false;
}

/* the sort expressions of the query as the keys of the spec: columns of the index and what PostgreSQL's own expression machinery computes from them. The keys past the first that the scan
 * cannot read (a column of the heap the index does not hold) are sorted above the node, which keeps every row that ties its K-th on the keys before them */
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

		if (IsA(e, Const) || !sift_v6_compile_sort_key(p, e, k))
		{
			p->spec.topk_ties = p->spec.ntopk > 0;
			return p->spec.topk_ties;
		}
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
	const char *limit = spec->topk_count != NIL ? sift_v6_topn_limit_text(linitial(spec->topk_count), lsecond(spec->topk_count)) :
		spec->topk == SIFT_V6_ROWS_ALL ? "all" : psprintf("%u", spec->topk);

	sift_v6_topn_explain(es, spec->topn_desc, spec->topk_ties ? psprintf("%s with ties", limit) : limit);
	if (spec->agg_desc != NULL)
		ExplainPropertyText("Pushed Aggregates", spec->agg_desc, es);
}

/* the format keeps the best rows of the relation, and what the spec asks beyond them (the aggregates of the whole set, the keys computed from columns, the dimensions of a join, a text match) when it serves that;
 * a key computed from columns, and a column the node gives from its scan, is the value of the column in the heap row, which only a store that reads its values exactly gives */
static bool
rows_served(const V6Plan *p)
{
	const SiftOrderOps *order = p->ops->order;
	const SiftV6Rel rel = sift_v6_plan_rel(p);
	const uint8 asks = (p->spec.naggs > 0 ? SIFT_ROWS_TOTALS : 0) | (p->spec.nderive > 0 ? SIFT_ROWS_DERIVED : 0) | (p->spec.ext != NULL ? SIFT_ROWS_STAR : 0) |
		(OidIsValid(p->spec.text_indexoid) ? SIFT_ROWS_TEXT : 0);

	return order != NULL && order->scan_rows != NULL && (order->rows_serves & asks) == asks && ((p->spec.nderive == 0 && !p->rows_from_store) || sift_v6_values_exact(&rel));
}

bool
sift_v6_rows_compile(V6Plan *p, uint32 k)
{
	p->spec = (SiftV6Spec) {.relid = p->relid,.is_table = p->is_table,.rows = true,.topk = k,.topk_count = sift_v6_rows_count_of(p->root->parse),.ext = p->xseed};
	p->freg = sift_freg_create(CurrentMemoryContext);
	p->cap_aggs = p->cap_funcs = 0;
	return p->ops != NULL && sift_v6_index_complete(p) && rows_keys(p) && sift_v6_rows_outputs(p) && sift_v6_compile_quals(p) && rows_served(p);
}

/* the rows of the node in the order of the statement: a node that keeps the rows tying on the keys it reads is sorted on the others above it */
static Path *
rows_sorted(PlannerInfo *root, RelOptInfo *out, Path *path)
{
	int			presorted;

	if (pathkeys_count_contained_in(root->sort_pathkeys, path->pathkeys, &presorted))
		return path;
	if (presorted == 0)
		return (Path *) create_sort_path(root, out, path, root->sort_pathkeys, root->limit_tuples);
	return (Path *) create_incremental_sort_path(root, out, path, root->sort_pathkeys, presorted, root->limit_tuples);
}

/* the rows the node keeps: the best K, and with ties every row of the group of the keys it reads that holds the K-th */
static double
rows_kept(const V6Plan *p, uint32 k)
{
	List	   *exprs = NIL;

	if (!p->spec.topk_ties)
		return Min((double) k, p->rel->rows);
	foreach_node(SortGroupClause, sgc, list_copy_head(p->root->parse->sortClause, p->spec.ntopk))
		exprs = lappend(exprs, get_sortgroupclause_expr(sgc, p->root->parse->targetList));
	return Min(p->rel->rows, k + p->rel->rows / estimate_num_groups(p->root, exprs, p->rel->rows, NULL, NULL));
}

/* the path of the best K rows the scan of cost scan keeps, the heap of the winners priced at the rows it holds */
static Path *
rows_path(V6Plan *p, uint32 k, Cost scan, List *dims, Cost dims_startup, Cost dims_total)
{
	PlannerInfo *root = p->root;
	const double kept = rows_kept(p, k);
	List	   *order = p->spec.topk_ties ? make_pathkeys_for_sortclauses(root, list_copy_head(root->parse->sortClause, p->spec.ntopk), root->parse->targetList) : root->sort_pathkeys;
	Path		price;

	p->spec.topn_desc = sift_v6_topn_desc(root, p->spec.ntopk);
	p->spec.serial_because = sift_v6_par_rows_serial_why(root, p->rel, dims != NIL);
	scan += cpu_operator_cost * p->rel->rows * log2(kept + 1.0) + sort_exprs_cost(root, p->rel->rows);
	price = (Path) {.startup_cost = scan + dims_startup,.total_cost = scan + cpu_tuple_cost * kept + dims_total};
	sift_v6_force_price(&price);
	return rows_sorted(root, p->out, sift_v6_make_path(p->out, sift_v6_group_path_methods(), price.startup_cost, price.total_cost, kept, order,
														list_make3(sift_v6_spec_pack(&p->spec), p->scan_tlist, NIL), dims));
}

void
sift_v6_rows_add(V6Plan *p, uint32 k, Cost scan, List *dims, Cost dims_startup, Cost dims_total)
{
	add_path(p->out, rows_path(p, k, scan, dims, dims_startup, dims_total));
}

/* the plan of the rows over the sift_table relid and the cost of its scan */
static bool
rows_table_scan(V6Plan *p, Oid relid, uint32 k, Cost *scan)
{
	Cost		open;
	SiftV6CostIn in;

	if (!sift_v6_plan_table(p, relid) || !sift_v6_rows_compile(p, k))
		return false;
	in = (SiftV6CostIn) {.tuples = p->rel->tuples,.rows = p->rel->rows,.f_prune = sift_v6_clause_prune(p->root, p->rel->baserestrictinfo, p->ops->order != NULL ? p->ops->order->lead_att(p->relid, true) : 0),.groups = k,
	.row_frac = sift_v6_rel_row_frac(p->relid),.ncols_read = sift_v6_spec_ncols_read(&p->spec)};
	sift_v6_scan_cost(&in, &open, scan);
	return true;
}

/* the plan of the rows over the index p->index and the cost of its scan */
static bool
rows_index_scan(V6Plan *p, uint32 k, Cost *scan)
{
	Relation	irel;
	Cost		open;
	bool		ok;

	if (!sift_v6_index_owned(p->index))
		return false;
	irel = index_open(p->index->indexoid, AccessShareLock);
	sift_v6_plan_index(p, irel);
	ok = sift_v6_rows_compile(p, k);
	index_close(irel, NoLock);
	if (ok)
		sift_v6_scan_price(p->root, p->rel, p->index, 0, &open, scan);
	return ok;
}

/* the path of the best K rows of the one relation of the statement: over its own store when it is a sift_table, else over the first of its indexes that serves it; NULL for none */
static Path *
rows_rel_path(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, uint32 k, bool from_store)
{
	const Oid	relid = planner_rt_fetch(rel->relid, root)->relid;
	Cost		scan;

	if (sift_v6_rel_owned(relid, true))
	{
		V6Plan		p = {.root = root,.rel = rel,.out = out,.relid = relid,.batch_ok = true,.rows_from_store = from_store};

		return rows_table_scan(&p, relid, k, &scan) ? rows_path(&p, k, scan, NIL, 0, 0) : NULL;
	}
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
	{
		V6Plan		p = {.root = root,.rel = rel,.out = out,.relid = ix->indexoid,.index = ix,.batch_ok = true,.rows_from_store = from_store};

		if (rows_index_scan(&p, k, &scan))
			return rows_path(&p, k, scan, NIL, 0, 0);
	}
	return NULL;
}

Path *
sift_v6_rows_store_path(PlannerInfo *root, RelOptInfo *rel)
{
	const Query *q = root->parse;

	if (rel->reloptkind != RELOPT_BASEREL || q->sortClause == NIL || q->hasTargetSRFs || q->rowMarks != NIL || !sift_v6_base_rel_ok(root, rel))
		return NULL;
	return rows_rel_path(root, rel, rel, SIFT_V6_ROWS_ALL, true);
}

/* the rows of the best K of the rows of one table, or of a fact table joined to its dimensions, with every qual pushed to the scan: the rows go to a heap of the best K (v6_rows_topn.h) */
void
sift_v6_add_rows_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	const Query *q = root->parse;
	const uint32 k = sift_v6_rows_k_of(q);
	RelOptInfo *rel;
	Path	   *path;

	if (q->sortClause != NIL && q->limitCount != NULL && (q->distinctClause != NIL || q->setOperations != NULL))
	{
		sift_agg_arm_refused_by("topn", AGG_REFUSE_TOPN_SHAPE, "the query has DISTINCT or a set operation");
		return;
	}
	if (k == 0 || q->hasAggs || q->groupClause != NIL || q->groupingSets != NIL || q->hasTargetSRFs || q->hasDistinctOn || q->setOperations != NULL || q->rowMarks != NIL)
		return;
	if (bms_membership(root->all_baserels) == BMS_MULTIPLE)
	{
		if (!q->hasWindowFuncs && input_rel->reloptkind == RELOPT_JOINREL)
		{
			sift_v6_stage_target(root, UPPERREL_ORDERED, output_rel);
			sift_v6_star_rows_paths(root, input_rel, output_rel, k);
			sift_v6_join_rows_paths(root, input_rel, output_rel, k);
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
	if ((path = rows_rel_path(root, rel, output_rel, k, false)) != NULL)
		add_path(output_rel, path);
}
