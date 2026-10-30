/*
 * gscan_int.h - what the files of the group scan (gscan*.c) share: its types and the functions that cross them.
 */
#ifndef SIFT_TABLE_GSCAN_INT_H
#define SIFT_TABLE_GSCAN_INT_H

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/skey.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "col/pg.h"
#include "col/seg.h"
#include "core/lane.h"
#include "bm25/maint.h"
#include "bm25/merge.h"
#include "core/ops.h"
#include "table/table.h"
#include "table/seg_int.h"
#include "core/cmp.h"

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
	SiftTblRel *st;
	Snapshot	snapshot;
	SiftTblVers *vers;
} VisArg;

struct SiftTblGroupScan
{
	Relation	rel;
	SiftTblRel *st;
	TupleDesc	desc;
	MemoryContext cxt;
	MemoryContext rowcxt;
	MemoryContext unitcxt;		/* the TIDs, selection and vectors of the unit being read (reset by the next one) */
	int			nsrc;
	GroupSrc   *s;
	Datum	   *values;
	bool	   *isnull;
	SiftColVec *vec;
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
