/*
 * agg/refuse.h - why a query kept the normal plan: the registry of refusals, reported as a notice when sift.explain_refusals is on
 * or as a row of sift.explain_why.
 */
#ifndef SIFT_AGG_REFUSE_H
#define SIFT_AGG_REFUSE_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "core/limits.h"

/*
 * X(ID, slug, sentence, limit, by_design, action): the one table of refusals. The slug is the stable name (docs/USAGE.md lists them). limit is 1 for a slug that is always a fixed limit of the extension
 * (core/limits.h): the query has more of something than the scan holds, and the detail names the limit and its value. by_design is 1 when the normal plan is the answer (a setting, a row lock,
 * a volatile function, no total order, no index, a choice by cost) and 0 when the pushdown lacks the shape.
 * action is the one line an operator reads after the sentence: what to change in the statement, the index or a setting, or "None needed" when the normal plan is the answer.
 */
#define SIFT_AGG_REFUSALS(X) \
	X(OTHER, "other", "the shape is not one the aggregate pushdown proves, and no more specific reason is recorded", 0, 0, "Try a simpler form of the statement (one table, a plain GROUP BY) to find the part that is refused, and report the statement if it should be accepted.") \
	X(NO_SOURCE, "no_source", "no sift_col index or sift_table covers the table", 0, 1, "Create a columnar index: CREATE INDEX ON <table> USING sift_col (<filter_and_group_cols>);") \
	X(WHERE, "where", "a condition of the WHERE or ON clause is not one the scan can test", 0, 0, "Test a column of the index with a comparison, IN, BETWEEN, LIKE or IS NULL, or add the column or expression the condition reads to the sift_col index.") \
	X(AGGREGATE, "aggregate", "an aggregate, or its FILTER, DISTINCT or ORDER BY, is not one the scan computes", 0, 0, "Use aggregates supported by SiftAgg (see docs/AGG.md) without the unsupported clause, or compute the remaining aggregates in an outer query.") \
	X(KEY, "key", "a GROUP BY key, a grouping set or an output of the statement is not one the scan computes", 0, 0, "Group by columns of the index or expressions of them (date_trunc, date_bin, arithmetic), and compute the other outputs in an outer query.") \
	X(JSONB, "jsonb", "the statement reads both the JSON type and the value of a key, and the leaf of the key cannot hold the values of the other types its type test leaves out", 0, 0, "Test the type in the WHERE clause (jsonb_typeof(doc -> 'k') = 'number'), whose scan keys drop the other rows before the value is read.") \
	X(JSON_ORDER, "json_order", "a cast of a JSON value that can raise would run for a row the normal plan tests first and drops", 0, 0, "Guard the cast with a type test of the same key (jsonb_typeof(doc -> 'k') = 'number'), or declare the path with the type it is cast to.") \
	X(FORMAT, "format", "the format of the index or sift_table lacks a capability the statement needs", 0, 0, "Rebuild the index or sift_table in the current format (REINDEX, VACUUM FULL), or compute the part the detail names in an outer query.") \
	X(SCOPE, "scope", "a join to the scope relation of a scope index is not one the scope read proves equal to the join: the join runs as written", 0, 0, "Join the scope relation by its declared member column to the declared column of the table, bind its scope key with an equality or an IN list of constants or parameters, and read no other column of it; the detail names what differs.") \
	X(TOPN_SHAPE, "topn_shape", "the ORDER BY with LIMIT is not over the scanned table's columns with a condition the scan tests", 0, 0, "ORDER BY columns of the scanned table with a WHERE filter the scan tests, followed by LIMIT; compute DISTINCT, set operations and windows that are not aggregates over the whole set in an outer query.") \
	X(COST, "cost", "the normal plan is estimated cheaper than the scan", 0, 1, "None needed, the normal plan is estimated cheaper (compare scan_cost vs normal_cost); run ANALYZE <table> if row estimates are stale.") \
	X(JOIN, "join", "the statement joins more tables than the scan looks up, or joins them in a way it does not", 0, 0, "Join the fact table to at most 16 small tables, each on equalities of its keys, or aggregate the rest of the join in an outer query.") \
	X(TWO_ARMS, "two_arms", "the arms of the two level aggregate are not UNION ALL branches of aggregates over one scan, each grouped by a unique row id", 0, 0, "Write the arms as UNION ALL branches of aggregates over one scan, each grouped by a unique row id.") \
	X(WINDOW_FIRST, "window_first", "the first rows of each group, asked with a window function (rn = 1, rn <= N), are not the one window of a sub-select the scan computes", 0, 0, "Ask the first rows of each group with one window (row_number(), rank() or dense_rank() OVER (PARTITION BY .. ORDER BY ..)) in the sub-select, and compute the others in an outer query.")

typedef enum AggRefusal
{
	AGG_REFUSE_NONE = 0,
#define AGG_REFUSAL_ID(id, slug, text, limit, by_design, action) AGG_REFUSE_##id,
	SIFT_AGG_REFUSALS(AGG_REFUSAL_ID)
#undef AGG_REFUSAL_ID
	AGG_REFUSE_COUNT
} AggRefusal;

extern const char *sift_agg_limit_name(int lim);

/* a refusal of the plan EXPLAIN (SIFT) reports: the arm, the slug of the registry and the reason */
typedef struct SiftRefusalNote
{
	const char *arm;
	const char *slug;
	const char *reason;
} SiftRefusalNote;

/* a planner of the aggregate leaves its path out for the refusal why of the registry: a row of sift.explain_why for the arm, a notice, a count of sift.stats() (scan 0 and normal < 0: no costs) */
extern void sift_agg_refuse(const char *arm, AggRefusal why, const char *detail, double scan, double normal);

/*
 * For sift.explain_why, the row of an arm of the aggregate: it added a path of cost scan beside the normal plan of cost normal (< 0: none), and the plan takes it (sift_agg_arm_used)
 * or not (cost); an arm that added no path says why (sift_agg_arm_refused, the first reason; _by names what it refused, which is worth making only when sift_agg_arms_noted)
 */
extern void sift_agg_arm_priced(const char *arm, double scan, double normal);
extern void sift_agg_arm_used(const char *arm);
/* the cheapest path of rel that no arm of the aggregate added, once a LIMIT stops it after k rows (k < 0: all of them); -1: none */
extern double sift_agg_normal_cost(const RelOptInfo *rel, double k);
extern void sift_agg_arm_refused(const char *arm, AggRefusal why);
extern void sift_agg_arm_refused_by(const char *arm, AggRefusal why, const char *detail);
/* the same, for a reason about the shape of the form of the statement being planned: one that a planning of another form of it names (agg/plan/cte.c) replaces */
extern void sift_agg_arm_refused_by_shape(const char *arm, AggRefusal why, const char *detail);
extern bool sift_agg_arms_noted(void);

extern void agg_explain_begin(void);
/* The statement EXPLAIN (SIFT) explains is planned (its outermost planning ended): the arms it priced join its refusals, and what it plans while it runs is not its own */
extern void agg_explain_planned(void);
extern List *agg_explain_take(void);
extern bool sift_agg_refusal_row(int i, const char **slug, const char **text, const char **action, bool *limit);

extern void sift_agg_refuse_init(void);

#endif
