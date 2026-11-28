/*
 * agg/join/plan.h - the joins of a fact table to other relations that the planner of the star join (v6_star_plan.c) and the one of the unnest (v6_unnest_plan.c) leave to PostgreSQL:
 * the relations the fact table's rows are looked up in, that may hold a key more than once or be the result of any plan (a table, a VALUES list, a CTE, a sub-select, a tree of joins), joined by
 * an inner, left, right, full, semi or anti join on equalities whose probing side is a column of the fact table, the element of an unnest or a column of an earlier relation, and the
 * conditions the keys do not say (an inequality, a WHERE over the columns of a left join), which run on the pairs (agg/join/lookup.h).
 *
 * It offers a path only for a statement that needs one of those: the join that v6_star_plan.c takes (the dimensions all inner or left joins on the fact table's columns, no condition but the keys)
 * and the unnest alone are left to their planners, so that the hot paths of the node (one lookup a row, the filter of the keys in the scan) are not touched.
 */
#ifndef SIFT_AGG_V6_JOIN_PLAN_H
#define SIFT_AGG_V6_JOIN_PLAN_H

#include "postgres.h"

#include "nodes/pathnodes.h"

/* charge: what each fact relation's probes are charged for the lookups PostgreSQL guesses (sift_v6_lookup_paths, sift_v6_charge_probes), by relid */
extern void sift_v6_join_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs, double *charge);
/* the same joins for the best k rows of the join in the order of its ORDER BY (agg/plan/topn_rows.c) */
extern void sift_v6_join_rows_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, uint32 k);

/*
 * The paths of the star and of the lookup joins (below) of an aggregate or a DISTINCT over a join; the plans of PostgreSQL that probe a fact relation once a key of a lookup it
 * only guesses are charged for the keys it passes, once whichever planner found the lookup (sift_v6_lookups_guess)
 */
extern void sift_v6_lookup_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs);

#endif							/* SIFT_AGG_V6_JOIN_PLAN_H */
