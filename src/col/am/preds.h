/*
 * col/am/preds.h - the predicates of a scan of either format: the clauses of the index qual as predicates on leaves (col/am/preds.c)
 */
#ifndef SIFT_COL_PREDS_H
#define SIFT_COL_PREDS_H

#include "access/relscan.h"

#include "col/col.h"

/* What the keys of one scan need to know besides the keys */
typedef struct ColKeys
{
	const SiftColDef *defs;		/* the leaves of the index */
	const int  *first;			/* first[attno]: the leaf of attribute attno */
	TupleDesc	idesc;
	bool		infix;			/* a LIKE with no literal prefix but a literal run is a substring test of the leaf (the skipping indexes of format major 2 keep the three byte windows) */
	int			(*key_leaf) (const void *arg, int attno, const char *key, int len);	/* the leaf that answers jsonb_exists(document, key) (-1: none keeps it); NULL: the index cannot */
	const void *key_leaf_arg;
	bool		recheck;		/* a key replaced by IS NOT NULL because the leaf cannot test its constant exactly: the heap scan applies the clause */
} ColKeys;

/*
 * The strategies of the operator classes: btree's numbers (1 <, 2 <=, 3 =, 4 >=, 5 >) and, for text, 7 LIKE and 8 starts_with (^@). The default class also has the
 * containment operators of a range (10 @>, 11 <@, 12 &&, 13 <<, 14 >>, 15 &<, 16 &>, 17 -|-, 18 @> an element) and of a network (21 <<, 22 <<=, 23 >>, 24 >>=, 25 &&),
 * in the order of SIFT_COLW_R_* and SIFT_COLW_N_* (col/col.h), and 3 = for both.
 */
#define COL_STRAT_NE			6
#define COL_STRAT_LIKE			7

#define COL_STRAT_STARTS_WITH	8

/* The constant of a key as a predicate value of the leaf (an integer, date, timestamp, boolean, float, text, char(n) or uuid) */
extern void key_value(Oid argtype, int32 typmod, Datum d, SiftColValue *v);
extern bool xtime_pair(Oid coltype, Oid argtype);

extern int	xtime_bound(Oid coltype, Oid argtype, Datum d, int *strat, int64 *v);

/* A pattern with no literal prefix but a literal run of this many bytes is a substring test of the leaf when ColKeys.infix */
#define SIFT_LIKE_INFIX_MIN	3
extern void like_pred(ColKeys *k, SiftColPred *p, SiftColValue *v, bool starts_with, bool infix);

/* The predicates of the keys of a scan, one for each key, on the leaves of k; false: a key no leaf can decide and no recheck covers (the scan finds no row) */
extern bool sift_col_key_preds(const ScanKeyData *keys, int nkeys, ColKeys *k, SiftColPred *preds);

extern bool scan_preds(IndexScanDesc scan, ColKeys *k, SiftColPred *preds);

#endif
