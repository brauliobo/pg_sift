/*
 * col/f2/f2_v6_read.h - the granules of an index of format major 2 read one at a time for the operators of SiftAggV6 (f2_v6_bridge.c, f2_v6_topn.c): a snapshot of the index with the
 * deletions its pending deletes and its transaction's staged rows give, a part opened once with its deletion vector, and a granule or a slab decoded for the leaves asked, with the rows the
 * deletion vector and the pending deletes leave as a bitmap. The scan of the access method (f2_scan.c) does the same for the predicates of a query; here the quals are the operator's.
 */
#ifndef SIFT_COL_F2_V6_READ_H
#define SIFT_COL_F2_V6_READ_H

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"

#include "agg/v6/v6_vexpr.h"
#include "col/f2/f2.h"
#include "col/f2/f2_cache.h"
#include "col/f2/f2_col_delta_idx.h"
#include "col/f2/f2_sink.h"
#include "col/f2/delvec.h"
#include "col/f2/pend_del.h"
#include "col/granule/granule.h"

typedef struct F2V6Ctx
{
	MemoryContext cx;
	Relation	index;
	Relation	heap;
	const SiftF2Rel *f;
	const SiftColSchema *schema;
	const SiftColRel *rel;
	const SiftPlanSnapshot *plan;
	Snapshot	snap;
	const SiftPendDelIndex *staged_px;	/* the rows its transaction deleted, as a set the pending deletes of each partition add to */
	IndexFetchTableData *fetch;
	TupleTableSlot *slot;
	Buffer		vmbuf;
	BlockNumber vm_blk;
	bool		vm_visible;
	SiftXidCache xc;			/* the snapshot, for the column deltas of the partitions */
} F2V6Ctx;

/* A part opened for its granules */
typedef struct F2V6Part
{
	const SiftPartitionEntry *e;
	const SiftPartDesc *d;
	SiftPart   *part;
	SiftGranuleDesc *dir;
	SiftDv	   *dv;
	SiftTidIndexEntry *tix;
	const SiftPendDelIndex *px;
	SiftColDeltaPartMap *cd;	/* the patches of the column deltas the snapshot sees of the part; NULL: none */
	uint32		ngranules;
} F2V6Part;

/* The rows of a granule or a slab: views[leaf] for the leaves asked; live: a bit per row that no deletion vector or pending delete removed */
typedef struct F2V6Gran
{
	int			n;
	ColView    *views;
	uint64	   *tid;
	uint64	   *live;
} F2V6Gran;

/* The index of the estate's statement: its relation opened, the staged rows written, the snapshot of the plan cache, the heap to vet rows in; everything in cx */
extern F2V6Ctx *f2v6_begin(MemoryContext cx, Snapshot snap, Oid indexoid);
extern void f2v6_end(F2V6Ctx *c);

/* The pending deletes of the partition the snapshot sees (and the staged rows' deletes) as a set; NULL: none */
extern const SiftPendDelIndex *f2v6_pending(F2V6Ctx *c, const SiftPartitionEntry *e);

/* Whether the column deltas of the partition patched its time columns: the bounds of the partition and its parts do not hold for them */
extern bool f2v6_time_dirty(F2V6Ctx *c, const SiftPartitionEntry *e);
extern bool f2v6_entry_meets_time(F2V6Ctx *c, const SiftPartitionEntry *e, const SiftF2TimeWin *w);
extern bool f2v6_part_meets_time(F2V6Ctx *c, const SiftPartitionEntry *e, const SiftPartDesc *d, const SiftF2TimeWin *w);

extern F2V6Part *f2v6_part_open(F2V6Ctx *c, const SiftPartitionEntry *e, const SiftPartDesc *d, const SiftPendDelIndex *px);

/* Granule g of the part decoded for the leaves, in the memory the caller resets for each granule */
extern void f2v6_granule(F2V6Ctx *c, const F2V6Part *p, uint32 g, const int *leaves, int nleaves, F2V6Gran *out);

/* The rows of the slab of the partition (the rows its inserts staged and no part holds yet), for the leaves, in the order of a part when sorted */
extern void f2v6_slab(F2V6Ctx *c, const SiftPartitionEntry *e, const SiftPendDelIndex *px, bool sorted, const int *leaves, int nleaves, F2V6Gran *out);

/* The zones of a leaf over every granule of the part; NULL when the part keeps none for it */
extern SiftZone *f2v6_zones(F2V6Ctx *c, const F2V6Part *p, int leaf);

/* Whether the heap shows the row at address addr to the snapshot (a page the visibility map has all visible needs no look) */
extern bool f2v6_row_visible(F2V6Ctx *c, uint64 addr);

/* The columns attnos[0 .. n-1] of the heap row at address addr; false when it is not visible to the snapshot */
extern bool f2v6_fetch(F2V6Ctx *c, uint64 addr, const AttrNumber *attnos, int n, Datum *values, bool *isnull);

#endif							/* SIFT_COL_F2_V6_READ_H */
