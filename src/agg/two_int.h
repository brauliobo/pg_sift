/*
 * agg/two_int.h - two level aggregation: the arms (agg/two.c)
 */
#ifndef SIFT_AGG_TWO_INT_H
#define SIFT_AGG_TWO_INT_H

#include "agg/agg_fwd_int.h"
#include "agg/tup_int.h"

/* the state of one arm of a two level query (SiftAggArm): per tuple of the chunk, and per outer group the inner aggregates of the current row */
typedef struct ArmSt
{
	uint32	   *gid;
	uint8	   *ok;				/* the arm's filters hold */
	ExprState  *hstate;			/* the rest of the arm's HAVING */
	uint8	  **iok;			/* per inner aggregate: it takes the tuple */
	double	  **dx;				/* its float8 value */
	int64	  **ix;				/* its integer value */
	uint64	   *ord;			/* per outer group: the row its cell holds, UINT64_MAX none */
	int64	  **n;				/* per inner aggregate and group */
	int64	  **i;
	double	  **d;
	uint8	  **nf;
	double	   *amax;			/* per inner aggregate: the largest absolute float input */
} ArmSt;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* two.c */
void arms_resize(AggExec *ex, uint32 newcap, const uint32 *remap, uint32 oldcap);
void arms_init(AggExec *ex);
void arms_flush(AggExec *ex);
void arm_chunk(AggExec *ex, Tup *t);

#pragma GCC visibility pop

#endif
