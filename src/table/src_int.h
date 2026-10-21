/*
 * table/src_int.h - shared between the files of the column source of a sift_table (src.c and wide.c).
 */
#ifndef SIFT_TBL_SRC_INT_H
#define SIFT_TBL_SRC_INT_H

#include "nodes/bitmapset.h"
#include "agg/pre.h"
#include "agg/sel.h"
#include "agg/colsrc.h"
#include "table/table.h"

typedef struct TblLeafBuf
{
	size_t		cap;			/* elements the buffers hold */
	void	   *data;
	uint8	   *isnull;
	uint64	   *ovf;			/* the values the columns cannot hold: the NaNs of a numeric leaf, the values of a path of another type */
} TblLeafBuf;

/* A conjunct on a wide leaf: decided with the type's own comparison on the exact values */
typedef struct TblWide
{
	int			leaf;
	int			op;
	int			nvals;
	const Datum *vals;
	const bool *isnull;
	FmgrInfo   *cmp;
} TblWide;

/* The source of one column layout */
typedef struct TblPart
{
	SiftColSource *src;
	const SiftColSchema *schema;
	int		   *col;			/* [nleaves] the leaf of this schema that holds layout leaf i, -1 when it has none (a column added later) */
	int		   *leaf_of_col;	/* [schema->ncols] the layout leaf a leaf of this schema holds, -1 when the table dropped it */
	SiftColUnit unit;			/* the head of this source's units, valid when have */
	bool		have;
} TblPart;

typedef struct TblSource
{
	Relation	rel;
	SiftTblRel *st;
	Snapshot	snapshot;
	MemoryContext cxt;
	SiftDirSnapshot *dir;
	SiftTblVers *vers;			/* the groups of dir with unfrozen rows */
	SiftTblSeg *wbuf;			/* private copies of the write buffers */
	int			nwbuf;
	uint32		wbuf_lo;		/* the superblocks that hold write buffer rows: [wbuf_lo, wbuf_hi] */
	uint32		wbuf_hi;
	Bitmapset  *batched;		/* the superblocks whose write buffer rows are in the sources already */
	SiftSrcLayout *lay;
	TblPart    *part;
	int			nparts;
	int			last;			/* the part of the unit returned last, -1 at the start */
	SiftSrcDict *dict;			/* per leaf: the dictionary of the last read */
	SiftSrcDict *adict;			/* per leaf: the dictionary of its default in the segments that lack the column */
	TblLeafBuf *buf;			/* per leaf */
	SiftAggSel	sel;			/* the match set the units are restricted to */
	SiftAggPre	pre;			/* the histogram of the last unit_hist */
	TblWide    *wide;			/* the conjuncts on wide leaves of set_wide */
	int			nwide;
	bool	   *tol;			/* per leaf: its values the table cannot hold are read as NULL (tolerate_ovf) */
	MemoryContext *wcxt;		/* per wide leaf: the dictionary of its last read */
	uint64		wseq;			/* the id of the next wide dictionary */
} TblSource;

extern void wide_read(TblSource *t, int leaf, const TblPart *p, const SiftColVec *v, size_t n, Datum dflt, bool dnull, SiftSrcVec *vec);
extern TblLeafBuf *leaf_buf(TblSource *t, int leaf, SiftSrcVecType vt, size_t n);
extern int	tbl_set_wide(void *impl, const SiftSrcWideTest *t, int nt);
extern bool tbl_filter_wide(void *impl, const SiftSrcUnit *unit, int i, uint64 *rows);

#endif							/* SIFT_TBL_SRC_INT_H */
