/*
 * agg/v6/v6_source_rel.c - the sources of a base relation for the operators of SiftAggV6 (v6_source_rel.h), and the plan of one of them.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"

#include "agg/plan/safety.h"
#include "agg/v6/v6_card.h"
#include "agg/v6/v6_source_rel.h"
#include "core/meta_peek.h"
#include "core/quarantine.h"
#include "core/store_ops.h"

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
	return rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && !rte->inh && rte->tablesample == NULL && sift_v6_rel_has_source(root, rel);
}

bool
sift_v6_source_usable(const SiftV6RelSource *s)
{
	return s->is_table || agg_index_complete(s->ix);
}

void
sift_v6_source_scan_price(PlannerInfo *root, RelOptInfo *rel, const SiftV6RelSource *s, AttrNumber key, Cost *open, Cost *total)
{
	if (!s->is_table)
		sift_v6_scan_price(root, rel, s->ix, key, open, total);
	else
	{
		*open = 0;
		*total = cpu_tuple_cost * 0.05 * rel->tuples;
	}
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
	if (!is_table)
		sift_quarantine_net(source, sift_meta_peek_format(source).flags);
}
