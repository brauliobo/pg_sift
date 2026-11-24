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

#include "agg/cost.h"
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

/* The first leaf of the statistics of the column (0-based) of the index: a column that is an array or a document takes several leaves, so a later column is not at its own position */
extern int sift_v6_card_leaf(const IndexOptInfo *ix, int col);

/* The share of the granules of the index the zones of its sampled granules leave for the clauses the estimator describes, and in rest the clauses it does not; -1 (rest: every clause) when the statistics are not used */
extern double sift_v6_card_prune(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftCardStatsView *view, List **rest);

/* The groups of the keys (Vars of the index) over rows rows; -1 when a key has no distinct count or the relation is under row security */
extern double sift_v6_card_groups(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftCardStatsView *view, const List *group_exprs, double rows);

/* v6_cost.c: the rows of a relation from the statistics of its index (when trusted), the groups of its keys, and the cost of the aggregate scan */
extern void sift_v6_card_apply_rel(PlannerInfo *root, RelOptInfo *rel);
extern double sift_v6_card_group_estimate(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, List *group_exprs, double rows);
extern void sift_v6_agg_cost(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftV6Spec *spec, double groups, Cost *startup, Cost *total);

/* The inputs of that cost (agg/cost.h) for a scan that spreads over workers: the bytes decoded, the share of the decoded rows that pass the quals, and the open the workers do not share */
typedef struct SiftV6AggPrice
{
	SiftCostInputs in;
	double		selectivity;
	Cost		open;
} SiftV6AggPrice;

extern SiftV6AggPrice sift_v6_agg_price(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftV6Spec *spec, double groups);

/*
 * The price of reading the columns a scan of the relation needs: those of its quals, and key (a join key the scan tests on every row of the granules the zones leave; 0: none), on the rows of
 * those granules, the others on the rows that pass the quals. open is what the scan pays before its first row, total includes it; the work of the operator above is not in it.
 */
extern void sift_v6_scan_price(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, AttrNumber key, Cost *open, Cost *total);

/* What the node pays to read a dimension whole: the price of its plan, less the page reads of the share of its pages the cache holds when a sequential scan takes the table */
extern Cost sift_v6_dim_cost(const RelOptInfo *rel);

/* The share of the granules the clauses leave of a relation that has no statistics of its own: a column that follows the order of the rows keeps the share of the rows, any other (Yao) nearly all */
extern double sift_v6_clause_prune(PlannerInfo *root, const List *clauses);

#endif							/* SIFT_AGG_V6_CARD_H */
