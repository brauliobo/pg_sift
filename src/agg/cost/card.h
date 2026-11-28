/*
 * agg/cost/card.h - the cardinality of a scan of an index of format major 2 from the statistics its parts folded (agg/cost/card_est.h), for the planner: the WHERE clauses of the
 * relation become the predicates of the estimator (integer, date and timestamp columns compared with constants or with an array of constants, IS [NOT] NULL), the others keep
 * PostgreSQL's own selectivity. The estimate replaces PostgreSQL's only when it is trusted (SIFT_V6_CARD_TRUST), and never for a relation that row security or a security barrier applies
 * to: its counts are those of every tenant, and the rows of a plan are visible in EXPLAIN. A parameter of a generic plan is no predicate: its value is not known, so it proves nothing.
 */
#ifndef SIFT_AGG_V6_CARD_H
#define SIFT_AGG_V6_CARD_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/cost/model.h"
#include "agg/cost/card_est.h"
#include "agg/engine.h"

#define SIFT_V6_CARD_TRUST	0.5

/* The clauses of a relation as the estimator's query: preds and vals are its storage; rest holds the clauses no predicate describes */
typedef struct SiftV6CardQuery
{
	SiftCardPred preds[SIFT_CARD_MAX_PREDS];
	uint64	   *vals;
	uint32		nvals;
	uint32		cap;			/* the values vals has room for */
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
/*
 * Of each leaf of the index, the share of the rows of the granules the quals keep whose column the scan still decodes to test them: a granule whose zone decides every
 * predicate on the leaf is tested without it (sampled granules; a leaf a clause the statistics cannot describe reads keeps 1). NULL without statistics: every row.
 */
extern double *sift_v6_card_undecided(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftCardStatsView *view);
extern double sift_v6_card_groups(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftCardStatsView *view, const List *group_exprs, double rows);

/* v6_cost.c: the rows of a relation from the statistics of its index (when trusted), the groups of its keys, and the cost of the aggregate scan */
extern void sift_v6_card_apply_rel(PlannerInfo *root, RelOptInfo *rel);
/* range_join.c: the selectivity of each join clause `col <@ range` of rel against a list of constant ranges, from the statistics of the column */
extern void sift_v6_range_join_rows(PlannerInfo *root, RelOptInfo *rel);
extern double sift_v6_card_group_estimate(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, List *group_exprs, double rows);
extern void sift_v6_agg_cost(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftV6Spec *spec, double groups, Cost *startup, Cost *total);

/* The inputs of that cost (agg/cost/model.h) for a scan that spreads over workers: the bytes decoded, the share of the decoded rows that pass the quals, and the open the workers do not share */
typedef struct SiftV6AggPrice
{
	SiftCostInputs in;
	double		selectivity;
	Cost		open;
	double		granule_rows;	/* the rows of a granule of the index: what a participant of a parallel scan claims at a time */
} SiftV6AggPrice;

extern SiftV6AggPrice sift_v6_agg_price(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, const SiftV6Spec *spec, double groups);

/*
 * The price of reading the columns a scan of the relation needs: those of its quals, and key (a join key the scan tests on every row of the granules the zones leave; 0: none), on the rows of
 * those granules, the others on the rows that pass the quals. open is what the scan pays before its first row, total includes it; the work of the operator above is not in it.
 */
extern void sift_v6_scan_price(PlannerInfo *root, RelOptInfo *rel, const IndexOptInfo *ix, AttrNumber key, Cost *open, Cost *total);

/* Per row decoded, the search of the constants of the IN lists among the clauses (over the columns of the table) that a scan of the index ix tests each row against */
extern Cost sift_v6_in_lists_cost(const IndexOptInfo *ix, const List *clauses);

/* What a scan pays to give one row it fetches by its tag: of a sift_table its locate and decode (SIFT_TABLE_FETCH_OPS), of an index the tuple of the heap */
extern Cost sift_v6_fetch_cost(bool is_table);

/* The elements of a constant array (0: NULL or no constant) */
extern uint32 sift_v6_const_array_len(const Node *e);

/* What a sequential read of the relation with its own conditions costs, less the page reads of the share of its pages the cache holds, as sift_v6_dim_cost prices a dimension */
extern Cost sift_v6_rel_read_cost(const RelOptInfo *rel);

/* What the node pays to read a dimension whole: the price of its plan, less the page reads of the share of its pages the cache holds when a sequential scan takes the table */
extern Cost sift_v6_dim_cost(const Path *child);

/*
 * How many times more rows the key of a row owns than the mean, for column att of rel: nd * sum(freq^2) over its values, the frequencies of its most common values by PostgreSQL's
 * statistics and the other rows spread evenly; 1 for a flat column or one without statistics. PostgreSQL prices a probe of a key for the mean, and the keys a lookup passes are ones with rows.
 */
extern double sift_v6_key_skew(PlannerInfo *root, RelOptInfo *rel, AttrNumber att);

/*
 * How many times more keys of column att of rel a lookup passes than PostgreSQL counts: 1 when it counts the lookup from statistics; when a filter of the lookup has none (a jsonb path,
 * an expression) or the lookup is the output of a plan, its count is a default guess of a row or two, and the keys are taken at the geometric mean of the guess and the distinct values
 * of the column.
 */
extern double sift_v6_lookup_keys_guess(PlannerInfo *root, RelOptInfo *rel, AttrNumber att, const RelOptInfo *lookup);

/*
 * The same of the lookups of a join, lookups[i] on column atts[i] of rel (0: an outer join, a join of relations, a key of several columns or of an expression, which is not counted): the most of the lookups
 * on one column, which restrict the same keys, times those of the other columns, up to the rows of rel
 */
extern double sift_v6_lookups_guess(PlannerInfo *root, RelOptInfo *rel, const AttrNumber *atts, RelOptInfo *const *lookups, int n, double input_rows);

/* The share of the granules the clauses leave of a relation that has no statistics of its own: a column that follows the order of the rows keeps the share of the rows, any other (Yao) nearly all */
extern double sift_v6_clause_prune(PlannerInfo *root, const List *clauses, AttrNumber lead_att);
/* The same for the clauses of the sift_table relid: its ctid bounds keep the share of its sampled granules that have a row on a block of the range, as its scan reads them (PostgreSQL prices
 * them by the pages of the relation, which are not its TID space); a format that keeps no TID envelope of its granules, the share of the virtual blocks it handed out that the range covers */
extern double sift_v6_table_prune(PlannerInfo *root, Index relid, const List *clauses, AttrNumber lead_att);

#endif							/* SIFT_AGG_V6_CARD_H */
