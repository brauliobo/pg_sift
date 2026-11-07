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
	X(DISABLED, "disabled", "sift.enable_agg is off", 0, 1, "SET sift.enable_agg = on (it is off in this session, role or database).") \
	X(QUERY_KIND, "query_kind", "the query has a set-returning function in the select list, row locks or a recursive CTE", 0, 1, "None needed, the normal plan is the answer; take the set-returning function, FOR UPDATE or the recursive CTE out of the statement to get the scan.") \
	X(FROM_SHAPE, "from_shape", "the FROM list is not one table with the inner, left, semi or anti joins and lateral functions the scan reads", 0, 0, "Write the FROM list as one table with inner or left joins and lateral functions, or move the other sources into a sub-select the scan can read.") \
	X(TABLE_KIND, "table_kind", "the table is inherited, sampled, row-secured or not a plain, materialized or partitioned table", 0, 1, "None needed, the normal plan is the answer; the scan reads plain, materialized and partitioned tables that are not inherited, sampled or row-secured.") \
	X(NO_SOURCE, "no_source", "no sift_col index or sift_table covers the table", 0, 1, "CREATE INDEX .. USING sift_col on the table; sift.index_choices shows what it would hold.") \
	X(PARTITION_SOURCE, "partition_source", "a partition has no column index the plan can use, or the partitions do not share a layout", 0, 0, "Create the same sift_col index (same columns and options) on every partition, as CREATE INDEX on the parent does.") \
	X(PARTITION_COUNT, "partition_count", "the table has more partitions left by PostgreSQL's pruning than a scan reads", 1, 0, "Filter on the partition key so that fewer partitions are left, or query the partitions that matter.") \
	X(JOIN_CHILD, "join_child", "a joined sub-select, VALUES list, CTE or function is not one the scan can read or run as a child", 0, 0, "Join a table the scan reads, or write the sub-select, VALUES list or CTE as a single-table SELECT with a WHERE; the detail names the child.") \
	X(OUTER_JOIN, "outer_join", "an outer join of the scanned table keeps rows the scan cannot reproduce", 0, 0, "Use an inner join if the unmatched rows are not needed, or aggregate the joined table in a sub-select first.") \
	X(UNNEST_LEAF, "unnest_leaf", "the array or object a lateral function unnests is not a leaf of the column index", 0, 0, "Add the array or object path to the paths option of the sift_col index (WITH (paths = '..')) and REINDEX.") \
	X(WHERE, "where", "a condition of the WHERE or ON clause is not one the scan can test", 0, 0, "Test a column of the index with a comparison, IN, BETWEEN, LIKE or IS NULL, or add the column or expression the condition reads to the sift_col index.") \
	X(JSONB_PATH_NOT_DECLARED, "jsonb_path_not_declared", "a containment (@>) or key test (?, ?|, ?&) on a jsonb column names a path the index does not declare in paths, so no leaf holds its values", 0, 0, "Declare the path in the paths of the index (sift.suggest_paths writes the spec), or test a path it declares; the detail names the path.") \
	X(JOIN_CONDITION, "join_condition", "a join condition is not an equality or filter the scan can look up", 0, 0, "Join on an equality of columns of the scanned table and the other side, or an IN list, and filter the rest in the WHERE.") \
	X(GROUP_KEY, "group_key", "a group key is not a leaf, or an expression of one, that the scan reads", 0, 0, "Add the column the key reads to the sift_col index, or group by the column itself.") \
	X(TOO_MANY_KEYS, "too_many_keys", "the query groups by more columns than the scan holds", 1, 0, "Group by fewer columns; the detail names the limit.") \
	X(ERROR_PARITY, "error_parity", "the scan could raise, or not raise, an error for a value where the normal plan does the other (a cast of a JSON null before PostgreSQL 18, a guard of jsonb_typeof around a read)", 0, 0, "None needed, the normal plan keeps SQL's errors; guard the cast so that it returns NULL, or use PostgreSQL 18, to get the scan.") \
	X(OUTPUT_EXPR, "output_expr", "the select list or HAVING reads something besides group keys and the aggregates the scan computes", 0, 0, "Select the group keys and the aggregates only, and compute the expression over them in an outer query.") \
	X(AGGREGATE, "aggregate", "an aggregate, or its FILTER, DISTINCT or ORDER BY, is not one the scan computes", 0, 0, "Use aggregates the scan computes (the list in USAGE) without the clause that is refused, or compute the rest in an outer query.") \
	X(DISTINCT_ON, "distinct_on", "DISTINCT ON keeps a row the scan cannot pick: more than four sort columns after the ON columns, a sort or select-list column of a joined table, or a volatile select list", 0, 1, "None needed, the normal plan is the answer; keep the sort columns after the ON columns to four and the select list on the scanned table to get the scan.") \
	X(GROUPING_SETS, "grouping_sets", "the arm does not group by grouping sets", 0, 0, "Use a plain GROUP BY for this arm; the single-scan arm answers ROLLUP, CUBE and GROUPING SETS.") \
	X(DIRTY_UNITS, "dirty_units", "some unit holds values a leaf the query reads cannot store, and the scan could not read those rows again as SQL does", 0, 0, "Find the leaf with values it cannot store in sift.col_leaves, fix the data or the type of its path, and REINDEX.") \
	X(DIRTY_ORDER, "dirty_order", "a condition casts a path whose value some unit cannot store, and PostgreSQL may test it before a condition that would drop the row whose value raises, or after one that raises too", 0, 1, "None needed, PostgreSQL's order of the conditions decides; put the guard in a CASE or a sub-select so that it runs first.") \
	X(VOLATILE, "volatile", "the query calls a volatile function, which PostgreSQL evaluates for every row and the scan cannot reproduce", 0, 1, "None needed, the normal plan is the answer; compute the volatile function outside the statement or in an outer query to get the scan.") \
	X(FIRST_MEMORY, "first_memory", "the first rows of the groups would not fit in the memory of a hash aggregate", 1, 0, "Raise work_mem or group by fewer values; the detail names the limit.") \
	X(GROUP_MEMORY, "group_memory", "the groups would not fit in the memory of a hash aggregate, and the scan cannot spill them, or the normal plan, which spills them, is estimated cheaper than the scan with its spill", 1, 1, "Raise work_mem so that the groups fit a hash aggregate, or leave the normal plan, which spills them.") \
	X(LATERAL_LIMIT, "lateral_limit", "a LATERAL sub-select with LIMIT 1 stays a child that runs for each key: no unique index holds its key, or its ORDER BY leaves ties", 0, 0, "Add a unique index on the key of the LATERAL sub-select, or an ORDER BY that makes its LIMIT 1 unique.") \
	X(DISTINCT_SET, "distinct_set", "count(DISTINCT x) keeps the distinct values of each group: x is not a NOT NULL column of the scanned table under a unique single-column index, so its rows cannot be counted in its place", 0, 0, "Count DISTINCT over a NOT NULL column under a unique single-column index, or leave the normal plan.") \
	X(TWO_ARMS, "two_arms", "the arms of the two level aggregate are not UNION ALL branches of aggregates over one scan, each grouped by a unique row id", 0, 0, "Write the arms as UNION ALL branches of aggregates over one scan, each grouped by a unique row id.") \
	X(TWO_KEYS, "two_keys", "a key of the two level aggregate is not one every arm groups by", 0, 0, "Group every arm by the keys the outer aggregate groups by.") \
	X(TWO_AGGS, "two_aggs", "an aggregate over the arms is not one the two level scan computes", 0, 0, "Use sum, count, min or max over the arms, or compute the other aggregate in an outer query.") \
	X(SPINE_SHAPE, "spine_shape", "the join of the arms to a spine is not an equality of their keys, or the WHERE reads the arms", 0, 0, "Join the arms to the spine on an equality of their keys, and keep the WHERE off the arms.") \
	X(SHAPE_WIDTH, "shape_width", "the query has more joined tables, value combinations, leaves, arms or columns than the scan holds: the detail names the limit and its value", 1, 0, "Reduce the joined tables, value combinations, leaves, arms or columns below the limit the detail names.") \
	X(SPINE_ROWS, "spine_rows", "the rows of the spine are estimated above sift.agg_lookup_max_kb", 1, 0, "Raise sift.agg_lookup_max_kb, or filter the spine to fewer rows.") \
	X(ROWS_SHAPE, "rows_shape", "the grouping of the rows of a SiftAgg plan has no count(DISTINCT) of integers, or a key or aggregate it cannot group", 0, 0, "Group by integer keys with aggregates the scan computes (count(DISTINCT) of integers), or leave the normal plan.") \
	X(TOPN_SHAPE, "topn_shape", "the ORDER BY .. LIMIT is not over the scanned table's columns with a condition the scan tests", 0, 0, "ORDER BY columns of the scanned table, with a WHERE the scan tests, then LIMIT.") \
	X(WINDOW_FIRST, "window_first", "the first rows of each group, asked with a window function (rn = 1, rn <= N), are not a lone row_number(), rank() or dense_rank() with PARTITION BY and ORDER BY in a sub-select that does nothing else, over a table with a column index for N > 1 (rank and dense_rank: up to 1,000)", 0, 0, "Write the first rows of each group as a lone row_number(), rank() or dense_rank() OVER (PARTITION BY .. ORDER BY ..) in a sub-select that does nothing else, kept with rn = 1 or rn <= N.") \
	X(COST, "cost", "the normal plan is estimated cheaper than the scan", 0, 1, "None needed, the normal plan is estimated cheaper; ANALYZE the table if the estimates look wrong, and compare with sift.agg_force = on (the row shows both costs).") \
	X(CTE_COST, "cte_cost", "the shared CTE plan is estimated cheaper than the plan with the CTE inlined", 0, 1, "None needed, the cheaper form was chosen; MATERIALIZED or NOT MATERIALIZED on the CTE picks the other.")

typedef enum AggRefusal
{
	AGG_REFUSE_NONE = 0,
#define AGG_REFUSAL_ID(id, slug, text, limit, by_design, action) AGG_REFUSE_##id,
	SIFT_AGG_REFUSALS(AGG_REFUSAL_ID)
#undef AGG_REFUSAL_ID
	AGG_REFUSE_COUNT
} AggRefusal;

extern bool sift_explain_refusals;
extern const char *agg_variant;

/* the refusal that gave up the arm being planned, if it is the first: false, so that a site returns it */
extern bool agg_refuse_note(AggRefusal why, const char *detail);
extern bool sift_agg_refusal_row(int i, const char **slug, const char **text, const char **action, bool *limit);
extern void agg_refuse_retry(void);
/* the same for a refusal by the limit lim of the registry (SiftLimitId): the arm counts it under that name */
extern bool agg_refuse_limit_note(AggRefusal why, int lim, const char *detail);
extern void agg_arm_begin(const char *name, bool candidate);
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
