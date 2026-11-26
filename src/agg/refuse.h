/*
 * agg/refuse.h - why a query kept the normal plan: the registry of refusals, reported as a notice when sift.explain_refusals is on
 * or as a row of sift.explain_why.
 */
#ifndef SIFT_AGG_REFUSE_H
#define SIFT_AGG_REFUSE_H

#include "postgres.h"

#include "nodes/pg_list.h"

#include "core/limits.h"

/*
 * X(ID, slug, sentence, limit, by_design, action): the one table of refusals. The slug is the stable name (docs/USAGE.md lists them). limit is 1 for a slug that is always a fixed limit of the extension
 * (core/limits.h): the query has more of something than the scan holds, and the detail names the limit and its value. by_design is 1 when the normal plan is the answer (a setting, a row lock,
 * a volatile function, no total order, no index, a choice by cost) and 0 when the pushdown lacks the shape.
 * action is the one line an operator reads after the sentence: what to change in the statement, the index or a setting, or "None needed" when the normal plan is the answer.
 */
#define SIFT_AGG_REFUSALS(X) \
	X(OTHER, "other", "the shape is not one the aggregate pushdown proves, and no more specific reason is recorded", 0, 0, "Try a simpler form of the statement (one table, a plain GROUP BY) to find the part that is refused, and report the statement if it should be accepted.") \
	X(FROM_SHAPE, "from_shape", "the FROM list is not one table with the inner, left, semi or anti joins and lateral functions the scan reads", 0, 0, "Write the FROM list as one table with inner or left joins and lateral functions, or move the other sources into a sub-select the scan can read.") \
	X(NO_SOURCE, "no_source", "no sift_col index or sift_table covers the table", 0, 1, "Create a columnar index: CREATE INDEX ON <table> USING sift_col (<filter_and_group_cols>);") \
	X(WHERE, "where", "a condition of the WHERE or ON clause is not one the scan can test", 0, 0, "Test a column of the index with a comparison, IN, BETWEEN, LIKE or IS NULL, or add the column or expression the condition reads to the sift_col index.") \
	X(VOLATILE, "volatile", "the query calls a volatile function, which PostgreSQL evaluates for every row and the scan cannot reproduce", 0, 1, "None needed, the normal plan is the answer; compute the volatile function outside the statement or in an outer query to get the scan.") \
	X(GROUP_MEMORY, "group_memory", "the groups would not fit in the memory of a hash aggregate, and the scan cannot spill them, or the normal plan, which spills them, is estimated cheaper than the scan with its spill", 1, 1, "Raise work_mem so that the groups fit a hash aggregate, or leave the normal plan, which spills them.") \
	X(COST, "cost", "the normal plan is estimated cheaper than the scan", 0, 1, "None needed, the normal plan is estimated cheaper (compare scan_cost vs normal_cost); run ANALYZE <table> if row estimates are stale.") \
	X(CTE_COST, "cte_cost", "the shared CTE plan is estimated cheaper than the plan with the CTE inlined", 0, 1, "None needed, the cheaper form was chosen; MATERIALIZED or NOT MATERIALIZED on the CTE picks the other.")

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

extern void agg_explain_begin(void);
extern List *agg_explain_take(void);
extern bool sift_agg_refusal_row(int i, const char **slug, const char **text, const char **action, bool *limit);

extern void sift_agg_refuse_init(void);

#endif
