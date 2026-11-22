/*
 * core/wal_conflict.h - the recovery conflict of a change a standby's snapshots could not tell from the primary's.
 */
#ifndef SIFT_WAL_CONFLICT_H
#define SIFT_WAL_CONFLICT_H

#include "core/host.h"

/*
 * Generic WAL carries no snapshot horizon, so a standby that does not feed its xmin back (hot_standby_feedback) is protected only by this record: a btree page-reuse record of the
 * relation (blk: the block reused, or InvalidBlockNumber) that a standby replays as the usual recovery conflict, waiting for the snapshots older than horizon up to
 * max_standby_streaming_delay and then cancelling them. It touches no page. Call it before the commit that strips visibility history (a freeze, a settled delete) or lets blocks be
 * reused or cut off. Logged once for each horizon a backend has not covered yet (an older one is covered by a newer); false when nothing was logged: sift.standby_conflicts is off,
 * no standby can exist, the relation is not logged or the horizon is invalid.
 */
extern PGDLLIMPORT bool sift_standby_conflicts;

extern bool sift_wal_snapshot_conflict(Relation index, BlockNumber blk, FullTransactionId horizon);

#endif							/* SIFT_WAL_CONFLICT_H */
