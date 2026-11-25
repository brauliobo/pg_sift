/*
 * gscan_int.h - what the files of the group scan (gscan*.c) share: its types and the functions that cross them.
 */
#ifndef SIFT_TABLE_GSCAN_INT_H
#define SIFT_TABLE_GSCAN_INT_H

#include "postgres.h"

#include "utils/datum.h"

#include "col/v1/seg/seg.h"
#include "table/v1/table.h"
#include "table/v1/seg_int.h"
#include "core/cmp.h"
#include "core/memtrack.h"

#define WIN_MIN_ROWS	256			/* rows of a window at the least */
#define WIN_SHARE		2			/* a window takes 1/n of work_mem */
#define WIN_ROW_BYTES	24			/* bytes a row takes of a decoded column: a value, its code or string, a null bit */
#define SB_KEYS		(((uint64) SIFT_SB_PAGES) << 16)	/* keys of a superblock: (vblock << 16) | offset */

typedef struct GroupSrc
{
	SiftColSource *src;
	const SiftColSchema *schema;
	int		   *leaf;			/* [natts + 1] the leaf of attribute a in this schema, -1 for none */
	int			exc;
} GroupSrc;

typedef struct UnitTids
{
	int			src;
	uint32		ordinal;
	uint32		n;				/* live rows */
	ItemPointerData *tid;		/* of the live rows, ascending; NULL in a fetch scan, which bisects ix (a key-cut unit has no ix and is decoded) */
	uint32	   *ord;			/* of tid: the row index in the vectors of the unit, taken together with tid (VACUUM clears live bits in place) */
	SiftColUnitIndex *ix;		/* where the rows of the unit are, from its page directory */
	uint64	   *live;			/* the live rows of the unit by row index */
	uint32	   *xmin;			/* the versions of every row of the unit by that index; NULL when none is unfrozen */
	uint32	   *cmin;
} UnitTids;

/* A unit of the scan's range: which source, its superblock, and its place among the units of that superblock */
typedef struct UnitRef
{
	int			src;
	uint32		sb;
	uint32		ordinal;
} UnitRef;

typedef struct VisArg
{
	Relation	rel;
	SiftTblScanState ss;		/* a group scan reads groups: no write buffer */
} VisArg;

struct SiftTblGroupScan
{
	Relation	rel;
	SiftTblRel *st;
	TupleDesc	desc;
	MemoryContext cxt;
	MemoryContext rowcxt;
	MemoryContext unitcxt;		/* the TIDs, selection and the whole-unit vectors of the unit being read (reset by the next one) */
	MemoryContext wincxt;		/* the vectors of the rows [win_lo, win_lo + win_n) of the unit, reset when a row outside them is read */
	int			nsrc;
	GroupSrc   *s;
	Datum	   *values;
	bool	   *isnull;
	SiftColVec *vec;			/* [ncols] of the schema of the current unit: PG_UINT32_MAX in n when not read */
	bool	   *vwin;			/* [ncols] the vector holds the window's rows, not the unit's */
	uint32		win_rows;		/* rows a window takes: what a share of work_mem holds of the columns of the table */
	uint32		win_lo;
	uint32		win_n;
	uint32		win_token;		/* the token of the unit the window is of */
	Snapshot	snapshot;
	SiftTblVers *vers;			/* of the directory copy the scan reads */
	SiftColSegment *useg;		/* the segment and unit of the current unit, to read its versions */
	uint32		uu;
	SiftColVerSum usum;
	uint32	   *xm;				/* its versions by row ordinal, read when a row's version is asked for */
	uint32	   *cm;
	bool		hidden;			/* the (MVCC) snapshot sees no row of the current unit: it has none to return */
	/* iteration */
	int			cur;
	bool		begun;
	uint32		sb_lo;
	uint32		sb_hi;
	SiftColUnit unit;
	SiftColSel	sel;
	ItemPointerData *tids;
	uint32	   *ord;			/* the row ordinals of tids: the position of a row in the vectors, which hold every row of the unit */
	uint32		n;
	uint32		i;
	bool		in_unit;
	bool		plain;
	/* the units of the range in the order of a forward scan, listed when a scan first goes backward; uidx is the current one (-1: none yet) */
	UnitRef    *units;
	uint32		nunits;
	int64		uidx;
	/* the rows of the superblock of the last block read */
	uint32		cache_sb;
	UnitTids   *cu;
	int			ncu;
	/* the unit of cu whose rows are read into vec (all of them: the next fetches of its rows decode nothing), valid for one cu */
	int			rd_unit;
	uint64		serial;			/* of cu */
	uint64		rd_serial;
	/* readers of the current unit that outlive the call that found the row: a slot holds a reference and its token */
	int			refs;
	bool		ended;
	bool		point;			/* the fetch scan: a superblock's cache is the page directories of its units, not the TIDs of their rows (sift_col_unit_index) */
	Bitmapset  *want;			/* the attributes a reader asks for (offset by FirstLowInvalidHeapAttributeNumber, as pull_varattnos makes them); NULL: all; the others come back NULL */
	bool		park_fill;		/* the fetch scan: the pages a fill pinned are released at once (nothing ends it before the transaction does) */
	uint32		token;		/* changes whenever another unit becomes the current one: vec, ord and unit are then not the slot's */
};


extern void gscan_park(SiftTblGroupScan *g);
extern HeapTuple row_tuple(SiftTblGroupScan *g, uint32 i, ItemPointer tid);

#endif
