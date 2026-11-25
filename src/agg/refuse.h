/*
 * agg/refuse.h - why a query kept the normal plan: the registry of refusals, recorded by the planner when sift.explain_refusals is on
 * (or while sift.explain_why plans a query), and reported as a notice or as a row of sift.explain_why.
 */
#ifndef SIFT_AGG_REFUSE_H
#define SIFT_AGG_REFUSE_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "core/limits.h"

/*
 * X(ID, slug, sentence, limit, by_design, action): the one table of refusals. The slug is the stable name (docs/USAGE.md lists them); a refusal no site names is 'other'. limit is 1 for a slug that is always a fixed limit of the extension (core/limits.h): the
 * query has more of something than the scan holds, and the detail names the limit and its value. A site that meets a registered limit under another slug
 * (a top-N past its rows) says so with agg_refuse_limit. by_design is 1 when the normal plan is the answer (a setting, a row lock, a volatile function, no
 * total order, no index, a choice by cost) and 0 when the pushdown lacks the shape.
 * action is the one line an operator reads after the sentence: what to change in the statement, the index or a setting, or "None needed" when the normal plan is the answer.
 * A site names the first reason an arm (a candidate path of the planner: single, distinct, two_level, rows, topn) gave up for.
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
extern bool sift_explain_refusals;
extern const char *agg_variant;

/* the refusal that gave up the arm being planned, if it is the first: false, so that a site returns it */
extern bool agg_refuse_note(AggRefusal why, const char *detail);
/* a refusal of the plan EXPLAIN (SIFT) reports: the arm, the slug of the registry and the reason */
typedef struct SiftRefusalNote
{
	const char *arm;
	const char *slug;
	const char *reason;
} SiftRefusalNote;

extern void agg_explain_begin(void);
extern void agg_explain_planned(void);
extern List *agg_explain_take(void);
extern bool sift_agg_refusal_row(int i, const char **slug, const char **text, const char **action, bool *limit);
extern void agg_refuse_retry(void);
/* the same for a refusal by the limit lim of the registry (SiftLimitId): the arm counts it under that name */
extern bool agg_refuse_limit_note(AggRefusal why, int lim, const char *detail);
extern void agg_arm_begin(const char *name, bool candidate);
extern void agg_arm_claim_required(void);
extern void agg_arm_candidate(void);
extern void agg_arm_spill(double cost, double groups, double bytes);
extern void agg_arm_vet(double cost, double pct);
extern void agg_arm_path(Path *p);
extern double agg_vet_of(double total_cost, double *pct);
extern void agg_vet_forget(void);
extern void agg_arm_end(void);
extern void agg_report(const char *name, AggRefusal why, const char *detail, bool used, double scan, double normal);
extern const char *agg_variant_push(const char *v);
extern bool agg_quiet_reports(bool quiet);
extern void agg_variant_hold(void);
extern void agg_variant_settle(bool kept, const char *detail);

/* the arms of one call of the planner hook: the refusals of the picks of its column indexes are one */
typedef struct AggStage
{
	void	   *outer_arm;
	List	   *outer_done;
	int			outer_level;
	bool		outer_volatile;
	bool		on;
	PlannerInfo *root;
} AggStage;

extern void agg_stage_begin(AggStage *st, PlannerInfo *root);
extern void agg_stage_end(AggStage *st);
extern void agg_final_paths(PlannerInfo *root, RelOptInfo *final_rel);

/*
 * `return agg_refuse(ID);` in a function that returns bool, `agg_refused(ID);` before a bare return; _on: with a detail (a name); _f: with a detail
 * built from a format, evaluated only while the refusals are noted. _limit: a limit of core/limits.h (joins, combos, ...) by its id, with the value the
 * site compares with (sift_limit(id, MACRO) once the limit is hooked for sift.test_limits) and a format for what the query has more of: the detail
 * ends "the limit <id> is <value>" and sift.stats counts limit.<id>.
 */
#define agg_refuse(id) (sift_explain_refusals ? agg_refuse_note(AGG_REFUSE_##id, NULL) : false)
#define agg_refused(id) do { if (sift_explain_refusals) agg_refuse_note(AGG_REFUSE_##id, NULL); } while (0)
#define agg_refused_on(id, what) do { if (sift_explain_refusals) agg_refuse_note(AGG_REFUSE_##id, (what)); } while (0)
#define agg_refuse_on(id, what) (sift_explain_refusals ? agg_refuse_note(AGG_REFUSE_##id, (what)) : false)
#define agg_refuse_f(id, ...) (sift_explain_refusals ? agg_refuse_note(AGG_REFUSE_##id, psprintf(__VA_ARGS__)) : false)
#define agg_refused_f(id, ...) do { if (sift_explain_refusals) agg_refuse_note(AGG_REFUSE_##id, psprintf(__VA_ARGS__)); } while (0)
#define agg_refuse_limit(slug, lim, value, ...) \
	(sift_explain_refusals ? agg_refuse_limit_note(AGG_REFUSE_##slug, SIFT_LIM_##lim, \
		psprintf("%s, the limit %s is %lld", psprintf(__VA_ARGS__), #lim, (long long) (value))) : false)
#define agg_refused_limit(slug, lim, value, ...) ((void) agg_refuse_limit(slug, lim, value, __VA_ARGS__))

extern void sift_agg_refuse_init(void);

#endif
