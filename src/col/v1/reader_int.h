/*
 * col/v1/reader_int.h - the state of the index reader (col/v1/reader.c), which the aggregate engine's source of an index starts with
 */
#ifndef SIFT_COL_V1_V1_READER_INT_H
#define SIFT_COL_V1_V1_READER_INT_H

#include "col/col.h"
#include "col/v1/src_sel.h"
#include "col/v1/src_wide.h"
#include "col/v1/src_zone.h"
#include "col/v1/reader.h"

typedef struct SiftV1Idx
{
	SiftColSource *cs;
	SiftColUnit unit;
	SiftSrcLayout *lay;
	MemoryContext cxt;
	SiftSrcDict *dict;			/* per leaf: the dictionary of the last read */
	MemoryContext hcx;			/* the arrays of the last histogram, and its dictionary */
	SiftSrcDict hdict;
	SiftSrcLeafBuf *buf;		/* per leaf */
	bool	   *tol;			/* per leaf: its values the index cannot hold are read as NULL */
	bool	   *raw;			/* per leaf: an image leaf read as the codes of its stored dictionary */
	Relation	heap;
	Relation	index;
	Snapshot	snapshot;
	SiftSrcSel	sel;			/* the match set the units are restricted to */
	SiftSrcZoneTests zones;		/* the zone tests of the prune entries of the last begin */
	SiftSrcWide *wide;			/* per leaf: the dictionary of a wide leaf in the unit (wgen: the unit it was made for) */
	uint64	   *wgen;
	uint64		ugen;			/* the unit the reader is at, counted */
	uint64		wseq;			/* dictionaries made, for their ids */
	/* makes the dictionary of a wide leaf from its lanes into w and r->dict[leaf]: sift_v1_idx_wide_dict, or a reader's own for the leaves it reads another way */
	void		(*wide_dict) (struct SiftV1Idx *r, int leaf, SiftSrcWide *w, const SiftColVec *v, uint32 n);
} SiftV1Idx;

/* a leaf read through the per-unit dictionary of datums (col/v1/src_wide.h) */
static inline bool
sift_v1_idx_wide(const SiftV1Idx *r, int leaf)
{
	return r->lay->leaf[leaf].wide && !r->raw[leaf];
}


#endif							/* SIFT_COL_V1_V1_READER_INT_H */
