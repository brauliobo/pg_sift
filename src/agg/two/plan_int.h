/*
 * agg/two/plan_int.h - the state of the planning of one two-level aggregate, shared by agg/two/plan.c (the arms, the outer aggregate, the path) and agg/two/label.c (what EXPLAIN says).
 */
#ifndef SIFT_AGG_TWO_PLAN_INT_H
#define SIFT_AGG_TWO_PLAN_INT_H

#include "postgres.h"

#include "nodes/pathnodes.h"

#include "agg/engine.h"
#include "agg/fuse/fuse.h"

typedef struct TwoArm
{
	RelOptInfo *rel;			/* the sub-select of the arm in the outer statement */
	PlannerInfo *sub;
	List	   *cols;			/* of each column of the outer FROM item, the expression of the arm's target list it is */
	Node	   *id;				/* the group expression of the arm that is the unique row id */
	Path	   *path;			/* the subquery scan of the arm over its SiftAggV6 path, whose tuple is a cell */
} TwoArm;

/* A relation the outer statement joins the arms to (spine JOIN arms, spine LEFT JOIN arms) on equalities of an expression of the arms and one of the spine */
typedef struct TwoSpine
{
	RelOptInfo *rel;			/* NULL: no spine */
	Path	   *path;
	bool		left;			/* the spine rows no cell matches are a row of NULL arm columns each */
	List	   *cell_keys;		/* the expressions of the arms (the FROM item) of the equalities */
	List	   *spine_keys;		/* ... and those of the spine */
	List	   *ops;			/* the equality operator of each, its left operand the cell's */
	List	   *colls;
	List	   *null_safe;		/* IS NOT DISTINCT FROM: a NULL equals a NULL */
	List	   *match_quals;	/* the other conditions of the join: a cell and a spine row match when they hold */
	List	   *post_quals;		/* the WHERE above a LEFT JOIN: tested on each joined row */
} TwoSpine;

typedef struct TwoPlan
{
	PlannerInfo *root;
	RelOptInfo *input;			/* the input of the outer aggregate: the FROM item, or its join to the spine */
	RelOptInfo *rel;			/* the FROM item of the arms */
	Index		srti;
	TwoSpine	spine;
	int			ncols;
	int			narms;
	TwoArm		arm[SIFT_V6_FUSE_MAX_ARMS];
	SiftV6Spec	spec;			/* the outer aggregate: its columns are the inputs */
	List	   *inputs;			/* the expression of each input over the columns of the FROM item */
	List	   *scan_tlist;		/* the keys, then the aggregates */
	List	   *group_exprs;
	bool		sorted;			/* the groups can leave in the order of the GROUP BY: no grouping sets, and a sort operator for every key */
	List	   *sort;			/* of each key of the GROUP BY: its column of the group tuple, sort operator, collation and nulls_first */
	char	   *why;			/* the first reason the statement is no two-level aggregate */
	bool		candidate;		/* an arm is a SiftAggV6 aggregate: a refusal of the arms is worth reporting (a statement whose arms the scan does not answer names none) */
} TwoPlan;

/* The FROM item of the arms and the spine of the input of the outer aggregate (agg/two/spine.c): false when it is neither a sub-select nor its join to one relation on equalities */
extern bool sift_two_from(TwoPlan *t);
/* the Vars of e are columns of the FROM item (side 0) or of the spine (side 1); -1: of both, of neither, or of another relation */
extern int	sift_two_side(const TwoPlan *t, Node *e);

/*
 * n with each placeholder the node computes as its expression: one that no outer join nulls (the grouping sets and pulled-up sub-selects mark them), or a strict expression of the
 * arms' columns, which is NULL on a row of the spine no cell matched as the placeholder is
 */
extern Node *sift_two_phvs(Node *n, TwoPlan *t);
/* an expression the node computes over a cell and a spine row: their columns, no aggregate, sub-plan, parameter of another node or volatile function (placeholders stripped first) */
extern bool sift_two_cell_expr(TwoPlan *t, Node *e);
/* n has a placeholder (an expression walker) */
extern bool sift_two_has_phv(Node *n, void *context);

/* The labels of EXPLAIN (agg/two/int.h TwoLabel) of the planned aggregate, as String nodes */
extern List *sift_two_labels(const TwoPlan *t);

#endif							/* SIFT_AGG_TWO_PLAN_INT_H */
