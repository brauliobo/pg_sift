/*
 * col/amscan/amscan_int.h - what the files of the index scan share
 * the statics one file of the scan calls from another
 */
#ifndef SIFT_COL_AMSCAN_INT_H
#define SIFT_COL_AMSCAN_INT_H

#include "col/point.h"

/* What the keys of one scan need to know besides the keys */
typedef struct ColKeys
{
	const SiftColDef *defs;		/* the leaves of the index */
	const int  *first;			/* first[attno]: the leaf of attribute attno */
	TupleDesc	idesc;
	bool		recheck;		/* a key replaced by IS NOT NULL because the leaf cannot test its constant exactly: the heap scan applies the clause */
} ColKeys;

/*
 * The strategies of the operator classes: btree's numbers (1 <, 2 <=, 3 =, 4 >=, 5 >) and, for text, 7 LIKE and 8 starts_with (^@). The default class also has the
 * containment operators of a range (10 @>, 11 <@, 12 &&, 13 <<, 14 >>, 15 &<, 16 &>, 17 -|-, 18 @> an element) and of a network (21 <<, 22 <<=, 23 >>, 24 >>=, 25 &&),
 * in the order of SIFT_COLW_R_* and SIFT_COLW_N_* (col/col.h), and 3 = for both.
 */
#define COL_STRAT_LIKE			7

#define COL_STRAT_STARTS_WITH	8

extern bool	xtime_pair(Oid coltype, Oid argtype);

extern int	xtime_bound(Oid coltype, Oid argtype, Datum d, int *strat, int64 *v);

extern void	like_pred(ColKeys *k, SiftColPred *p, SiftColValue *v, bool starts_with);

extern bool	scan_preds(IndexScanDesc scan, ColKeys *k, SiftColPred *preds);

extern double	zones_kept(PlannerInfo *root, IndexPath *path, Relation irel);

extern double	sets_kept(PlannerInfo *root, IndexPath *path, Relation irel);

extern double	point_kept(PlannerInfo *root, IndexPath *path, Relation irel, double units, int *nprobes, double *tid, double *extra);

#endif