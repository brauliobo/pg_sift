/*
 * agg/flat/int.h - what the files of agg/flat/ share
 * The types and the functions the pull-up, the grouping of aggregate laterals and the
 * decorrelation call in one another.
 */
#ifndef SIFT_AGG_FLAT_DIR_INT_H
#define SIFT_AGG_FLAT_DIR_INT_H

#include "agg/plan_int.h"

/* a pulled-up sub-select: the range table entry its columns are read through, and the expressions that compute them */
typedef struct Pulled
{
	Index		rti;
	RangeTblEntry *rte;
	List	   *targetlist;
} Pulled;

/*
 * A sub-select of aggregates over one table, tied to the query around it by equalities of columns (`SELECT sum(f.a) FROM f WHERE f.k = x.k AND ..`, a LATERAL
 * sub-select or a scalar sublink), is `f GROUP BY k` joined to x by k. The rows of x do not change: the groups hold each k once, a NULL k matches no
 * group (as no row of f equals it) and a count of the rows a k does not have is 0 (every other aggregate is NULL). The planner hook plans the query both ways
 * and keeps this one if it runs in the scan and costs less: an index lookup for each of a few rows of x stays what it was.
 */
typedef struct Decor
{
	Query	   *sub;
	List	   *conds;			/* the equalities: the table's column and the outer query's */
	List	   *rest;			/* the other conditions of the sub-select */
	Bitmapset  *outer;			/* the range table entries of the outer query it reads */
	List	   *groups;			/* the expressions it groups by (a LATERAL one, which gives a row for each group, not one row) */
	bool		exists;			/* EXISTS (SELECT .. FROM f WHERE f.k = x.k AND ..): whether a row is there, whatever it holds */
} Decor;

/* a lateral sub-select to replace (the replacement waits for the joins of the level to be spliced, as for a pull-up) */
typedef struct DecorJoin
{
	Index		lrti;
	Decor		d;
} DecorJoin;

extern List *flat_levels;

extern bool true_or_missing(Node *quals);
extern Node * read_columns(Node *n, Pulled *p);
extern void group_by(Query *q, TargetEntry *te);
extern bool group_aggregate_lateral(Query *q, bool apply);

extern bool decorrelate_join(Query *q, JoinExpr *j, bool apply, List **decor);
extern void decor_apply(Query *q, DecorJoin *r);
extern bool decorrelate_sublinks(Query *q, bool apply);
extern bool decorrelate_comma(Query *q, FromExpr *f, bool apply, List **decor);

#endif							/* SIFT_AGG_FLAT_DIR_INT_H */
