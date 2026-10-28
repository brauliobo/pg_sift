/*
 * agg/pred_int.h - the predicates of the scan (agg/pred.c)
 */
#ifndef SIFT_AGG_PRED_INT_H
#define SIFT_AGG_PRED_INT_H

#include "agg/agg_fwd_int.h"
#include "core/i64set.h"
#include "agg/leaf_int.h"
#include "agg/tup_int.h"
#include "agg/zoneoff.h"

typedef struct PredState
{
	bool		ready;
	bool		cnull;
	int64		ci;
	double		cd;
	bool		in_hasnull;
	bool		in_hasnan;		/* a numeric list with a NaN: the rows the leaf stores as overflow (its NaNs) are in the set */
	bool		in_dense;		/* the elements are every value of [in_lo, in_lo + in_nlut): the interval of the zone holds nothing else */
	bool		in_empty;		/* the array has no element: x NOT IN () holds for every row, a NULL x too */
	int64		in_lo;			/* the least and the greatest element, when there is one (in_nv) */
	int64		in_hi;
	uint32		in_nv;
	uint32		in_nlut;
	uint8	   *in_lut;			/* a table over [in_lo, in_lo + in_nlut) ... */
	int64	   *in_vals;		/* ... with the elements sorted and distinct, in_nvals of them, when there are at most ZONE_SET_CAP: no walk of a wide table to list them */
	uint32		in_nvals;
	uint64	   *in_bits;		/* ... or, when the range is too wide for a byte table, a bit for each value of [in_lo, in_lo + in_nbits) ... */
	uint64		in_nbits;
	SiftI64Set	in_hset;		/* ... or, when the values are too far apart for that, a hash table of them (.slot is NULL otherwise) */
	ExprState  *expr;			/* SAP_LUT, SAP_STEP */
	Param	   *param;
	bool		null_result;	/* SAP_LUT, SAP_STEP: the expression for a NULL input */
	const char *eq_s;			/* SAP_LUT: the expression is `$0 = 'literal'` of a text with a deterministic collation: an entry is true when its bytes are eq_s (eq_len of them) */
	int			eq_len;
	int			elem;			/* 1 + the index of the element test of an EXISTS over a list that the source may answer exactly (str_init), else 0 */
	SiftSrcStrTest *str;		/* a row level text test of the query, which the source runs on the encoded form (str_init) */
	RangeTab	rt;				/* SAP_LUT on an integer leaf, SAP_STEP: result bytes */
	ZoneOff    *step_zone;		/* SAP_STEP: the zone of the steps (NULL for UTC) */
	int64		step_us;		/* SAP_STEP: microseconds of a step (1: each value on its own) */
	uint32	   *step_iota;		/* SAP_STEP: 0 .. SIFT_AGG_VEC - 1, the rows of a stretch */
	uint64	   *rowmask;		/* element queries, a row predicate: the unit's rows */
	bool		rowmask_ok;
	int			nop;			/* numeric operand: the comparison of the scaled integers equivalent to the query's (SiftCmpOp) ... */
	sift_i128	cn;				/* ... with this operand, or ... */
	int			konst;			/* ... -1, or 1 / 0: it holds for every / no value */
	bool		nan_res;		/* numeric operand: the comparison holds for a NaN (numeric orders it above every number and equal to itself) */
	uint32		memo_row;		/* SAP_ELEM_EXISTS (valid while rowmask_ok): the answer for this row of the unit and these lookup rows */
	uint32		memo_ref[SIFT_AGG_MAX_CLEAVES];
	bool		memo_res;
	uint64	   *poison;			/* SAP_ELEM_EXISTS with pass_poison (valid while poison_ok): the rows of the unit whose list SQL cannot read */
	bool		poison_ok;
} PredState;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* pred.c */
void in_build(PredState *st, const int64 *vals, int nv, int64 lo, int64 hi);
void eval_operand(AggExec *ex, SiftAggPred *p, PredState *st);
void rows_of_elems(const uint32 *offs, int nrow, const uint64 *eb, uint64 *out);
uint64 *row_bitmap(AggExec *ex, SiftAggPred *p);
void eval_pred(AggExec *ex, SiftAggPred *p, int qlevel, int start, int n, uint64 *out);
bool elem_exists(AggExec *ex, const Tup *t, int j, const SiftAggPred *p);

#pragma GCC visibility pop

#endif
