/*
 * agg/v6/scope_v6_int.h - what the planning (scope_v6.c) and the executor (scope_v6_run.c) of the scope aggregate share: the statement as a spec the plan carries.
 */
#ifndef SIFT_AGG_V6_SCOPE_INT_H
#define SIFT_AGG_V6_SCOPE_INT_H

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
	Oid			pairs_rel;		/* scan: the relation of the pairs, its key and member attributes, and the index column of the member of the facts */
	int			pairs_key_att;
	int			pairs_member_att;
	int			member_col;
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

/* A relation of the plain kind the scope planner reads: a table without inheritance, sampling or security quals */
extern bool sift_v6_plain_table(const RangeTblEntry *rte);
extern List *sift_v6_scope_spec_pack(const ScopeSpec *s);
extern ScopeSpec *sift_v6_scope_spec_unpack(const List *ints);

/* The scope of the index ix of the facts that summarizes the pairs relation the statement joins by member (fact column fact_att = pairs column pairs_att) and binds to one key: its ordinal and the expression of the key */
extern bool sift_v6_scope_one_key(PlannerInfo *root, RelOptInfo *fact, RelOptInfo *pairs, IndexOptInfo *ix, AttrNumber fact_att, AttrNumber pairs_att, int *ord, Expr **key);

/* The cells of the leaves (leaves[0 .. nleaves - 1]) of the newest rows of a key under the order of the statement, from its ring and the rows the summary lacks; false: the join answers */
extern bool sift_v6_scope_topn(SiftF2Scope *sc, const ScopeSpec *sp, int64 key, bool no_key, int tie_leaf, const int *leaves, int nleaves, SiftF2ScopeCell **out, int *nout);

#endif							/* SIFT_AGG_V6_SCOPE_INT_H */
