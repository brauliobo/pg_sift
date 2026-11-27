/*
 * agg/plan/int.h - what plan.c (a table) and plan_part.c (a table of partitions) of SiftAggV6 share: the state of the plan of one index, and the compile of a statement against it.
 */
#ifndef SIFT_AGG_V6_PLAN_INT_H
#define SIFT_AGG_V6_PLAN_INT_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/engine.h"
#include "agg/plan/shape.h"
#include "agg/refuse.h"
#include "col/am.h"
#include "core/store/ops.h"

struct SiftV6RelSource;

/* The leaves an unnest reads the arrays of a relation by: those of a sift_col index, or of the store of a sift_table whose format reads its elements (SiftJsonbOps.table_elems); schema NULL: none.
 * Attribute att (of the index or the table) is held by nleaves[att] leaves from first[att]; json[att]: it is a jsonb document */
typedef struct SiftV6Leaves
{
	const SiftColSchema *schema;
	const int  *first;
	const int  *nleaves;
	const bool *json;
} SiftV6Leaves;

typedef struct V6Plan
{
	PlannerInfo *root;
	RelOptInfo *rel;
	RelOptInfo *out;
	Oid			relid;			/* the relation scanned: the index, or the table */
	IndexOptInfo *index;		/* a sift_col: its index; NULL for a sift_table */
	const SiftColRel *colrel;	/* a sift_col: the planner's own copy of the state of its index, which a reload of the relcache entry does not free; NULL for any other source */
	SiftV6Leaves leaves;
	const SiftStoreOps *ops;	/* the format of the relation; for an index NULL when it holds nothing (a stale index) */
	bool		is_table;		/* a sift_table: sch is the schema of its store, as it is of an index, whose attributes are those of the table */
	SiftStoreSchema sch;
	SiftV6Spec	spec;
	int			cap_aggs;		/* the room of spec.agg and spec.func */
	int			cap_funcs;
	List	   *scan_tlist;
	Bitmapset  *scan_vars;		/* the entries of scan_tlist that are plain columns (a Var), by varno */
	SiftFilterReg *freg;		/* the distinct FILTER clauses of the statement */
	bool		in_filter;		/* compiling a FILTER clause: its operands are constants, which the clause folds into its bitmaps at plan time */
	bool		pg_derive;		/* a key or argument that PostgreSQL's evaluator computes is not computed by the native derivations: the partitions of one table compile to one plan whichever kinds of relation they are */
	AggRefusal	why;			/* the first reason the compile gave up for, which sift_v6_compile_on names for the arm, and the expression of the statement it refused (NULL: no one) */
	Node	   *why_at;
	bool		batch_ok;		/* the quals no scan key says may be run over the batches of the scan (SiftV6Spec.vqual): the scan of one relation that no other operator reads the rows of, or the unnest of its array by a store that runs them (SiftJsonbOps.row_quals) */
	struct SiftV6Ext *xseed;	/* the dimensions or the unnest the join of the statement makes of its relations (v6_ext.h); NULL for a scan of the fact relation alone */
	List	   *dim_rels;		/* the RelOptInfo of each dimension, in the order of xseed->dim */
	List	   *gen_vars;		/* the columns the generic aggregates read, from any relation of the statement: the position + 1 of a column is the attribute their expressions read it as (v6_gen.h) */
	RelOptInfo *elem_rel;		/* the relation of the unnest */
	bool		members;		/* a member of a map of a document may be a column of the scan of a format that reads the rows its leaves cannot hold from the document (sift_v6_members_again); a format whose scan reads them from the exceptions of its chunks (SiftJsonbOps.exact_values) always may */
	bool		score_ok;		/* an aggregate over sift.score(ctid): the scan that brings its rows brings their scores (agg/hybrid/plan.c) */
	IndexOptInfo *text;			/* the sift index of the text queries of the statement, if it has them (v6_text.c) */
	List	   *text_quals;		/* the clauses they come from */
	List	   *text_queries;		/* the arms of their search (SiftV6Spec.text_queries) */
	const RestrictInfo *sub_qual;	/* the EXISTS over the elements of an array that the scan joins the dimensions of its elements for and decides itself (v6_exists_plan.h): no clause of the table is compiled for it */
	List	   *tests;			/* the tests of the elements of an array (EXISTS, jsonpath) that the scan decides itself (v6_unnest_exists.h): no clause of the table is compiled for those of a WHERE, a FILTER reads their flags */
	const int  *elem_map;		/* the element column of the union of the tests' (SiftV6ElemRte) that column i of the relation of the test being built is; NULL: its own */
	int			elem_stream;	/* the stream of the Ext (sift_v6_ext_stream) whose elements are the columns of elem_rel */
	bool		heap_rows;		/* the unnest of a sift_table reads every row again by its address: a qual or a test of the statement only the row itself decides (sift_v6_unnest_ready) */
} V6Plan;

