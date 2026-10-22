/*
 * agg/two_int.h - two level aggregation: the arms (agg/two.c)
 */
#ifndef SIFT_AGG_TWO_INT_H
#define SIFT_AGG_TWO_INT_H

#include "agg/agg_fwd_int.h"
#include "agg/leaf_int.h"
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
	int64	  **n;				/* per inner aggregate and group (local: and cell) */
	int64	  **i;
	double	  **d;
	uint8	  **nf;
	double	   *amax;			/* per inner aggregate: the largest absolute float input */
	/*
	 * A local arm has hidden keys: its cells are finer than the outer groups and live for one row, so they are numbered in the row (a small
	 * table of the key ids of its cells) and not in the group table, which then holds the outer groups only.
	 */
	bool		local;
	int			nlk;			/* the keys of a cell: the arm's outer and hidden keys, lk[] */
	int			lk[SIFT_AGG_MAX_KEYS];
	int8		lpos[SIFT_AGG_MAX_KEYS];	/* per key: its place in lk, -1 not a key of the arm */
	uint32	   *kid;			/* per key and tuple of the chunk: the key's id */
	uint32		nc;				/* the cells of the row */
	uint32		ccap;
	uint32	   *cid;			/* per cell: the ids of its keys, nlk each */
	uint32		lcap;			/* the slots of the row's table of cells, a power of 2 */
	uint64	   *ltab;			/* a slot: the row's stamp << 32 | cell + 1, free when the stamp is not the row's */
	uint32		stamp;
} ArmSt;

/* the join keys of an arm's cells that were seen, with the spine rows each equals */
typedef struct SpineMemo
{
	uint32		n;
	uint32		cap;
	uint32		nslot;
	uint32	   *slot;			/* entry + 1, 0 free */
	uint32	   *ids;			/* per entry: the arm's key ids of the pairs of the join */
	uint32	   *first;			/* ... and its rows: SpineSt.match[first .. first + count) */
	uint32	   *count;
} SpineMemo;

/* the rows of the spine of a two level aggregate (SiftAggSpine), read when the scan starts */
typedef struct SpineSt
{
	uint32		nrow;
	uint32	   *oid;			/* [row * nout + o]: the id of the row's column outcol[o] in key outkey[o] */
	uint32	   *jid;			/* [row * npair + p]: the id of its value of pair p in jot[p], 0 for NULL */
	bool	   *hit;			/* the row joined a cell */
	OutTable	jot[SIFT_AGG_MAX_JKEYS];
	SpineMemo	memo[SIFT_AGG_MAX_ARMS];
	uint32	   *match;
	uint32		nmatch;
	uint32		cmatch;
} SpineSt;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* two.c */
void arms_resize(AggExec *ex, uint32 newcap, const uint32 *remap, uint32 oldcap);
void arms_init(AggExec *ex);
void arms_flush(AggExec *ex);
void arm_chunk(AggExec *ex, Tup *t);
void spine_tail(AggExec *ex);

/* spine.c */
void spine_load(AggExec *ex);
uint32 spine_rows(AggExec *ex, int k, const uint32 *ids, const uint32 **rows);

#pragma GCC visibility pop

#endif
