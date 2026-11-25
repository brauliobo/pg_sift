/*
 * col/f2/f2_v6_scope_int.h - what the planning (scope_v6.c) and the executor (scope_v6_run.c) of the scope aggregate share: the statement as a spec the plan carries.
 */
#ifndef SIFT_COL_F2_V6_SCOPE_INT_H
#define SIFT_COL_F2_V6_SCOPE_INT_H

#include "postgres.h"

#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"

#include "agg/v6/v6.h"
#include "col/f2/f2_scope.h"

/* The columns of the tuple the node leaves, in the order of its scan tlist: the scope key, the calendar month of the time leaf, or the aggregate of that number */
#define SCOPE_COL_KEY		(-1)
#define SCOPE_COL_MONTH		(-2)

#define SCOPE_COLS_MAX		(2 * SIFT_V6_MAX_KEYS)

typedef struct ScopeSpec
{
	Oid			relid;			/* the sift_col index, or the sift_table when is_table */
	bool		is_table;
	int			ord;
	bool		by_key;			/* GROUP BY the key: a tuple for every key of the summary */
	Oid			key_type;
	bool		key_given;		/* key = constant, or key = ANY(array): the keys the statement names */
	bool		key_array;		/* key_given by an array: the sorted distinct keys it holds; the cubes of the keys, added when the statement does not group by the key */
	bool		grouped_const;	/* GROUP BY of columns that are constants: no tuple when there is no row */
	bool		by_month;		/* GROUP BY date_trunc('month', time leaf) */
	Oid			month_type;		/* timestamptz or timestamp: what date_trunc answers */
	bool		ranged;			/* a window [lo, hi) of the time leaf, bounds on month starts, is the only other condition on the facts */
	int			lo_strat;
	int			hi_strat;
	Oid			time_type;
	int			excl_col;		/* column of the relation of `col IS NULL`: the rows where it is not NULL leave the cubes (0: none) */
	bool		scan;			/* the facts are scanned, with the members of the key in the quals, for what no cube tells (a qual on other leaves): the spec of the scan follows in the plan */
	int			member_col;		/* scan: the index column of the member of the facts */
	bool		topk;			/* ORDER BY ring_col DESC LIMIT limit of the facts of the key: the newest rows of its ring of the summary; the columns of the rows are the keys of the spec of the scan that follows in the plan */
	int			ring_col;
	bool		nulls_first;
	bool		leaf_not_null;	/* the ring leaf cannot be NULL: the ring (NULLS LAST) serves an order that puts NULLs first */
	int			limit;
	int			tie_col;		/* topk: the second ORDER BY leaf (never NULL, an integer or a time), 0: none */
	bool		tie_desc;
	int			ncols;
	int			col[SCOPE_COLS_MAX];
	int			naggs;
	SiftV6Agg  *agg;
} ScopeSpec;

/* The statement of the scope planner: the relations it joins, the scope that answers them and the spec the plan carries */
typedef struct ScopePlan
{
	PlannerInfo *root;
	RelOptInfo *fact;
	RelOptInfo *pairs;
	IndexOptInfo *index;		/* the sift_col index of the facts; NULL when the facts are a sift_table */
	Relation	irel;			/* the index or the table that keeps the summary */
	SiftF2ScopeLayout layout;
	SiftScopeLeg leg;
	Query	   *pairs_query;	/* the single-table sub-select or CTE the pairs are, NULL for the relation itself */
	Query	   *pairs_raw;		/* a sub-select or CTE of another form: it is the pairs of a scope of two paths when scope_matches says so (pairs_query then stands for it) */
	Expr	   *key_expr;
	Expr	   *lo_expr;
	Expr	   *hi_expr;
	Expr	   *month_expr;
	List	   *scan_tlist;
	ScopeSpec	spec;
	SiftV6Spec	v6;				/* a scan of the facts: its spec and the columns of its tuple */
	List	   *v6_tlist;
	int			cap_aggs;
	bool		semi;			/* the pairs are the right side of a semi join: IN or EXISTS over them */
} ScopePlan;

extern int	sift_v6_scope_index_col(const ScopePlan *p, const Var *v);
extern bool sift_v6_scope_topk_order(ScopePlan *p, SortGroupClause *sgc);
extern bool sift_v6_scope_topk_tie(ScopePlan *p, SortGroupClause *sgc);
extern bool sift_v6_scope_topk_output(ScopePlan *p, const PathTarget *target);

/* A relation of the plain kind the scope planner reads: a table without inheritance, sampling or security quals */
extern bool sift_v6_plain_table(const RangeTblEntry *rte);
extern List *sift_v6_scope_spec_pack(const ScopeSpec *s);
extern ScopeSpec *sift_v6_scope_spec_unpack(const List *ints);

/* The scope of the index ix of the facts that summarizes the pairs relation the statement joins by member (fact column fact_att = pairs column pairs_att) and binds to one key: its ordinal and the expression of the key */
extern bool sift_v6_scope_one_key(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *pairs, IndexOptInfo *ix, AttrNumber fact_att, AttrNumber pairs_att, int *ord, Expr **key);

/* The cells of the leaves (leaves[0 .. nleaves - 1]) of the newest rows of a key under the order of the statement, from its ring and the rows the summary lacks; false: the join answers */
extern bool sift_v6_scope_topn(SiftF2Scope *sc, const ScopeSpec *sp, int64 key, bool no_key, int tie_leaf, const int *leaves, int nleaves, SiftF2ScopeCell **out, int *nout);

/* The scope aggregate (col/f2/f2_v6_scope.c): a join of the facts with the pairs of a scope of the index is answered by the cubes of its summary (col/f2/f2_scope.h) */
extern PGDLLIMPORT const CustomScanMethods sift_agg_v6_scope_plan_methods;

extern void sift_agg_v6_scope_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);
/* ORDER BY ring leaf DESC LIMIT n of the facts of a scope key: the newest rows of its ring (ORDERED stage) */
extern void sift_agg_v6_scope_topk_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel);
extern Plan *sift_agg_v6_scope_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);

#endif							/* SIFT_COL_F2_V6_SCOPE_INT_H */
