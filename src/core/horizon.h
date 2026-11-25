/*
 * core/horizon.h - the xid horizon of the table of an index: what a change may strip or reuse once no snapshot can still see it. Format-neutral: both formats ask it.
 */
#ifndef SIFT_HORIZON_H
#define SIFT_HORIZON_H

#include "core/host.h"

/* The GlobalVis horizon is the heap's: it asserts a table, not the index. A sift_table is its own table */
extern bool sift_horizon_removable(Relation index, FullTransactionId xid);

/*
 * The horizon computed anew: the one GlobalVis caches only moves when this backend takes a snapshot, so a backend without one (the maintenance worker, or a merge that takes long) would
 * not see the extents it freed become reusable. Records before it are reusable.
 */
extern FullTransactionId sift_horizon(Relation index);

/*
 * A VACUUM is left out of every horizon (PROC_IN_VACUUM): the extents a writer releases while the VACUUM reads the manifest of an index would be reusable the moment the writer commits, under
 * the feet of the reader. Between begin and end of the read, the extents released stay in the quarantine of the allocator (sift_horizon_vacuum_reading); the writers never wait.
 */
extern void sift_horizon_vacuum_read_begin(Relation index);
extern void sift_horizon_vacuum_read_end(Relation index);
extern bool sift_horizon_vacuum_reading(Relation index);

#endif
