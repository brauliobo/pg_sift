/*
 * agg/v6/plan_int.h - what plan.c (a table) and plan_part.c (a table of partitions) of SiftAggV6 share: the state of the plan of one index, and the compile of a statement against it.
 */
#ifndef SIFT_AGG_V6_PLAN_INT_H
#define SIFT_AGG_V6_PLAN_INT_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/v6/v6.h"
#include "agg/v6/v6_plan_shape.h"
#include "col/am.h"
#include "col/f2/f2_store.h"

typedef struct V6Plan
{
	PlannerInfo *root;
	RelOptInfo *rel;
	RelOptInfo *out;
	Oid			relid;			/* the relation scanned: the index, or the table */
	IndexOptInfo *index;		/* a sift_col: its index; NULL for a sift_table */
	const SiftColRel *colrel;
	const SiftF2Rel *f2;
	SiftF2Store *store;			/* a sift_table: its store, whose attributes are those of the table */
	SiftV6Spec	spec;
	int			cap_aggs;		/* the room of spec.agg and spec.func */
	int			cap_funcs;
	List	   *scan_tlist;
	Bitmapset  *scan_vars;		/* the entries of scan_tlist that are plain columns (a Var), by varno */
	SiftFilterReg *freg;		/* the distinct FILTER clauses of the statement */
	bool		in_filter;		/* compiling a FILTER clause: its operands are constants, which the clause folds into its bitmaps at plan time */
	struct SiftV6Ext *xseed;	/* the dimensions or the unnest the join of the statement makes of its relations (v6_ext.h); NULL for a scan of the fact relation alone */
	List	   *dim_rels;		/* the RelOptInfo of each dimension, in the order of xseed->dim */
	RelOptInfo *elem_rel;		/* the relation of the unnest */
	bool		score_ok;		/* an aggregate over sift.score(ctid): the scan that brings its rows brings their scores (agg/hybrid_plan.c) */
	IndexOptInfo *text;			/* the sift index of the text query of the statement, if it has one (v6_text.c) */
	RestrictInfo *text_qual;
	Const	   *text_query;
} V6Plan;

/* An index of format major 2 with no expression; usable also has no predicate (a partial index serves a statement that implies it, which only plan.c judges) */
extern bool sift_v6_index_owned(const IndexOptInfo *ix);
extern bool sift_v6_index_usable(const IndexOptInfo *ix);

/* Compiles the keys, aggregates and quals of the statement against p->index; false when the index does not hold what they read */
extern bool sift_v6_compile_on(V6Plan *p, GroupPathExtraData *extra, List *group_exprs);

/* Compiles the keys, sets and aggregates of the statement, not its quals, against p->index; the caller's scan brings the rows (plan_hybrid.c) */
extern bool sift_v6_compile_head(V6Plan *p, GroupPathExtraData *extra, List *group_exprs);

/* plan_hybrid.c: sift.score(ctid) as the argument of an aggregate when the scan brings scores; the text query (body @@> 'q') among the quals; the statement is of the shape the node answers */
extern bool sift_v6_score_col(V6Plan *p, const Expr *e, SiftV6Col *out);
extern bool sift_v6_text_qual(V6Plan *p, RestrictInfo *ri);
extern bool sift_v6_compile_text(V6Plan *p);
extern bool sift_v6_plan_shape(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel);
extern List *sift_v6_group_exprs_of(PlannerInfo *root);

/* The path of the plan for the output relation; a spec with parts reads each of their indexes into the same groups */
extern void sift_v6_add_group_path(V6Plan *p, RelOptInfo *output_rel, List *group_exprs, List *having);

/* The relation of the plan as the scan reads it, and the operator family of an attribute of the host */
extern SiftV6Rel sift_v6_plan_rel(const V6Plan *p);
extern Oid sift_v6_plan_opfamily(const V6Plan *p, int att);

/* plan_qual.c: the quals of the index scan; the index holds every row the statement reads; the quals of the statement compiled into p->spec (false: the scan cannot decide one) */
extern Expr *sift_v6_index_qual(V6Plan *p, Expr *clause);
extern bool sift_v6_index_complete(const V6Plan *p);
extern bool sift_v6_compile_quals(V6Plan *p);

/* A column of the fact relation that its index holds as a plain leaf of the class of its type */
extern bool sift_v6_plan_column(V6Plan *p, const Expr *e, bool numeric_ok, SiftV6Col *out);
/* The statement is one the node may answer, whatever its relations; and the relation is a plain table the node may read (no inheritance, sampling or security quals) */
extern bool sift_v6_plan_query_ok(PlannerInfo *root, RelOptInfo *output_rel);
extern bool sift_v6_plan_fact_ok(PlannerInfo *root, RelOptInfo *rel);
/* The base relations of a join relation: its relids without the joins of the query */
extern Relids sift_v6_plan_base_relids(PlannerInfo *root, RelOptInfo *rel);
/* The path of a join: child_paths are the plans of the dimensions, whose cost and that of the join come on top of the scan's */
extern void sift_v6_add_join_path(V6Plan *p, List *group_exprs, List *having, List *child_paths, Cost extra_startup, Cost extra_total, double input_rows);
/* sift.agg_force: the price of a path of the node against the normal plan's (agg/plan_join.c) */
extern void agg_force_price(Path *p);

/* The columns the planners of joins and unnests answer for plan.c: false when the expression is not one of theirs */
extern bool sift_v6_star_column(V6Plan *p, const Expr *e, SiftV6Col *out);
extern bool sift_v6_unnest_column(V6Plan *p, const Expr *e, SiftV6Col *out);
extern void sift_v6_star_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);
extern void sift_v6_unnest_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra);

/* The paths for a partitioned table: one SiftAggV6 over the live partitions, whose indexes all lay the statement out alike */
extern void sift_agg_v6_partition_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra, List *group_exprs);

#endif							/* SIFT_AGG_V6_PLAN_INT_H */
