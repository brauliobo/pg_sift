/*
 * core/extlock.h - the one way sift holds the relation extension lock.
 *
 * PostgreSQL forbids every other heavyweight lock while the extension lock is held (an assertion in LockAcquireExtended since 19; a lock-order violation in any build). Whatever runs under
 * it may report: the error context of the index, the damage record and the catalog of sift look names and access methods up, and a lookup reads a catalog under a lock whenever the
 * cache misses (a build with CATCACHE_FORCE_RELEASE, as the assert lane has, misses every time). So the lock is taken here after the lines of the error contexts are resolved and kept
 * in their scopes (core/errctx.h) and the catalog of sift is read; the section below reads no cache that a report could miss, and takes no heavyweight lock.
 * sift_extension_lock is for the one section that must hold the extension lock itself; every write extends through sift_extend_zeroed_to and holds the lock for that extension only.
 * The append of a value store excludes the other appenders, the members of a parallel build too, with a page lock of the store (core/page.h), not with this one.
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
