/*
 * agg/join/pairs.h - the expressions of a statement over the pairs of a join (agg/pipe.h): its Vars as the columns of the pairs (SiftV6Ext.rcol, the columns of the fact table, of the
 * element of its unnest and the payload of its dimensions), each one the virtual attribute 1.. of its place there, which PostgreSQL's evaluator reads from the slot of the column.
 */
#ifndef SIFT_AGG_V6_JOIN_PAIRS_H
#define SIFT_AGG_V6_JOIN_PAIRS_H

#include "postgres.h"

#include "nodes/primnodes.h"

#include "agg/join/shape.h"
#include "agg/plan/int.h"

/* e over the columns of the pairs of p->xseed, which gets those it lacks; NULL when a Var of e is none of them, or e holds what the evaluator of the pipe cannot run (a sub-plan, a placeholder
 * that is not its expression) */
extern Node *sift_v6_pair_expr(V6Plan *p, Node *e);

/* The placeholder is its expression in every row: a strict expression of columns of the relations it is evaluated at, all NULL in a row an outer join nulls it in (PostgreSQL wraps the
 * outputs of a sub-select pulled up under grouping sets so) */
extern bool sift_v6_placeholder_is_expr(PlannerInfo *root, PlaceHolderVar *phv);
/* A placeholder other than a strict expression in a condition of the join of base: the keys and conditions of the pairs read columns */
extern bool sift_v6_join_placeholder_in_quals(PlannerInfo *root, Relids base);
/* The conditions of the join of sh the pairs run (its residual quals, the CASE of each split over the flags of its tests, the ON clause of each dimension) into p->xseed; false when one
 * reads what the pairs do not carry */
extern bool sift_v6_join_seed_resid(V6Plan *p, const SiftV6JoinShape *sh);

#endif							/* SIFT_AGG_V6_JOIN_PAIRS_H */
