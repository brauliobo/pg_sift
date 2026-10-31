/*
 * bm25/attr.h - the values of the INCLUDE columns of a sift index as the ranker tests them (bm25/segment.h describes how a segment holds them).
 */
#ifndef SIFT_ATTR_H
#define SIFT_ATTR_H

#include "core/host.h"

enum SiftAttrState
{
	SIFT_AV_VALUE = 0,
	SIFT_AV_NULL,
	SIFT_AV_UNKNOWN
};

/* A test of one INCLUDE column that the ranker applies to the docs of a block before it scores them (a doc whose value is unknown passes every test) */
enum SiftAttrOp
{
	SIFT_AT_EQ,
	SIFT_AT_NE,
	SIFT_AT_LT,
	SIFT_AT_LE,
	SIFT_AT_GT,
	SIFT_AT_GE,
	SIFT_AT_IN,					/* v is in set (nset ascending values) */
	SIFT_AT_ISNULL,
	SIFT_AT_NOTNULL
};

typedef struct SiftAttrTest
{
	int			col;
	int			op;
	int64		v;
	const int64 *set;
	int			nset;
} SiftAttrTest;

/* Does a doc whose column holds state (and v for SIFT_AV_VALUE) pass the test? A comparison is never true of NULL */
static inline bool
sift_attr_test_pass(const SiftAttrTest *t, int state, int64 v)
{
	if (state == SIFT_AV_UNKNOWN)
		return true;
	if (t->op == SIFT_AT_ISNULL)
		return state == SIFT_AV_NULL;
	if (state == SIFT_AV_NULL)
		return false;
	switch (t->op)
	{
		case SIFT_AT_EQ:
			return v == t->v;
		case SIFT_AT_NE:
			return v != t->v;
		case SIFT_AT_LT:
			return v < t->v;
		case SIFT_AT_LE:
			return v <= t->v;
		case SIFT_AT_GT:
			return v > t->v;
		case SIFT_AT_GE:
			return v >= t->v;
		case SIFT_AT_IN:
			{
				int			lo = 0,
							hi = t->nset;

				while (lo < hi)
				{
					int			mid = lo + (hi - lo) / 2;

					if (t->set[mid] < v)
						lo = mid + 1;
					else
						hi = mid;
				}
				return lo < t->nset && t->set[lo] == v;
			}
		default:
			return true;		/* SIFT_AT_NOTNULL */
	}
}

#endif							/* SIFT_ATTR_H */
