/*
 * core/extlock.h - the one way sift holds the relation extension lock.
 *
 * PostgreSQL forbids every other heavyweight lock while the extension lock is held (an assertion in LockAcquireExtended since 19; a lock-order violation in any build). Whatever runs under
 * it may report: the error context of the index, the damage record and the catalog of sift look names and access methods up, and a lookup that misses the cache reads a catalog under a
 * lock. So the lock is taken here, after the lookups of every report are warm; sections below it take no heavyweight lock, which keeps the cache from being invalidated while it is held.
 * scripts/check-extlock.sh fails a file that takes the lock or extends a relation under it any other way.
 */
#ifndef SIFT_EXTLOCK_H
#define SIFT_EXTLOCK_H

#include "storage/bufmgr.h"
#include "utils/rel.h"

extern void sift_extension_lock(Relation rel);
extern void sift_extension_unlock(Relation rel);

/* The blocks of the relation up to nblocks zeroed, the last one returned locked: ExtendBufferedRelTo, which takes the lock itself */
extern Buffer sift_extend_zeroed_to(Relation rel, BlockNumber nblocks);

#endif							/* SIFT_EXTLOCK_H */
