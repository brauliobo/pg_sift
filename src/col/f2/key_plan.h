/*
 * col/f2/key_plan.h - what the predicates of a scan leave of the key space of an index, and the granules of a part that it can reach (the composite form of the lead column range of f2_prune.c).
 *
 * The plan walks the key columns in order while the predicates pin them: an equality or a short IN list (at most SIFT_KEY_PLAN_MAX_RANGES values together) extends the prefix, IS NULL extends it
 * with the NULL, and the first column that has only a range (<, <=, >, >=, BETWEEN, a longer IN list as its least and greatest value, a text prefix, IS NOT NULL) ends it. The result is at most
 * SIFT_KEY_PLAN_MAX_RANGES ranges of key bytes; a predicate the plan cannot use narrows nothing, which is always right: the heap or the later predicates decide the rows.
 *
 * A range holds the first key_bytes of its bounds, as a mark does, so that a granule is tested by comparing the bytes of its marks with them.
 */
#ifndef SIFT_KEY_PLAN_H
#define SIFT_KEY_PLAN_H

#include "core/host.h"

#include "col/f2/key_marks.h"

#define SIFT_KEY_PLAN_MAX_RANGES	16

typedef enum SiftKeyOp
{
	SIFT_KOP_EQ = 1,			/* vals[0] */
	SIFT_KOP_LT,
	SIFT_KOP_LE,
	SIFT_KOP_GT,
	SIFT_KOP_GE,
	SIFT_KOP_BETWEEN,			/* vals[0] <= x <= vals[1] */
	SIFT_KOP_IN,				/* nvals values */
	SIFT_KOP_ISNULL,
	SIFT_KOP_NOTNULL,
	SIFT_KOP_PREFIX				/* TEXT: the bytes of vals[0] are a prefix of the value */
} SiftKeyOp;

/* A predicate of one conjunct on key column col (its place in key_order); a predicate on a column that is none of them is not given */
typedef struct SiftKeyPred
{
	uint8		col;
	uint8		op;				/* SiftKeyOp */
	int			nvals;
	const SiftKeyVal *vals;
} SiftKeyPred;

/* [lo, hi): the images of the bounds; has_lo/has_hi false: open. hi_exact: hi was not cut, so a granule that starts at hi is outside the range */
typedef struct SiftMarkRange
{
	uint8		lo[SIFT_KEY_BYTES_MAX];
	uint8		hi[SIFT_KEY_BYTES_MAX];
	bool		has_lo;
	bool		has_hi;
	bool		hi_exact;
} SiftMarkRange;

typedef enum SiftKeyPlanState
{
	SIFT_KEY_PLAN_FULL,			/* the predicates leave the whole key space: every granule is read */
	SIFT_KEY_PLAN_EMPTY,		/* no row can satisfy them */
	SIFT_KEY_PLAN_RANGES
} SiftKeyPlanState;

typedef struct SiftKeyPlan
{
	SiftKeyPlanState state;
	int			n;
	SiftMarkRange range[SIFT_KEY_PLAN_MAX_RANGES];	/* ascending by lower bound */
	int			prefix_cols;	/* the columns the ranges pin by an equality: how deep the plan went */
	bool		range_col;		/* and the next one is confined by a range */
} SiftKeyPlan;

extern void sift_key_plan_build(const SiftKeySpec *spec, const SiftKeyPred *preds, int npreds, SiftKeyPlan *plan);

/*
 * (key column 0, .., n - 1) op (vals): the keys of the leading columns compared as a row, which is one range of the key bytes when the columns are ordered alike (op: SIFT_KOP_LT, LE, GT or GE in the order of the values); the
 * ranges of the plan are cut to it. A NULL value or columns ordered otherwise leave the plan as it is: the rows the range keeps are those the comparison decides.
 */
extern void sift_key_plan_row_bound(const SiftKeySpec *spec, SiftKeyPlan *plan, int op, const SiftKeyVal *vals, int n);

/* Whether a key (key_bytes of an encoded tuple) is inside the range: the check of a plan against the rows */
extern bool sift_mark_range_has(const SiftMarkRange *r, const uint8 *key, uint8 key_bytes);

/*
 * The granules of the part that the ranges of the plan can reach, as ascending disjoint spans [spans[i][0], spans[i][1]): each range is two binary searches over the marks, the first granule whose
 * last key is not below the range and the first whose first key is above it. Returns the number of spans (at most SIFT_KEY_PLAN_MAX_RANGES). A plan that is FULL gives the whole part, an EMPTY one none.
 */
extern int	sift_key_marks_spans(const SiftKeyMarks *km, const SiftKeyPlan *plan, uint32 spans[][2]);

#endif							/* SIFT_KEY_PLAN_H */