/* An index of format major 2 with no expression; usable also has no predicate (a partial index serves a statement that implies it, which only plan.c judges) */
extern bool sift_v6_index_owned(const IndexOptInfo *ix);
extern bool sift_v6_index_usable(const IndexOptInfo *ix);

/* Compiles the keys, aggregates and quals of the statement against p->index; false when the index does not hold what they read */
extern bool sift_v6_compile_on(V6Plan *p, GroupPathExtraData *extra, List *group_exprs);

/*
 * Whether a plan that read members (p->members) is compiled again without them: the format reads the member of a row its leaves cannot hold from the document (no SiftJsonbOps.exact_values), and
 * the plan failed (ok false) or a row the source gives may be left out after it, by a batch qual, a FILTER or a join, or a text bridge folds the rows
 */
extern bool sift_v6_members_again(const V6Plan *p, bool ok);

/* The group path of the statement over one source: compiled with the members of a map as columns of the scan, and again without them when the plan drops rows after the source (plan_hybrid.c) */
extern void sift_v6_source_group_path(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, const struct SiftV6RelSource *s, GroupPathExtraData *extra, List *group_exprs);

/* Compiles the keys, sets and aggregates of the statement, not its quals, against p->index; the caller's scan brings the rows (plan_hybrid.c) */
extern bool sift_v6_compile_head(V6Plan *p, GroupPathExtraData *extra, List *group_exprs);

/* plan_hybrid.c: sift.score(ctid) as the argument of an aggregate when the scan brings scores; the text query (body @@> 'q') among the quals; the statement is of the shape the node answers */
extern bool sift_v6_score_col(V6Plan *p, const Expr *e, SiftV6Col *out);
extern bool sift_v6_text_qual(V6Plan *p, RestrictInfo *ri);
extern bool sift_v6_compile_text(V6Plan *p);
extern bool sift_v6_plan_shape(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel);
extern List *sift_v6_group_exprs_of(PlannerInfo *root);

/* The HAVING of the statement: none below the grouping stage, where extra is NULL */
static inline List *
sift_v6_having(const GroupPathExtraData *extra)
{
	return extra != NULL ? (List *) extra->havingQual : NIL;
}

/* The paths of the one table of the input: the table or each index that holds what the statement reads; extra is NULL below the grouping stage */
extern void sift_v6_scan_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs);
/* plan_distinct.c: SELECT DISTINCT of the columns of one table, at the UPPERREL_DISTINCT stage */
extern void sift_v6_distinct_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel);

/* The path of the plan for the output relation; a spec with parts reads each of their indexes into the same groups */
extern void sift_v6_add_group_path(V6Plan *p, RelOptInfo *output_rel, List *group_exprs, List *having);
/* plan_topn_rows.c: the path of ORDER BY columns LIMIT K of the rows of one table, at the UPPERREL_ORDERED stage */
extern void sift_v6_add_rows_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel);
/* the row Top-N of the one index in hand (a partition's), and its path once the scan is priced; dims are the plans of the dimensions of a join, and what they cost */
extern bool sift_v6_rows_compile(V6Plan *p, uint32 k);
extern Node *sift_v6_rows_window_aggs(Node *expr);
extern void sift_v6_rows_add(V6Plan *p, uint32 k, Cost scan, List *dims, Cost dims_startup, Cost dims_total);
/* a partitioned table the plan can read as one scan, and the row Top-N over the partitions left after pruning */
extern bool sift_v6_partitioned_rel_ok(PlannerInfo *root, RelOptInfo *rel);
extern void sift_v6_add_rows_partition_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, uint32 k);

/* The plan of a sift_table or of an open index: the format that owns it (ops NULL when none does) and, for a table, the schema of its store; false: no format owns the table */
extern bool sift_v6_plan_table(V6Plan *p, Oid relid);
extern void sift_v6_plan_index(V6Plan *p, Relation irel);

/* The relation of the plan as the scan reads it, and the operator family of an attribute of the host */
extern SiftV6Rel sift_v6_plan_rel(const V6Plan *p);
extern Oid sift_v6_plan_opfamily(const V6Plan *p, int att);

