/*
 * agg/scope/scope.h - the planner of the scope aggregate of SiftAggV6 (plan.c, keys.c, match.c): a join of the facts with the pairs of a scope of the relation that keeps a summary of it, answered
 * from the summary by the executor of the format that owns the relation (SiftScopeOps.exec). The planner reaches the format through SiftScopeOps only; the plan carries the statement as a spec.
 */
#ifndef SIFT_AGG_SCOPE_SCOPE_H
#define SIFT_AGG_SCOPE_SCOPE_H

#include "postgres.h"

#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"

#include "agg/engine.h"
#include "col/scope/scope.h"
#include "core/store/ops.h"

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
	bool		once;			/* a semi join (IN, EXISTS): a fact counts once, not once for each pair of its member with the keys */
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
	const SiftScopeOps *scope;	/* the scopes of the format the planner asks for */
	const SiftStoreOps *ops;	/* the format of the relation that keeps the scope, once a relation of it is found */
	PlannerInfo *root;
	RelOptInfo *fact;
	RelOptInfo *pairs;
	IndexOptInfo *index;		/* the sift_col index of the facts; NULL when the facts are a sift_table */
	Relation	irel;			/* the index or the table that keeps the summary */
	const Oid  *table_opfamily; /* the operator families of the columns of a sift_table (by attribute) */
	SiftScopeLayout layout;
	SiftScopeLeg leg;
	Query	   *pairs_query;	/* the single-table sub-select or CTE the pairs are, NULL for the relation itself */
	Query	   *pairs_raw;		/* a sub-select or CTE of another form: it is the pairs of a scope of two paths when sift_v6_scope_matches says so (pairs_query then stands for it) */
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
	bool		once;			/* the statement wrote IN or EXISTS over the pairs (a semi join, or the inner join PostgreSQL made of it): a fact counts once */
} ScopePlan;

/* keys.c: the key the statement binds the pairs to, and the attribute of the pairs relation a Var of the pairs side is */
extern AttrNumber sift_v6_scope_pairs_attno(const ScopePlan *p, const Var *v);
extern bool sift_v6_scope_key_var(const ScopePlan *p, const Node *n);
extern bool sift_v6_scope_key_filter(ScopePlan *p);
extern Node *sift_v6_strip_relabel(Node *n);

/* match.c: the pairs side and the scope of the relation that summarizes it */
extern bool sift_v6_plain_table(const RangeTblEntry *rte);
extern bool sift_v6_scope_plain_relation(PlannerInfo *root, RelOptInfo *rel);
extern bool sift_v6_scope_pairs_ok(ScopePlan *p);
extern bool sift_v6_scope_pairs_joined(ScopePlan *p);
extern bool sift_v6_scope_semi_ok(const ScopePlan *p);
extern bool sift_v6_scope_matches(ScopePlan *p, int ord);
/* A sub-select or CTE with some form that sets the pairs of scope key K: the first path with its predicate, on its own */
extern Query *sift_v6_match_two_paths(const Query *q, const SiftScopeLeg legs[2]);

/* plan.c */
extern int	sift_v6_scope_index_col(const ScopePlan *p, const Var *v);
extern Oid	sift_v6_scope_opfamily(const ScopePlan *p, int col);
extern AttrNumber sift_v6_scope_fact_attno(const ScopePlan *p, int col);
extern List *sift_v6_scope_spec_pack(const ScopeSpec *s);
extern ScopeSpec *sift_v6_scope_spec_unpack(const List *ints);

/* The paths of the scope aggregate over the relations of the format whose scopes are scope, for SiftScopeOps.upper_paths, .topk_paths and .one_key */
extern void sift_v6_scope_upper_paths(const SiftScopeOps *scope, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);
extern void sift_v6_scope_topk_paths(const SiftScopeOps *scope, PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel);
/* The scope of the index ix of the facts that summarizes the pairs relation the statement joins by member (fact column fact_att = pairs column pairs_att) and binds to one key: its ordinal and the expression of the key */
extern bool sift_v6_scope_one_key(const SiftScopeOps *scope, PlannerInfo *root, RelOptInfo *fact, RelOptInfo *pairs, IndexOptInfo *ix, AttrNumber fact_att, AttrNumber pairs_att, int *ord,
								  Expr **key);

/* show.c: the EXPLAIN lines of a node of the scope aggregate: its relation and spec, the expression of its keys (NULL: none named), the keys of its run (-1: not run), why it ran the join (NULL: it did not) */
typedef struct ScopeShow
{
	Relation	rel;
	const ScopeSpec *spec;
	const Expr *key;
	int64		nkeys;
	const char *fallback;
} ScopeShow;

struct ExplainState;
extern void sift_v6_scope_show(const ScopeShow *s, PlanState *ps, List *ancestors, struct ExplainState *es);

/* scan.c: the executor of the plans that scan the facts of the members of a key (ScopeSpec.scan), for every format */
extern PGDLLIMPORT const CustomScanMethods sift_v6_scope_scan_methods;

#endif							/* SIFT_AGG_SCOPE_SCOPE_H */
