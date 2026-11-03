/*
 * agg/cte/keyfirst.c - a hoisted key list is joined before the tables it filters
 * hoist_sublinks (rewrite.c) turns the IN sub-select of a CTE body into a materialized CTE, read as the right side of a semi join. The list
 * is computed whatever the join order, and a table joined after it is probed for the keys of the list alone, where a table probed for every row of
 * the other side reads the index once per row (1,000 probes of an index against 10). PostgreSQL prices both orders from a one row estimate of the
 * list and keeps the first it finds when they tie. The order that applies the list first can only probe fewer keys, so the join paths that read a table
 * for the rows of the side the list could filter already are disabled like those of a setting that is off: they lose to any path that is not, and are the
 * plan when no legal order joins the list first.
 */
#include "postgres.h"

#include "nodes/pathnodes.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"

#include "agg/cte/int.h"

#define HOISTED_PREFIX "sift_hoisted_"

static set_join_pathlist_hook_type prev_join_hook;

/* the relid of the hoisted CTE a semi join reads as its right side, else 0 */
static int
hoisted_right(PlannerInfo *root, SpecialJoinInfo *sj)
{
	RangeTblEntry *rte;
	int			relid;

	if (sj->jointype != JOIN_SEMI || bms_membership(sj->min_righthand) != BMS_SINGLETON)
		return 0;
	relid = bms_singleton_member(sj->min_righthand);
	rte = planner_rt_fetch(relid, root);
	return rte->rtekind == RTE_CTE && strncmp(rte->ctename, HOISTED_PREFIX, strlen(HOISTED_PREFIX)) == 0 ? relid : 0;
}

/* the rels whose rows the list filters: those of the semi join's left side, and every rel that shares a key with the list (a join of any two of them applies the list's clause to either) */
static Relids
filtered_rels(PlannerInfo *root, SpecialJoinInfo *sj, int list)
{
	Relids		rels = bms_copy(sj->min_lefthand);

	foreach_node(EquivalenceClass, ec, root->eq_classes)
		foreach_node(EquivalenceMember, em, ec->ec_members)
			if (bms_is_member(list, em->em_relids))
			{
				rels = bms_add_members(rels, ec->ec_relids);
				break;
			}
	return bms_del_member(rels, list);
}

/* does the set hold a table (a relation, not a CTE or sub-select) that the list filters? */
static bool
holds_table(PlannerInfo *root, Relids rels, Relids filtered)
{
	int			x = -1;

	while ((x = bms_next_member(rels, x)) >= 0)
		if (bms_is_member(x, filtered) && planner_rt_fetch(x, root)->rtekind == RTE_RELATION)
			return true;
	return false;
}

/*
 * A path of the join of two sides reads a table for rows the list could have filtered: neither side holds the list and the two hold, between them, a table
 * and another rel it filters (the table is probed for the rows of the other); or one side holds the list and a table, and the other, without the list, is
 * what the nested loop probes it for.
 */
static bool
reads_before_list(PlannerInfo *root, JoinPath *jp, Relids filtered, int list)
{
	Path	   *sides[2] = {jp->outerjoinpath, jp->innerjoinpath};
	Relids		both = bms_union(sides[0]->parent->relids, sides[1]->parent->relids);
	bool		dominated = false;

	if (!bms_is_member(list, both))
		dominated = bms_num_members(bms_intersect(both, filtered)) > 1 && holds_table(root, both, filtered);
	else
		for (int i = 0; i < 2 && !dominated; i++)
		{
			Path	   *with = sides[i],
					   *without = sides[1 - i];

			dominated = bms_is_member(list, with->parent->relids) && !bms_is_member(list, without->parent->relids) &&
				bms_overlap(without->parent->relids, filtered) && with->param_info != NULL &&
				bms_overlap(with->param_info->ppi_req_outer, without->parent->relids) &&
				!bms_is_member(list, with->param_info->ppi_req_outer) && holds_table(root, with->parent->relids, filtered);
		}
	bms_free(both);
	return dominated;
}

/* once, however many times the hook is called for the joinrel: a join path counts the disabled nodes of its two sides */
static void
disable_join_paths(PlannerInfo *root, List *paths)
{
	foreach_ptr(Path, p, paths)
	{
		JoinPath   *jp;

		if (!IsA(p, NestPath) && !IsA(p, MergePath) && !IsA(p, HashPath))
			continue;
		jp = (JoinPath *) p;
		foreach_node(SpecialJoinInfo, sj, root->join_info_list)
		{
			int			list = hoisted_right(root, sj);
			Relids		filtered;
			bool		dominated;

			if (list == 0)
				continue;
			filtered = filtered_rels(root, sj, list);
			dominated = reads_before_list(root, jp, filtered, list);
			bms_free(filtered);
			if (dominated)
			{
				sift_path_disable(p, sift_disabled_nodes(jp->outerjoinpath) + sift_disabled_nodes(jp->innerjoinpath) + 1);
				break;
			}
		}
	}
}

static void
key_first_join_paths(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra)
{
	if (prev_join_hook)
		prev_join_hook(root, joinrel, outerrel, innerrel, jointype, extra);
	disable_join_paths(root, joinrel->pathlist);
	disable_join_paths(root, joinrel->partial_pathlist);
}

void
sift_key_first_init(void)
{
	prev_join_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = key_first_join_paths;
}
