/*
 * col/f2/f2_vacuum_run.h - the maintenance of VACUUM, autovacuum and sift.col_maintain() of a sift_col index, and the steps of its requests in the maintenance queue (f2_vacuum_run.c).
 */
#ifndef SIFT_COL_F2_VACUUM_RUN_H
#define SIFT_COL_F2_VACUUM_RUN_H

#include "postgres.h"

#include "access/genam.h"

#include "core/maint_sched.h"

/*
 * The cleanup of a vacuum (force_sync_full false): the stage is written, the matured extents are given back, the slabs that are due become parts and the settled parts are sealed; the
 * rewriting is requested of the maintenance workers, or done within sift.vacuum_rewrite_limit when the queue is not there. force_sync_full (sift.col_maintain()) does all of it, now.
 */
extern IndexBulkDeleteResult *sift_f2_vacuum_run(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, bool force_sync_full);

/* One request of the queue for a sift_col index or a sift_table of format major 2, run by the step of core/maint_step.c */
extern SiftMaintStep sift_f2_maint_step(Relation rel, const SiftMaintClaim *claim);

/* Asks the queue for the kind of work (part_id 0: the whole relation); false when there is no queue (the caller does the work), as for a temporary relation, which no other process can open */
extern bool sift_f2_maint_request(Relation rel, SiftMaintKind kind, uint64 part_id);

/*
 * A write or a read that finds a partition with SIFT_COMPACT_L0_FANOUT level-0 parts or more asks for their compaction: nothing waits for a VACUUM, which an append-only table never gets.
 * wanted (NULL: every partition) says which of the entries the caller touched
 */
extern void sift_f2_maint_note_parts(Relation rel, const SiftPartitionEntry *entries, int n, bool (*wanted) (const SiftPartitionEntry *e, void *arg), void *arg);

#endif							/* SIFT_COL_F2_VACUUM_RUN_H */
