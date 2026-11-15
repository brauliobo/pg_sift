/*
 * core/quarantine_plan.c - the planner does not offer an index that is out of service (core/quarantine.h).
 *
 * The index leaves the index list of its relation, so every path, scan and plan that PostgreSQL, sift_col and BM25 build from it is gone and the statement is planned on the heap:
 * the plain plans give the answers the index would have. The hook is the first to be installed of the planner hooks of sift (_PG_init), and each of the others calls the one it
 * replaced before it works, so it runs before any of them looks at the list. PostgreSQL 19 has no get_relation_info_hook; build_simple_rel_hook is called at the same point.
 */
#include "postgres.h"

#include "access/genam.h"
#include "nodes/pg_list.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "utils/rel.h"

#include "core/quarantine.h"
#include "core/sift.h"

static void
drop_quarantined(RelOptInfo *rel)
{
	const SiftCatalog *cat = sift_catalog();
	ListCell   *lc;

	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *ii = lfirst_node(IndexOptInfo, lc);
		Relation	index;
		bool		out;

		if (ii->relam != cat->am && ii->relam != cat->col_am)
			continue;
		index = index_open(ii->indexoid, NoLock);
		out = sift_index_quarantined(index);
		index_close(index, NoLock);
		if (out)
			rel->indexlist = foreach_delete_current(rel->indexlist, lc);
	}
}

#if PG_VERSION_NUM < 190000
static get_relation_info_hook_type prev_info_hook;

static void
quarantine_get_relation_info(PlannerInfo *root, Oid relid, bool inhparent, RelOptInfo *rel)
{
	if (prev_info_hook)
		prev_info_hook(root, relid, inhparent, rel);
	drop_quarantined(rel);
}

void
sift_quarantine_plan_init(void)
{
	prev_info_hook = get_relation_info_hook;
	get_relation_info_hook = quarantine_get_relation_info;
}
#else
static build_simple_rel_hook_type prev_simple_rel_hook;

static void
quarantine_build_simple_rel(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	if (prev_simple_rel_hook)
		prev_simple_rel_hook(root, rel, rte);
	drop_quarantined(rel);
}

void
sift_quarantine_plan_init(void)
{
	prev_simple_rel_hook = build_simple_rel_hook;
	build_simple_rel_hook = quarantine_build_simple_rel;
}
#endif
