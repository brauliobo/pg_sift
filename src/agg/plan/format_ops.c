/*
 * agg/plan/format_ops.c - the engines of agg/plan/format_ops.h and the dispatch of the upper path hook to them.
 */
#include "postgres.h"

#include "nodes/pg_list.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"

#include "agg/agg.h"
#include "agg/colsrc.h"
#include "agg/plan/format_ops.h"
#include "agg/v6/v6.h"
#include "col/am.h"
#include "col/scope.h"
#include "core/meta_peek.h"
#include "table/dispatch.h"

static const SiftAggFormatOps ENGINES[] = {
	{1, "v1", sift_agg_v1_upper_paths},
	{2, "v6", sift_agg_v6_upper_paths},
};

const SiftAggFormatOps *
sift_agg_format_ops(uint16 format_major)
{
	for (int i = 0; i < lengthof(ENGINES); i++)
		if (ENGINES[i].format_major == format_major)
			return &ENGINES[i];
	return NULL;
}

bool
sift_agg_format_ops_owns(const SiftAggFormatOps *ops, Oid indexoid)
{
	return sift_col_index_oid_engine(indexoid) == ops->format_major;
}

/*
 * Does a relation of the statement have a source of Format 1: a sift table or an index of that format that is not the key log of a scope (agg/colsrc.h)? The key log of a scope, which the
 * scope makes in Format 1 whatever the format of the facts, is read by the scope and by no aggregate: a statement whose relations have no other source is the engine of Format 2.0's alone, and
 * the engine of Format 1 does not open the log to find that out. An input that is not a set of plain relations, or that has partitions or children (whose indexes are theirs), is the engine's to judge.
 */
/* Does a live partition of a declarative partitioned table have a source of Format 1: the partitions that are all of format 2.0 are the engine of Format 2.0's alone */
static bool
rel_has_v1_source(PlannerInfo *root, RelOptInfo *rel, const SiftAggFormatOps *v1)
{
	int			i = -1;

	if (planner_rt_fetch(rel->relid, root)->relkind != RELKIND_PARTITIONED_TABLE || rel->part_rels == NULL)
		return true;
	while ((i = bms_next_member(rel->live_parts, i)) >= 0)
	{
		RelOptInfo *leaf = rel->part_rels[i];

		if (leaf == NULL)
			continue;
		if (planner_rt_fetch(leaf->relid, root)->inh || (sift_colsrc_is_table(planner_rt_fetch(leaf->relid, root)->relid) && !sift_tbl_is_f2_oid(planner_rt_fetch(leaf->relid, root)->relid)))
			return true;
		foreach_node(IndexOptInfo, ix, leaf->indexlist)
			if (!ix->hypothetical && sift_agg_format_ops_owns(v1, ix->indexoid) && !sift_scope_journal_managed(ix->indexoid))
				return true;
	}
	return false;
}

static bool
v1_may_plan(PlannerInfo *root, RelOptInfo *input_rel)
{
	const SiftAggFormatOps *v1 = sift_agg_format_ops(1);
	int			relid = -1;

	if (input_rel == NULL || bms_is_empty(input_rel->relids))
		return true;
	while ((relid = bms_next_member(input_rel->relids, relid)) >= 0)
	{
		RangeTblEntry *rte = planner_rt_fetch(relid, root);
		RelOptInfo *rel = root->simple_rel_array[relid];

		if (rte->rtekind == RTE_FUNCTION)
			continue;
		if (rte->rtekind != RTE_RELATION || rel == NULL)
			return true;
		if (rte->inh && !rel_has_v1_source(root, rel, v1))
			continue;
		if (rte->inh || (sift_colsrc_is_table(rte->relid) && !sift_tbl_is_f2_oid(rte->relid)))
			return true;
		foreach_node(IndexOptInfo, ix, rel->indexlist)
			if (!ix->hypothetical && sift_agg_format_ops_owns(v1, ix->indexoid) && !sift_scope_journal_managed(ix->indexoid))
				return true;
	}
	return false;
}

/* Does a relation of the statement have a source of Format 2.0: a sift_table or an index of that format? A statement that has none, of either format, is the first engine's to refuse (no_source) */
static bool
v2_has_source(PlannerInfo *root, RelOptInfo *input_rel)
{
	const SiftAggFormatOps *v2 = sift_agg_format_ops(2);
	int			relid = -1;

	if (input_rel == NULL)
		return true;
	while ((relid = bms_next_member(input_rel->relids, relid)) >= 0)
	{
		RangeTblEntry *rte = planner_rt_fetch(relid, root);
		RelOptInfo *rel = root->simple_rel_array[relid];

		if (rte->rtekind == RTE_FUNCTION)
			continue;
		if (rte->rtekind != RTE_RELATION || rel == NULL || rte->inh || sift_tbl_is_f2_oid(rte->relid))
			return true;
		foreach_node(IndexOptInfo, ix, rel->indexlist)
			if (!ix->hypothetical && sift_agg_format_ops_owns(v2, ix->indexoid))
				return true;
	}
	return false;
}

void
sift_agg_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	for (int i = 0; i < lengthof(ENGINES); i++)
		if (ENGINES[i].format_major != 1 || v1_may_plan(root, input_rel) || !v2_has_source(root, input_rel))
			ENGINES[i].upper_paths(root, stage, input_rel, output_rel, extra);
}
