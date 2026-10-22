/*
 * agg/stub_src_int.h - what stub_src.c and stub_dec.c share: the unit and its decoded leaves
 */
#ifndef SIFT_AGG_STUB_SRC_INT_H
#define SIFT_AGG_STUB_SRC_INT_H

#include "agg/stub.h"

#include "agg/colsrc.h"

typedef struct StubVecSlot
{
	SiftSrcVec	vec;
	SiftSrcDict dict;
	bool		done;
} StubVecSlot;

/* The index datums of nrow rows (row-major, natts per row) and what has been decoded of them */
typedef struct StubUnit
{
	int			nrow;
	int			natts;
	Datum	   *vals;
	bool	   *nulls;
	StubVecSlot slot[SIFT_COLSRC_MAX_LEAVES];
} StubUnit;

typedef struct StubDecode
{
	const StubLayout *lay;
	MemoryContext cxt;			/* the decoded arrays */
	MemoryContext tmp;			/* the conversions of one row, reset per row */
	MemoryContext idcxt;		/* dictionary ids: unique for the life of the source */
	ErrorSaveContext *esc;		/* statistics: a value that cannot be converted is NULL instead of an error */
	char	   *idnext;
	int			idleft;
	int64		bytes;			/* decoded */
} StubDecode;

extern void *alloc_vec(MemoryContext cxt, Size bytes);
extern const StubVecSlot *leaf_slot(StubDecode *dc, StubUnit *u, int leaf);

#endif							/* SIFT_AGG_STUB_SRC_INT_H */
