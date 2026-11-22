/*
 * agg/v6/v6_card.h - the cardinality of a scan of an index of format major 2 from the statistics its parts folded (agg/plan/card_est.h), for the planner: the WHERE clauses of the
 * relation become the predicates of the estimator (integer, date and timestamp columns compared with constants or with an array of constants, IS [NOT] NULL), the others keep
 * PostgreSQL's own selectivity. The estimate replaces PostgreSQL's only when it is trusted (SIFT_V6_CARD_TRUST), and never for a relation that row security or a security barrier applies
 * to: its counts are those of every tenant, and the rows of a plan are visible in EXPLAIN. A parameter of a generic plan is no predicate: its value is not known, so it proves nothing.
 */
#ifndef SIFT_AGG_V6_CARD_H
#define SIFT_AGG_V6_CARD_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/plan/card_est.h"
#include "agg/v6/v6.h"

#define SIFT_V6_CARD_TRUST	0.5

/* The clauses of a relation as the estimator's query: preds and vals are its storage; rest holds the clauses no predicate describes */
typedef struct SiftV6CardQuery
{
	SiftCardPred preds[SIFT_CARD_MAX_PREDS];
	uint64	   *vals;
	uint32		nvals;
	SiftCardQuery q;
	List	   *rest;
} SiftV6CardQuery;

extern void sift_v6_card_query(const List *clauses, Index relid, const IndexOptInfo *ix, SiftV6CardQuery *out);

/* rel->rows from the statistics; false (rel untouched) when the estimate is not trusted or the relation is under row security */
extern bool sift_v6_card_rel(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftCardStatsView *view);

/* The groups of the keys (Vars of the index) over rows rows; -1 when a key has no distinct count or the relation is under row security */
extern double sift_v6_card_groups(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftCardStatsView *view, const List *group_exprs, double rows);

/* v6_cost.c: the rows of a relation from the statistics of its index (when trusted), the groups of its keys, and the cost of the aggregate scan */
extern void sift_v6_card_apply_rel(PlannerInfo *root, RelOptInfo *rel);
extern double sift_v6_card_group_estimate(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, List *group_exprs, double rows);
extern void sift_v6_agg_cost(const RelOptInfo *rel, const IndexOptInfo *ix, const SiftV6Spec *spec, double groups, Cost *startup, Cost *total);

#endif							/* SIFT_AGG_V6_CARD_H */