/* plan_qual.c: the quals of the index scan; the index holds every row the statement reads; the quals of the statement compiled into p->spec (false: the scan cannot decide one) */
extern Expr *sift_v6_index_qual(V6Plan *p, Expr *clause);
/* the comparison of one column the clause implies when it is no index qual itself (a row comparison, a coalesce, a nullif, IS NOT DISTINCT FROM), NULL for none (agg/plan/prednorm.c) */
extern Expr *sift_v6_implied_qual(PlannerInfo *root, Expr *clause);
/* a IS DISTINCT FROM a constant as the tests a clause of the scan says: a <> c OR a IS NULL, or a IS NOT NULL for a NULL c; NULL when it is not of that form (agg/plan/prednorm.c) */
extern Expr *sift_v6_distinct_tests(const DistinctExpr *d);
extern bool sift_v6_index_complete(const V6Plan *p);
extern bool sift_v6_compile_quals(V6Plan *p);

/* plan_rows_out.c: the columns a row Top-N gives, and the dimensions of its join for EXPLAIN */
extern bool sift_v6_rows_outputs(V6Plan *p);
extern char *sift_v6_rows_dims_desc(const V6Plan *p);
/* An aggregate the native kernels of the scan fold, not entered in the scan tlist (the caller decides what the node gives it as) */
extern bool sift_v6_compile_agg_native(V6Plan *p, Aggref *a);

/* plan_tid.c: a WHERE clause that bounds ctid by a constant TID narrows the TID range of the spec; false: not one */
extern bool sift_v6_tid_qual(V6Plan *p, const RestrictInfo *ri);

/* A column of the fact relation that its index holds as a plain leaf of the class of its type */
extern bool sift_v6_plan_column(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out);
/* plan_cols.c: a column the scan reads whatever its form (a leaf, a datum, an index or generated expression), and a key or argument the scan computes from columns (v6_plan_rewrite.h) */
extern bool sift_v6_read_of(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out);
extern bool sift_v6_derived_col(V6Plan *p, Expr *e, bool key, SiftV6Col *col);
/* the resolver of the columns of an expression of the scan for v6_plan_rewrite.h */
extern bool sift_v6_resolve_column(void *ctx, const Expr *e, bool numeric_ok, SiftV6Col *out);
/* A sort key of a row Top-N: a column or an expression of the scan under any collation, since the heap of the best rows compares by the sort operator */
extern bool sift_v6_compile_sort_key(V6Plan *p, Expr *e, SiftV6Col *k);
/* a column the scan holds in a native vector: of the kernels' classes, or the text of a table */
extern bool sift_v6_plan_native_column(V6Plan *p, const Expr *e, SiftV6Col *out);
/* the same for a column the scan reads as any value: the kernels' columns, the text of a table, the datums, and a column of a dimension or of the unnested element (an aggregate of PostgreSQL's own functions reads them) */
extern bool sift_v6_plan_read_column(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out);
/* a key or argument PostgreSQL's executor computes from columns of the scan, as a column of the scan */
extern bool sift_v6_pg_derived_col(V6Plan *p, Expr *e, bool key, SiftV6Col *col);
/* plan_filter.c: the FILTER clause of an aggregate, as the id (1 on) of a clause of the scan; false when the scan cannot decide it */
extern bool sift_v6_compile_filter(V6Plan *p, Expr *filter, int *fclause);
/* An OR of the WHERE one of whose arms is a test of the elements of the row (p->tests): the OR of the flag of the test and the quals of the other arms, as a FILTER has them (NULL: none) */
extern Expr *sift_v6_tests_or_qual(V6Plan *p, Expr *clause);

/* the expression with each placeholder replaced by what it holds: the value the scan computes where the expression is read */
extern Node *sift_v6_placeholder_contents(Node *n, void *context);
/* e with the jsonb_typeof(doc -> 'key') of its shredded keys, and with values their `->>` text and its float8 cast, as virtual columns (agg/qual/jsonb.c), for a scan that reads them; e itself when this one cannot */
extern Expr *sift_v6_plan_json_vars(V6Plan *p, const Expr *e, bool values);
/* The scan raises the error of a float8 cast of a key of a document for the rows PostgreSQL's own plan raises it for (agg/jsonb_order.c) */
extern bool sift_v6_json_cast_order_ok(V6Plan *p);
/* The statement is one the node may answer, whatever its relations; and the relation is a plain table the node may read (no inheritance, sampling or security quals) */
extern bool sift_v6_plan_query_ok(PlannerInfo *root, RelOptInfo *output_rel);
extern bool sift_v6_plan_fact_ok(PlannerInfo *root, RelOptInfo *rel);
/* The base relations of a join relation: its relids without the joins of the query */
extern Relids sift_v6_plan_base_relids(PlannerInfo *root, RelOptInfo *rel);
/* The path of a join: child_paths are the plans of the dimensions, whose cost and that of the join come on top of the scan's */
extern void sift_v6_add_join_path(V6Plan *p, List *group_exprs, List *having, List *child_paths, Cost extra_startup, Cost extra_total, double input_rows);

