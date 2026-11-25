/*
 * col/part_vis.h - visibility in format major 2: one rule for every part, granule and deletion vector, by the bounds of the transactions that wrote them.
 *
 * Every part, granule and deletion vector carries the least and the greatest full transaction id of its writers, so no stored id wraps. That a writer is older than a snapshot
 * says it ended, not that it committed: an aborted writer leaves rows in a part that no snapshot may see. So the bounds are trusted only with a seal that says the writers committed:
 * SIFT_PART_FLAG_ALL_VISIBLE (frozen at the write, or set by sift_part_try_seal after it asked the log for each inserter) for the inserters and SIFT_PART_FLAG_DV_SETTLED for the
 * deleters of a deletion vector. A snapshot whose xmin is past the greatest writer of a sealed part sees every row it holds (no version vector, no clog, no heap visibility
 * map): the cheapest case is a sealed part that has no deletion vector and no pending delete, which is counted without reading a page of it. The inserters and the deleters are
 * judged apart: a part sealed for its inserters is read without its version vector whatever its deleters are. The rows of what is not sealed (the L0 parts of a young transaction,
 * the slab) are asked of the clog, once for each transaction (core/xid_cache.h).
 */
#ifndef SIFT_PART_VIS_H
#define SIFT_PART_VIS_H

#include "core/host.h"

#include "utils/snapshot.h"

#include "col/f2/del/delete.h"
#include "core/xid_cache.h"

typedef enum SiftVisClass
{
	SIFT_VIS_PART_CLEAN = 1,	/* every row visible, none dead: O(1) */
	SIFT_VIS_PART_SETTLED_DV,	/* every row visible, the vector's rows dead outright */
	SIFT_VIS_GRANULE_SETTLED,	/* the same, judged for one granule */
	SIFT_VIS_PER_ROW			/* the inserters and deleters are asked one row at a time */
} SiftVisClass;

/* The xmin of the snapshot as a full transaction id; invalid for a snapshot of another kind than MVCC, which no bound of a writer is trusted for */
extern FullTransactionId sift_vis_snap_xmin(Snapshot snap);

extern SiftVisClass sift_vis_part_class(const SiftPartDesc *d, bool has_pend_del, FullTransactionId snap_xmin);
/* dv_xid_max: the greatest deleter the granule's deletion vector keeps (invalid: none); pend_hit: a pending delete may name a row of the granule */
extern SiftVisClass sift_vis_granule_class(const SiftPartDesc *d, const SiftGranuleDesc *g, FullTransactionId dv_xid_max, bool pend_hit, FullTransactionId snap_xmin);

/* The inserters of every row of the part are known committed and older than the snapshot: its version vector need not be read */
extern bool sift_vis_part_inserts_settled(const SiftPartDesc *d, FullTransactionId snap_xmin);
/* The same for one granule: its inserters are known committed and older than the snapshot */
extern bool sift_vis_granule_inserts_settled(const SiftPartDesc *d, const SiftGranuleDesc *g, FullTransactionId snap_xmin);

/*
 * The row of the inserter (xmin, cmin) is visible to the snapshot (a frozen row always is). A snapshot that is not MVCC (dirty, self, any) hides the rows of a transaction that
 * aborted and no more: the heap, which every address an index hands out is checked against, decides the rest.
 */
extern bool sift_vis_inserted(uint32 xmin, uint32 cmin, Snapshot snap);

/* The delete by (xmax, cmax) is visible to the snapshot: an invalid xmax is a delete settled before every snapshot. A snapshot that is not MVCC sees committed deletes only. */
extern bool sift_vis_deleted(TransactionId xmax, CommandId cmax, Snapshot snap);

/* The same as sift_vis_inserted and sift_vis_deleted, the log asked once for each transaction of the cache (made for the same snapshot) */
extern bool sift_vis_inserted_cached(SiftXidCache *c, uint32 xmin, uint32 cmin);
extern bool sift_vis_deleted_cached(SiftXidCache *c, TransactionId xmax, CommandId cmax);

/*
 * Seals the part for its inserters without rewriting a page of it: when it is older than the horizon of hc and the log says that every row's inserter committed (so there is nothing
 * for a compaction to drop), its ALL_VISIBLE flag is set in d (the caller commits the changed descriptor, sift_manifest_commit update_parts). Its bounds stay, so no snapshot loses what told it
 * a row was not yet there. False when the part is sealed, frozen already, too young or holds a row of an inserter that is running or aborted.
 */
extern bool sift_part_try_seal(Relation rel, uint32 part_id, SiftPartDesc *d, SiftXidHorizonCache *hc);

typedef struct SiftVisStats
{
	uint32		parts_clean;
	uint32		parts_settled_dv;
	uint32		granules_settled;
	uint32		granules_per_row;
	uint64		rows_checked;	/* rows whose inserter or deleter was asked of the clog */
} SiftVisStats;

/* The rows of the part the snapshot sees: the reference reader of the rules above; pend: the pending deletes of the partition */
extern uint64 sift_part_count_visible(Relation rel, const SiftPartDesc *d, uint32 part_id, const SiftPendDelRec *pend, int npend, Snapshot snap, SiftVisStats *st);

#endif							/* SIFT_PART_VIS_H */
