/*
 * agg/source_rel.c - the sources of a base relation for the operators of SiftAggV6 (v6_source_rel.h), and the plan of one of them.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "storage/predicate.h"

#include "agg/safety.h"
#include "agg/cost/card.h"
#include "agg/cost/cost.h"
#include "agg/source_rel.h"
#include "agg/vexpr.h"
#include "col/am.h"
#include "core/meta_peek.h"
#include "core/quarantine/quarantine.h"
#include "core/store/ops.h"

static bool
rel_is_table(PlannerInfo *root, RelOptInfo *rel)
{
	return sift_v6_rel_owned(planner_rt_fetch(rel->relid, root)->relid, true);
}

static bool
index_is_source(const IndexOptInfo *ix)
{
	return sift_v6_index_owned(ix);
}

bool
sift_v6_rel_has_source(PlannerInfo *root, RelOptInfo *rel)
{
	if (rel_is_table(root, rel))
		return true;
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
		if (index_is_source(ix))
			return true;
	return false;
}

bool
sift_v6_base_rel_ok(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte;

	if (rel->reloptkind != RELOPT_BASEREL || rel->lateral_relids != NULL || agg_query_has_rls(root))
		return false;
	rte = planner_rt_fetch(rel->relid, root);
	return sift_v6_rte_stored(rte) && !rte->inh && rte->tablesample == NULL && sift_v6_rel_has_source(root, rel);
}

bool
sift_v6_source_usable(const SiftV6RelSource *s)
{
	return s->is_table || agg_index_complete(s->ix);
}

/* the columns of the quals of the relation and its key */
static Bitmapset *
qual_cols(RelOptInfo *rel, AttrNumber key)
{
	Bitmapset  *cols = key > 0 ? bms_make_singleton(key - FirstLowInvalidHeapAttributeNumber) : NULL;

	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		pull_varattnos((Node *) ri->clause, rel->relid, &cols);
	return cols;
}

/* a sift_table decodes the columns cols over the granules the quals leave, as its aggregate scan does */
static void
table_scan_price(PlannerInfo *root, RelOptInfo *rel, const Bitmapset *cols, Cost *open, Cost *total)
{
	SiftV6CostIn in = {.tuples = rel->tuples,.rows = rel->rows,.pages = rel->pages,.f_prune = sift_v6_clause_prune(root, rel->baserestrictinfo, 0),.groups = 1,
	.row_frac = sift_v6_rel_row_frac(planner_rt_fetch(rel->relid, root)->relid)};
	Cost		startup;

	in.ncols_read = bms_num_members(cols);
	*open = SIFT_OPEN_OPS * cpu_operator_cost;
	sift_v6_scan_cost(&in, &startup, total);
}

void
sift_v6_source_scan_price(PlannerInfo *root, RelOptInfo *rel, const SiftV6RelSource *s, AttrNumber key, Cost *open, Cost *total)
{
	Bitmapset  *cols = qual_cols(rel, key);

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &cols);
	if (s->is_table)
		table_scan_price(root, rel, cols, open, total);
	else
		sift_v6_scan_price(root, rel, s->ix, key, open, total);
}

void
sift_v6_source_filter_price(PlannerInfo *root, RelOptInfo *rel, const SiftV6RelSource *s, AttrNumber key, Cost *open, Cost *total)
{
	if (s->is_table)
		table_scan_price(root, rel, qual_cols(rel, key), open, total);
	else
		sift_v6_scan_price(root, rel, s->ix, key, open, total);
}

void
sift_v6_table_spec_cost(PlannerInfo *root, RelOptInfo *rel, Oid relid, const SiftV6Spec *spec, double groups, Cost *startup, Cost *total)
{
	const SiftStoreOps *ops = sift_store_ops_for_oid(relid, true);
	const AttrNumber lead = ops->order != NULL ? ops->order->lead_att(relid, true) : 0;
	const SiftV6CostIn in = {.tuples = rel->tuples,.pages = rel->pages,.rows = rel->rows,.f_prune = sift_v6_clause_prune(root, rel->baserestrictinfo, lead),.groups = groups,
	.row_frac = sift_v6_rel_row_frac(relid),.ncols_read = sift_v6_spec_ncols_read(spec)};

	sift_v6_scan_cost(&in, startup, total);
}

int
sift_v6_rel_sources(PlannerInfo *root, RelOptInfo *rel, SiftV6RelSource *out, int max_out)
{
	const Oid	heapoid = planner_rt_fetch(rel->relid, root)->relid;
	int			n = 0;

	if (rel_is_table(root, rel))
	{
		Relation	heap = table_open(heapoid, AccessShareLock);

		out[n] = (SiftV6RelSource) {.reloid = heapoid,.heapoid = heapoid,.is_table = true,.scan_relid = rel->relid};
		sift_v6_input_init_table(&out[n].input, heap);
		table_close(heap, NoLock);
		n++;
	}
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
	{
		if (n == max_out)
			break;
		if (!index_is_source(ix))
			continue;
		out[n] = (SiftV6RelSource) {.reloid = ix->indexoid,.heapoid = heapoid,.scan_relid = rel->relid,.ix = ix};
		sift_v6_input_init_info(&out[n].input, ix);
		n++;
	}
	return n;
}

SiftV6RelSource *
sift_v6_rel_sources_alloc(PlannerInfo *root, RelOptInfo *rel, int *n)
{
	SiftV6RelSource *out = palloc(SIFT_V6_REL_SOURCE_CAP(rel) * sizeof(SiftV6RelSource));

	*n = sift_v6_rel_sources(root, rel, out, SIFT_V6_REL_SOURCE_CAP(rel));
	return out;
}

Relation
sift_v6_source_open(Oid reloid, bool is_table, LOCKMODE lockmode, SiftV6Input *in_out)
{
	Relation	rel = is_table ? table_open(reloid, lockmode) : index_open(reloid, lockmode);

	if (in_out != NULL && is_table)
		sift_v6_input_init_table(in_out, rel);
	else if (in_out != NULL)
		sift_v6_input_init_rel(in_out, rel);
	return rel;
}

void
sift_v6_source_close(Relation rel, bool is_table, LOCKMODE lockmode)
{
	if (is_table)
		table_close(rel, lockmode);
	else
		index_close(rel, lockmode);
}

bool
sift_v6_plan_bind(V6Plan *p, const SiftV6RelSource *s, Relation rel)
{
	p->relid = s->reloid;
	p->index = s->ix;
	if (s->is_table)
		return sift_v6_plan_table(p, s->reloid);
	if (!sift_v6_index_owned(s->ix))
		return false;
	sift_v6_plan_index(p, rel);
	return p->ops != NULL;
}

void
sift_v6_source_enter(Relation source, bool is_table, Snapshot snap)
{
	Relation	heap = source;

	if (is_table)
	{
		pgstat_count_heap_scan(source);
		SIFT_COL_OPEN_GATE();
	}
	else
	{
		sift_quarantine_net(source, sift_meta_peek_format(source).flags);
		pgstat_count_index_scan(source);
	}
	if (!IsolationIsSerializable())
		return;
	if (!is_table)
		heap = table_open(source->rd_index->indrelid, NoLock);
	PredicateLockRelation(heap, snap);
	if (!is_table)
		table_close(heap, NoLock);
}
