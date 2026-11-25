/*
 * agg/dim_keys.h - a dimension of a join of SiftAggV6 read through its own index by the keys the fact table lets through. When the fact table's filter is a list of constants on the
 * column a dimension is joined by (fact.col IN (...)), no fact row outside the list reaches the join, so no row of the dimension outside it can match one: the plan of the dimension is
 * PostgreSQL's own plan of the relation with that list as one more condition on the key, which an index on the key answers by one probe a key instead of a read of the whole table. It is
 * offered for the joins that keep only the dimension rows a fact row matches (inner, left, semi and anti) and costed by PostgreSQL's own estimates of the index, and the cheaper of the two
 * plans is the one the join reads.
 */
#ifndef SIFT_AGG_V6_DIM_KEYS_H
#define SIFT_AGG_V6_DIM_KEYS_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/star/star.h"

/*
 * The plan the join reads dimension dim with: its cheapest plan, or the cheaper plan that probes it by the constant keys of the fact table's filter. probe_attno[k] is the fact column
 * (0: none) the column key[k] of the dimension is joined to. dim is a relation of the statement, not a join of relations.
 */
extern Path *sift_v6_dim_path(PlannerInfo *root, const RelOptInfo *fact, RelOptInfo *dim, SiftV6JoinMode mode, int nkeys, const AttrNumber *probe_attno, const Var *const *key);

#endif							/* SIFT_AGG_V6_DIM_KEYS_H */
