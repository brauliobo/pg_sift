/*
 * agg/lookcol_int.h - what the loads of a lookup table from the leaves of a sift_col index share: lookcol.c reads the rows, lookclass.c the classes of the keys
 */
#ifndef SIFT_AGG_LOOKCOL_INT_H
#define SIFT_AGG_LOOKCOL_INT_H

#include "agg/agg_int.h"
#include "agg/lookup/int.h"

typedef struct ColNeed
{
	int			att;			/* the column of the table; 0 for an expression */
	Node	   *expr;			/* the column of the join that is an expression of the table's columns (SiftAggJoin.colexpr), whose leaf is read */
	int			xcol;			/* ... and the column of the join it is; -1 for a column of the table */
	bool		feeder;			/* read only to compute an expression column (CompCol), not a column of the join */
	const SiftSrcLeaf *lf;
	SiftSrcVec	vec;
	Datum	   *dval;			/* a text leaf: the value of each entry of the unit's dictionary made so far */
	bool	   *dmade;
	uint32	   *dids;			/* ... and the id of its string among the classes' (lookclass.c), 0 not made yet */
	uint32		dn;
	MemoryContext cx;			/* the unit's: reset with it */
} ColNeed;

/* a column of the join that is an expression of the table's columns no index holds: computed from the leaves of the columns it reads, for each row */
typedef struct CompCol
{
	Node	   *expr;
	int			xcol;			/* the column of the join it is */
	ExprState  *st;
} CompCol;

#define MAX_NEED	(SIFT_AGG_MAX_JKEYS + 2 * SIFT_AGG_MAX_JCOLS)

/* what a unit reader hands to its callback: the live and visible rows of the unit (bit i of rows) and the decoded leaves of need[]; ux lives for the unit */
typedef void (*UnitFn) (void *arg, const SiftSrcUnit *unit, ColNeed *need, int nneed, uint64 *rows);

extern Relation lookcol_load_index(Relation rel, const SiftAggJoin *j, bool own, ColNeed *need, int *nneed, CompCol *comp, int *ncomp);
extern bool lookcol_read_units(AggExec *ex, Relation rel, Relation ix, ColNeed *need, int nneed, UnitFn fn, void *arg);
extern Datum lookcol_cell_datum(ColNeed *nd, int64 i, bool *isnull);
extern int64 lookcol_scalar_value(const ColNeed *nd, int64 i);
extern Datum lookcol_scalar_datum(const SiftSrcLeaf *lf, int64 v);

#endif
