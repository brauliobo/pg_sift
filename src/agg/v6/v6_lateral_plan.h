/*
 * agg/v6/v6_lateral_plan.h - a LATERAL sub-select of one table that PostgreSQL runs once for each row of the relations before it, as a dimension the node reads whole.
 *
 *   LEFT JOIN LATERAL (SELECT cols FROM dim d WHERE d.k = o.c [AND tests of dim alone] [ORDER BY ..] LIMIT 1) s ON true
 *
 * returns, for a row of o, the first row of dim with k = o.c, as the left join of o to `SELECT k, cols FROM dim` (one row for each k) on k = o.c does. PostgreSQL has no plan of the sub-select that
 * takes no parameter, so the dimension is planned here as a query of its own: its keys are the columns the equalities name, the rows of a key that dim repeats are cut to the first one (DISTINCT ON,
 * in the order of the sub-select, then of the table), and the plan stands for the relation of the sub-select in the plan of the node.
 */
#ifndef SIFT_AGG_V6_LATERAL_PLAN_H
#define SIFT_AGG_V6_LATERAL_PLAN_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/v6/v6_star.h"

typedef struct SiftV6LateralDim
{
	RelOptInfo *rel;			/* the relation of the sub-select as the node reads it: the columns the statement needs, then one for each key */
	int			nkeys;
	const Node *probe[SIFT_STAR_MAX_KEYS];	/* the expression of the relations before the sub-select that each key is probed with */
	const Var  *key[SIFT_STAR_MAX_KEYS];	/* the column of rel that holds the key */
} SiftV6LateralDim;

/* The dimension of the lateral relation of the statement; false when its sub-select is not of that shape */
extern bool sift_v6_lateral_dim(PlannerInfo *root, RelOptInfo *lateral, SiftV6LateralDim *out);

/* The plan of the node is made: the relations of its dimensions stand for the sub-selects they replace (their plans are read by set_plan_references) */
extern void sift_v6_lateral_adopt(PlannerInfo *root, const List *dim_paths);

#endif							/* SIFT_AGG_V6_LATERAL_PLAN_H */
