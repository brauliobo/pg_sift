/*
 * agg/filter/eval.h - the clauses of FILTER (WHERE ..) over the vectors of a batch: a clause is a conjunction of column-and-constant predicates (the forms the aggregate scan already takes
 * as quals: a comparison, IN of constants, IS [NOT] NULL, a text LIKE a pattern of no `_` and no `%` inside), and its rows are a bitmap (v6_filter_pivot.h evaluates each distinct clause once for a batch and shares the bitmap among the aggregates).
 *
 * A comparison with a NULL is not true; IS NULL looks at the validity only. A float8 compares as PostgreSQL does (NaN is above every number and equal to itself); a text compares by its bytes, which
 * the planner allows only for a collation whose equality is the bytes'. A pred holds its constant in the form of the column it is for (the integer of an int2, int4, int8, date, timestamp or boolean
 * column, a double, the bytes of a string): the planner has already refused a constant of a type whose comparison with the column is not that.
 */
#ifndef SIFT_AGG_V6_FILTER_EVAL_H
#define SIFT_AGG_V6_FILTER_EVAL_H

#include "core/host.h"

#include "agg/vec.h"
#include "col/like/simd.h"

#define SIFT_CLAUSE_MAX_PREDS	16

typedef enum SiftPredOp
{
	SPO_EQ,
	SPO_NE,
	SPO_LT,
	SPO_LE,
	SPO_GT,
	SPO_GE,
	SPO_IN,						/* equal to one of the constants */
	SPO_IS_NULL,
	SPO_NOT_NULL,
	SPO_NOT_IN,					/* equal to none of the constants (no NULL among them) */
	SPO_LIKE					/* a text that LIKE matches: the pattern of the shape like whose literal is s (needle) */
} SiftPredOp;

typedef enum SiftPredKind
{
	SPK_INT,
	SPK_F64,
	SPK_STR
} SiftPredKind;

typedef struct SiftPredStr
{
	const uint8 *bytes;
	uint32		len;
} SiftPredStr;

typedef struct SiftPred
{
	SiftPredOp	op;
	SiftPredKind kind;
	int			group;			/* 0: a conjunct of the clause; n: one of the consecutive arms of the disjunction n, which is a conjunct (the rows of a pred are the ones it is true for, so an OR is their union) */
	int			vec;			/* the column: its index in the vectors given to sift_clause_eval */
	int64		i;
	double		f;
	SiftPredStr s;
	SiftLikeShape like;
	SiftLikeNeedle needle;
	uint32		nin;			/* SPO_IN */
	const int64 *in_i;
	const double *in_f;
	const SiftPredStr *in_s;
} SiftPred;

typedef struct SiftClause
{
	int			npred;
	SiftPred	pred[SIFT_CLAUSE_MAX_PREDS];
} SiftClause;

/* The rows of 0 .. n_raw - 1 for which every pred is true, into out (SIFT_VEC_WORDS words); the bits past n_raw are zero */
extern void sift_clause_eval(const SiftClause *c, SiftVec *const *vecs, uint32 n_raw, uint64 *out);

/* One pred of the clause on its own: its rows into out */
extern void sift_pred_eval(const SiftPred *p, const SiftVec *v, uint32 n_raw, uint64 *out);

/* The comparison of PostgreSQL's float8: NaN above every number and equal to NaN */
extern int sift_f8_cmp(double a, double b);

#endif							/* SIFT_AGG_V6_FILTER_EVAL_H */
