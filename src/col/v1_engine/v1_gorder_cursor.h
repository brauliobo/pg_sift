/*
 * col/v1_engine/v1_gorder_cursor.h - the global order of a leaf of a Format 1 index (col/v1/gorder.h) as the rows a snapshot sees, in the order of an ORDER BY on the leaf.
 * The entries of the stored segments and of the write buffers are (key, heap address) of rows as of the build: the cursor merges them and has each entry confirmed. A row that is live on a page
 * every snapshot sees is exact and the heap is not read; a row the unit holds on another page, or one a write buffer holds, is fetched for the snapshot and must hold the key of its entry (an UPDATE
 * that changed the key leaves the old entry on the version that is no longer the row); a row the segment lost is fetched the same way, as a delete that aborted leaves it lost and live.
 */
#ifndef SIFT_COL_V1_ENGINE_V1_GORDER_CURSOR_H
#define SIFT_COL_V1_ENGINE_V1_GORDER_CURSOR_H

#include "postgres.h"

#include "access/attnum.h"
#include "executor/tuptable.h"
#include "storage/itemptr.h"
#include "utils/palloc.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"

typedef struct SiftV1GOrd SiftV1GOrd;

/* The key leaf of an index: the column of the index that holds the attribute alone, the attribute of the heap, and its type (int2, int4, int8, date, timestamp, timestamptz) */
typedef struct SiftV1GOrdSpec
{
	Relation	heap;
	Relation	index;
	Snapshot	snapshot;
	int			leaf;
	AttrNumber	heap_att;
	Oid			typid;
	bool		desc;
	bool		nulls_first;
	uint32		rows;			/* the rows the scan will take: the order of the write buffers is sorted for so many */
	uint64		budget;			/* the entries the walk may examine: past them it ends (sift_v1_gord_over) */
} SiftV1GOrdSpec;

typedef struct SiftV1GRow
{
	ItemPointerData tid;		/* the address of the entry: the row the snapshot sees hangs from it */
	int64		key;
	bool		isnull;
	bool		held;			/* the heap row is in the slot of the cursor */
} SiftV1GRow;

/* NULL when the stored orders cannot answer (a segment lacks the order of the leaf, a write buffer holds a value the order cannot) */
extern SiftV1GOrd *sift_v1_gord_open(MemoryContext cx, const SiftV1GOrdSpec *spec);
/* The next row the snapshot sees, in order; false at the end, or where the order is not known any further (sift_v1_gord_short) */
extern bool sift_v1_gord_next(SiftV1GOrd *g, SiftV1GRow *row);
/* The heap row of the entry sift_v1_gord_next gave, in the slot of the cursor until the next call */
extern TupleTableSlot *sift_v1_gord_row(SiftV1GOrd *g, SiftV1GRow *row);
/* The entries examined so far, and whether the walk ended because it examined more than its budget: the rows given are the order's first, those it would give next are not known */
extern uint64 sift_v1_gord_examined(const SiftV1GOrd *g);
extern bool sift_v1_gord_over(const SiftV1GOrd *g);
/* A segment that keeps the ends of its order only has more entries after the last it gave: the rows given are the order's first, those it would give next are not known */
extern bool sift_v1_gord_short(const SiftV1GOrd *g);
extern void sift_v1_gord_close(SiftV1GOrd *g);

#endif							/* SIFT_COL_V1_ENGINE_V1_GORDER_CURSOR_H */