/* The columns the planners of joins and unnests answer for plan.c: false when the expression is not one of theirs */
extern bool sift_v6_star_column(V6Plan *p, const Expr *e, SiftV6Col *out);
extern bool sift_v6_star_joined_col(V6Plan *p, Expr *e, bool key, SiftV6Col *out);
extern bool sift_v6_unnest_column(V6Plan *p, const Expr *e, SiftV6Col *out);
/* The pieces of the planner of a star join that the planner of the other joins (v6_join_plan.h) reuses */
extern const Var *sift_v6_plain_var(const Node *n);
extern bool sift_v6_key_types_agree(Oid ftype, Oid dtype, Oid inputcollid);
extern bool sift_v6_key_type_ok(Oid typid);
extern bool sift_v6_in_target(const RelOptInfo *rel, const Var *v);
extern bool sift_v6_dim_rel_ok(PlannerInfo *root, RelOptInfo *rel, uint64 budget);
extern void sift_v6_star_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs);
extern void sift_v6_star_rows_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, uint32 k);
extern bool sift_v6_unnest_ready(V6Plan *p);
extern Aggref *sift_v6_unnest_filter_as_case(const V6Plan *p, Aggref *a);
/* the pieces of it that the planner of an EXISTS over the elements (v6_unnest_exists.h) takes apart: the statement compiles to what the scan of an unnest computes; the quals of the table become those the scan evaluates on its batches and on a row of the heap */
extern bool sift_v6_unnest_compile(V6Plan *p, GroupPathExtraData *extra, List *group_exprs);
/* the quals of a relation (RestrictInfo) as the executor of the statement runs them: the cheaper first (a row that fails one is not given to a cast that raises for it), the order of the statement among equals */
extern List *sift_v6_quals_in_order(PlannerInfo *root, List *infos);
extern bool sift_v6_unnest_where(V6Plan *p);
/* v6_unnest_exists.c: the qual of the flag of the test a conjunct of a FILTER is (the virtual attribute SIFT_V6_XFLAG_VATT + the test against true or false); NULL when it is none of the tests of the plan */
extern Expr *sift_v6_unnest_test_qual(const V6Plan *p, const Node *conjunct);
/* the WHERE clause is a test of the plan, which the scan decides: it is not compiled as a qual of the table */
extern bool sift_v6_unnest_test_ri(const V6Plan *p, const RestrictInfo *ri);
/* the rows of the join of the fact rows with their elements, from the lengths of the arrays the index holds (PostgreSQL's own when it has none) */
extern double sift_v6_unnest_rows(const V6Plan *p, const RelOptInfo *input_rel);
extern Cost sift_v6_unnest_pairs_cost(const V6Plan *p, const RelOptInfo *input_rel);
extern void sift_v6_unnest_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);

/* What a planner does with a plan bound to one source of a fact: seeds the join of the statement in it and compiles the statement; for a partitioned fact it does so for a source of every partition */
typedef bool (*SiftV6CompileFn) (V6Plan *p, const void *stmt);
/* Adds the path of a plan that compiled: for a partitioned fact the one plan of all its partitions */
typedef void (*SiftV6AddFn) (V6Plan *p, const void *stmt);

/* The fact of a join the plan reads: a plain table that has a source, or a partitioned table */
extern bool sift_v6_join_fact_ok(PlannerInfo *root, RelOptInfo *fact);
/* The paths of the fact of a join: for each source of a plain table, the one plan over the partitions of a partitioned table */
extern void sift_v6_fact_paths(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *out, SiftV6CompileFn compile, SiftV6AddFn add, const void *stmt);
/* The plan of the statement over the partitions of parent that pruning left, and the source of the first partition it was compiled against */
struct SiftV6RelSource;
extern bool sift_v6_compile_partitions(PlannerInfo *root, RelOptInfo *parent, RelOptInfo *out, SiftV6CompileFn compile, const void *stmt, V6Plan *first, struct SiftV6RelSource *like);

/* The paths for a partitioned table: one SiftAggV6 over the live partitions, whose indexes all lay the statement out alike */
extern void sift_agg_v6_partition_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs);

#endif							/* SIFT_AGG_V6_PLAN_INT_H */
