/*
 * col/f2/drop/parts.h - sift.drop_parts_before: the imperative retention of a sift_table. The sealed parts whose newest row on a time column is older than a cutoff leave the manifest in
 * one commit per partition, with their blocks, deletion vectors, packed pages and wide values released: no row is read, deleted or logged.
 *
 * The decisions are made on descriptors alone (sift_drop_plan: no relation, no page), so that they can be tested without a server; sift_f2_drop_parts_before is the one commit of them.
 * A part that still holds a row inside the retention stays whole (ttl_only_drop_parts), a part that has not settled yet is marked and left for the next call, and a partition whose column
 * delta chain patched the time column is left whole: the bounds of its parts do not hold for it until the chain is folded.
 */
#ifndef SIFT_F2_DROP_PARTS_H
#define SIFT_F2_DROP_PARTS_H

#include "core/host.h"

#include "utils/relcache.h"

#include "col/f2/ttl.h"
#include "core/f2/col_delta_anchor.h"
#include "core/f2/manifest/manifest.h"

typedef struct SiftDropResult
{
	int32		parts_dropped;
	int64		rows_dropped;	/* the live rows of the dropped parts */
	int64		bytes_reclaimed;
	int32		parts_partial;	/* the parts that span the cutoff: kept whole */
	int32		parts_unsettled;	/* wholly expired but not droppable yet: marked, or behind a column delta that patched the time column */
} SiftDropResult;

/* The cutoff in the unit of a time column (a day for a date column): the least value that is not older than clock_us, which is the cutoff as the column's own clock reads it */
extern int64 sift_drop_cutoff_units(int64 clock_us, int64 unit);

/* A partition none of whose parts can hold a row older than the cutoff on the slot: no part list is read for it */
extern bool sift_drop_partition_skippable(const SiftPartitionEntry *e, int slot, int64 cutoff);

/* The chain of column deltas of a partition patched the time column of the slot: the time bounds of the parts do not hold for it */
extern bool sift_drop_time_dirty(const SiftColDeltaAnchor *anchor, int slot);

/*
 * The plan of one partition: the parts to drop and to mark (sift_ttl_plan) and the tally of what the call reports. time_dirty: no part is dropped or marked, and the parts that were wholly
 * expired by their bounds are counted unsettled. The plan is allocated with palloc and freed with sift_ttl_plan_free.
 */
extern SiftTtlPlan *sift_drop_plan(const SiftPartDesc *parts, int np, int slot, int64 cutoff, FullTransactionId horizon, bool time_dirty, SiftDropResult *tally);

/* The extents of the wide values the rows of a part refer to, appended to the vector: the references are in the granules that keep an exception stream */
struct SiftF2Store;
struct SiftF2PartPin;
struct SiftExtVec;
extern void sift_part_wide_extents(Relation rel, struct SiftF2Store *store, struct SiftF2PartPin *pin, struct SiftExtVec *v);

/* The extents of the wide values the dropped parts own, appended to the vector (release grown as needed); the count after the append */
extern int	sift_drop_wide_extents(Relation rel, struct SiftF2Store *store, uint32 part_id, const SiftPartDesc *parts, int np, const uint64 *gens, int ngens, SiftExtent **release, int nrelease);

/* The visibility map bits of the vblocks the dropped parts had rows in, cleared before the commit that drops them (nothing for a sift_col index) */
extern void sift_drop_clear_vm(Relation rel, const SiftPartDesc *parts, int np, const uint64 *gens, int ngens);

/* One commit per partition of the parts older than the cutoff; the tally of all of them */
extern SiftDropResult sift_f2_drop_parts_before(Relation rel, struct SiftF2Store *store, int slot, int64 cutoff, FullTransactionId horizon);

#endif							/* SIFT_F2_DROP_PARTS_H */
