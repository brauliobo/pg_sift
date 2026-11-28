/*
 * agg/lateral_plan.h - a LATERAL sub-select of one table that PostgreSQL runs once for each row of the relations before it, as a dimension the node reads whole.
 *
 *   LEFT JOIN LATERAL (SELECT cols FROM dim d WHERE d.k = o.c [AND tests of dim alone] [ORDER BY ..] LIMIT 1) s ON true
 *
 * returns, for a row of o, the first row of dim with k = o.c, as the left join of o to `SELECT k, cols FROM dim` (one row for each k) on k = o.c does. A condition of the sub-select over the
 * relations before it alone (a gate) gives the row that fails it no row of dim: its probe is then NULL, which matches none. A sub-select that only its gates tie to the outer rows has the
 * one key 1: every row that passes them finds the first row of dim. PostgreSQL has no plan of the sub-select that
 * takes no parameter, so the dimension is planned here as a query of its own: its keys are the columns the equalities name, the rows of a key that dim repeats are cut to the first one (DISTINCT ON,
 * in the order of the sub-select, then of the table), and the plan stands for the relation of the sub-select in the plan of the node.
 *
 * The sub-select may sit in the query the node plans, or in the sub-select of an EXISTS over the elements of an array (agg/exists_plan.h): its equalities then name the columns of the
 * element (one query level up) and of the relations of the statement (two up).
 */
#ifndef SIFT_AGG_V6_LATERAL_PLAN_H
#define SIFT_AGG_V6_LATERAL_PLAN_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/join/shape.h"
#include "agg/star/star.h"

typedef struct SiftV6LateralDim
{
	RelOptInfo *rel;			/* the relation of the sub-select as the node reads it: the columns the statement needs, then one for each key */
	int			nkeys;
	const Node *probe[SIFT_STAR_MAX_KEYS];	/* the expression of the relations before the sub-select that each key is probed with, in the terms of the planner of the node */
	const Var  *key[SIFT_STAR_MAX_KEYS];	/* the column of rel that holds the key */
	Oid			coll[SIFT_STAR_MAX_KEYS];	/* the collation the equality compares the key by */
	List	   *gates;			/* the conditions of the sub-select over the relations before it alone (NOT jsonb_exists(f.doc, 'k')), in the terms of the planner of the node */
} SiftV6LateralDim;

/*
 * The dimension of the lateral relation of home, as relation relid of root. home is root, or the planner of an EXISTS sub-select of root, whose relations map (indexed by their relid in home)
 * names in root. false when the sub-select is not of the shape above.
 */
extern bool sift_v6_lateral_dim(PlannerInfo *root, PlannerInfo *home, const Index *map, RelOptInfo *lateral, Index relid, SiftV6LateralDim *out);

/* The same as dimension at in sh: the keys probe the columns of the relations before it (a column of the fact table, of the element or of an earlier dimension), or an expression of them */
extern bool sift_v6_lateral_join_dim(PlannerInfo *root, PlannerInfo *home, const Index *map, RelOptInfo *lateral, Index relid, SiftV6JoinShape *sh, int at);

/* The plan of the node is made: the relations of its dimensions stand for the sub-selects they replace (their plans are read by set_plan_references) */
extern void sift_v6_lateral_adopt(PlannerInfo *root, const List *dim_paths);

#endif							/* SIFT_AGG_V6_LATERAL_PLAN_H */
