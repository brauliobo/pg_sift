/*
 * agg/refuse.h - why a query kept the normal plan: the registry of refusals, recorded by the planner when sift.explain_refusals is on
 * (or while sift.explain_why plans a query), and reported as a notice or as a row of sift.explain_why.
 */
#ifndef SIFT_AGG_REFUSE_H
#define SIFT_AGG_REFUSE_H

#include "postgres.h"

#include "nodes/pathnodes.h"

/*
 * X(ID, slug, sentence): the one table of refusals. The slug is the stable name (docs/USAGE.md lists them); a refusal no site names is 'other'.
 * A site names the first reason an arm (a candidate path of the planner: single, distinct, two_level, rows, topn) gave up for.
 */
#define SIFT_AGG_REFUSALS(X) \
	X(OTHER, "other", "the shape is not one the aggregate pushdown proves, and no more specific reason is recorded") \
	X(DISABLED, "disabled", "sift.enable_agg is off") \
	X(QUERY_KIND, "query_kind", "the query has a set-returning function in the select list, row locks or a recursive CTE") \
	X(FROM_SHAPE, "from_shape", "the FROM list is not one table with the inner, left, semi or anti joins and lateral functions the scan reads") \
	X(TABLE_KIND, "table_kind", "the table is inherited, sampled, row-secured or not a plain, materialized or partitioned table") \
	X(NO_SOURCE, "no_source", "no sift_col index or sift_table covers the table") \
	X(PARTITION_SOURCE, "partition_source", "a partition has no column index the plan can use, or the partitions do not share a layout") \
	X(PARTITION_COUNT, "partition_count", "the table has more partitions left by PostgreSQL's pruning than a scan reads") \
	X(JOIN_CHILD, "join_child", "a joined sub-select, VALUES list, CTE or function is not one the scan can read or run as a child") \
	X(OUTER_JOIN, "outer_join", "an outer join of the scanned table keeps rows the scan cannot reproduce") \
	X(UNNEST_LEAF, "unnest_leaf", "the array or object a lateral function unnests is not a leaf of the column index") \
	X(WHERE, "where", "a condition of the WHERE or ON clause is not one the scan can test") \
	X(JOIN_CONDITION, "join_condition", "a join condition is not an equality or filter the scan can look up") \
	X(GROUP_KEY, "group_key", "a group key is not a leaf, or an expression of one, that the scan reads") \
	X(TOO_MANY_KEYS, "too_many_keys", "the query groups by more columns than the scan holds") \
	X(OUTPUT_EXPR, "output_expr", "the select list or HAVING reads something besides group keys and the aggregates the scan computes") \
	X(AGGREGATE, "aggregate", "an aggregate, or its FILTER, DISTINCT or ORDER BY, is not one the scan computes") \
	X(DISTINCT_ON, "distinct_on", "DISTINCT ON has no total order for the row it keeps") \
	X(GROUPING_SETS, "grouping_sets", "the arm does not group by grouping sets") \
	X(DIRTY_UNITS, "dirty_units", "some unit holds values a leaf the query reads cannot store, and the scan could not read those rows again as SQL does") \
	X(FIRST_MEMORY, "first_memory", "the first rows of the groups would not fit in the memory of a hash aggregate") \
	X(GROUP_MEMORY, "group_memory", "the groups would not fit in the memory of a hash aggregate, and the normal plan, which spills them, is estimated cheaper than the scan with its spill") \
	X(LATERAL_LIMIT, "lateral_limit", "a LATERAL sub-select with LIMIT 1 stays a child that runs for each key: no unique index holds its key, or its ORDER BY leaves ties") \
	X(TWO_ARMS, "two_arms", "the arms of the two level aggregate are not UNION ALL branches of aggregates over one scan, each grouped by a unique row id") \
	X(TWO_KEYS, "two_keys", "a key of the two level aggregate is not one every arm groups by") \
	X(TWO_AGGS, "two_aggs", "an aggregate over the arms is not one the two level scan computes") \
	X(SPINE_SHAPE, "spine_shape", "the join of the arms to a spine is not an equality of their keys, or the WHERE reads the arms") \
	X(SPINE_ROWS, "spine_rows", "the spine has more than 10,000 rows") \
	X(ROWS_SHAPE, "rows_shape", "the grouping of the rows of a SiftAgg plan has no count(DISTINCT) of integers, or a key or aggregate it cannot group") \
	X(TOPN_SHAPE, "topn_shape", "the ORDER BY .. LIMIT is not over the scanned table's columns with a condition the scan tests") \
	X(COST, "cost", "the normal plan is estimated cheaper than the scan") \
	X(CTE_COST, "cte_cost", "the shared CTE plan is estimated cheaper than the plan with the CTE inlined")

typedef enum AggRefusal
{
	AGG_REFUSE_NONE = 0,
#define AGG_REFUSAL_ID(id, slug, text) AGG_REFUSE_##id,
	SIFT_AGG_REFUSALS(AGG_REFUSAL_ID)
#undef AGG_REFUSAL_ID
	AGG_REFUSE_COUNT
} AggRefusal;

extern bool sift_explain_refusals;
extern const char *agg_variant;

/* the refusal that gave up the arm being planned, if it is the first: false, so that a site returns it */
extern bool agg_refuse_note(AggRefusal why, const char *detail);
extern void agg_arm_begin(const char *name, bool candidate);
extern void agg_arm_candidate(void);
extern void agg_arm_spill(double cost, double groups, double bytes);
extern void agg_arm_path(Path *p);
extern void agg_arm_end(void);
extern void agg_report(const char *name, AggRefusal why, const char *detail, bool used);
extern const char *agg_variant_push(const char *v);

/* the arms of one call of the planner hook: the refusals of the picks of its column indexes are one */
typedef struct AggStage
{
	void	   *outer_arm;
	List	   *outer_done;
	int			outer_level;
	bool		on;
} AggStage;

extern void agg_stage_begin(AggStage *st, int level);
extern void agg_stage_end(AggStage *st);

/* `return agg_refuse(ID);` in a function that returns bool, `agg_refused(ID);` before a bare return; _on: with a detail (a name) */
#define agg_refuse(id) (sift_explain_refusals ? agg_refuse_note(AGG_REFUSE_##id, NULL) : false)
#define agg_refused(id) do { if (sift_explain_refusals) agg_refuse_note(AGG_REFUSE_##id, NULL); } while (0)
#define agg_refused_on(id, what) do { if (sift_explain_refusals) agg_refuse_note(AGG_REFUSE_##id, (what)); } while (0)

extern void sift_agg_refuse_init(void);

#endif
