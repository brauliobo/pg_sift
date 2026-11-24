/*
 * agg/v6/v6_source_rel.c - the sources of a base relation for the operators of SiftAggV6 (v6_source_rel.h), and the plan of one of them.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "parser/parsetree.h"

#include "agg/v6/v6_source_rel.h"
#include "core/store_ops.h"

static bool
rel_is_table(PlannerInfo *root, RelOptInfo *rel)
{
	return sift_v6_rel_owned(planner_rt_fetch(rel->relid, root)->relid, true);
}

bool
sift_v6_rel_has_source(PlannerInfo *root, RelOptInfo *rel)
{
	if (rel_is_table(root, rel))
		return true;
	foreach_ptr(IndexOptInfo, ix, rel->indexlist)
		if (sift_v6_index_owned(ix))
			return true;
	return false;
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
		if (!sift_v6_index_owned(ix))
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
	sift_v6_plan_index(p, rel);
	return p->ops != NULL;
}
