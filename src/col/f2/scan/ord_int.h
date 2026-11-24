/*
 * col/f2/scan/ord_int.h - what the ordered scan of an index of format major 2 shares between its merge (ord_stream.c) and its cursors (ord_cursor.c): a cursor reads one part, or the L0 slab, in
 * windows of rows of a granule, small first so that a scan that stops early decodes only the rows it reads.
 */
#ifndef SIFT_F2_ORD_INT_H
#define SIFT_F2_ORD_INT_H

#include "col/f2/scan/par_int.h"
#include "col/f2/scan/rescan_cache.h"

typedef struct OrdRow
{
	uint64		tid;
	int64		key[SIFT_BOUND_COMPS];
	uint32		retnull;
	uint8		knull;
	uint8		maybe;
	Datum		ret[FLEXIBLE_ARRAY_MEMBER];
} OrdRow;

/* The rows [lo, hi) of the gi-th granule of a cursor, in the order of the granule */
typedef struct OrdWin
{
	uint32		gi;
	uint16		lo;
	uint16		hi;
} OrdWin;

typedef struct OrdCursor
{
	struct SiftOrdStream *os;
	SiftF2PartPin *pin;			/* NULL for the slab */
	PartCx		pcx;
	const SiftPointCands *cands;
	const SiftPartitionEntry *e;
	const SiftPartDesc *d;
	const SiftPendDelIndex *px;
	bool		cached;			/* its granules are read decoded, whole (sift_rescan_part_ok) */
	uint32		spans[SIFT_F2_MAX_RANGES][2];
	int			nspans;
	uint32		ngran;
	bool		loaded;			/* the live rows of the window win are in rows */
	OrdWin		win;
	int			nwin;			/* the windows the cursor moved through since it was primed: the next one is larger */
	char	   *rows;
	int			nrows;
	int			cap;
	int			pos;			/* the cursor is between rows pos - 1 and pos */
	int			head;			/* the row the cursor gives next in the direction of the scan, -1: none */
	MemoryContext datacx;
} OrdCursor;

typedef struct OrdPos
{
	bool		loaded;
	OrdWin		win;
	int			pos;
} OrdPos;

typedef struct OrdMark
{
	bool		valid;
	bool		fwd;
	bool		ended;
	int			part;
	int			last;
	int			ncur;
	OrdPos	   *pos;
} OrdMark;

typedef struct SiftOrdStream
{
	F2Scan	   *s;
	Relation	rel;
	Snapshot	snap;
	const SiftPlanSnapshot *plan;
	const SiftF2Rel *f;
	const SiftColSchema *schema;
	const SiftPendDelIndex *staged_px;
	SiftRescanScan *rc;			/* not NULL: the granules of a part are read decoded (scan/rescan_cache.c) */
	int			nkey;
	int			key_leaf[SIFT_BOUND_COMPS];
	bool		nulls_first[SIFT_BOUND_COMPS];
	bool		desc[SIFT_BOUND_COMPS];
	int			leaves[SIFT_BOUND_COMPS + INDEX_MAX_KEYS];
	int			nleaves;
	Size		stride;
	int			plo;			/* the partitions that can hold a row, in the order of the key */
	int			phi;
	int			part;			/* the partition the cursors read; plo - 1 before the first and phi + 1 after the last */
	bool		fwd;
	bool		ended;
	MemoryContext pcx;			/* the cursors of the partition */
	OrdCursor  *cur;
	int			ncur;
	int		   *heap;
	int			nheap;
	int			last;			/* the cursor that gave the last row, -1: none */
	bool		itup_ok;		/* every returned attribute has a fixed size: the row is an index tuple (what the planner reads the ends of a column from); else a heap tuple */
	IndexTuple	itup;
	OrdMark		mark;
} SiftOrdStream;

static inline OrdRow *
ord_row_at(const SiftOrdStream *os, const OrdCursor *c, int i)
{
	return (OrdRow *) (c->rows + (Size) i * os->stride);
}

extern int	ord_row_cmp(const SiftOrdStream *os, const OrdRow *a, const OrdRow *b);
/* The sink of the scan: the rows of a batch of the window a cursor loads, kept in the order the granule has them */
extern void ord_sink(void *arg, const SiftF2Batch *b);
extern void ord_cur_init(SiftOrdStream *os, OrdCursor *c, const SiftPartitionEntry *e, const SiftPartDesc *d, const SiftPendDelIndex *px);
/* The next row of the cursor in the direction (its index in rows), loading the window that has it; -1 when there is none */
extern int	ord_cur_head(SiftOrdStream *os, OrdCursor *c, bool fwd);
/* Takes back the row the cursor advanced over in direction fwd: its position is before it again */
extern void ord_cur_unstep(SiftOrdStream *os, OrdCursor *c, bool fwd);
/* The window a mark names, read again */
extern void ord_cur_restore(SiftOrdStream *os, OrdCursor *c, const OrdPos *p);

#endif							/* SIFT_F2_ORD_INT_H */
