/*
 * core/extlock.h - the one way sift holds the relation extension lock.
 *
 * PostgreSQL forbids every other heavyweight lock while the extension lock is held (an assertion in LockAcquireExtended since 19; a lock-order violation in any build). Whatever runs under
 * it may report: the error context of the index, the damage record and the catalog of sift look names and access methods up, and a lookup reads a catalog under a lock whenever the
 * cache misses (a build with CATCACHE_FORCE_RELEASE, as the assert lane has, misses every time). So the lock is taken here after the lines of the error contexts are resolved and kept
 * in their scopes (core/errctx.h) and the catalog of sift is read; the section below reads no cache that a report could miss, and takes no heavyweight lock.
 * sift_extension_lock is for the one section that must exclude the members of a parallel build too (the append of a value store: the extension lock is the only heavyweight lock
 * that lock group members do not share, src/backend/storage/lmgr/README "group locking"); every other write extends through sift_extend_zeroed_to and holds the lock for that extension only.
 * A test gate waits for an advisory lock, which is a heavyweight lock: it never waits under this one (sift_test_wait fails there), a gate of the directory updates waits when the lock is released.
 * scripts/check-extlock.sh fails a file that takes the lock or extends a relation under it any other way.
 */
#ifndef SIFT_EXTLOCK_H
#define SIFT_EXTLOCK_H

#include "storage/bufmgr.h"
#include "storage/bulk_write.h"
#include "utils/rel.h"

extern void sift_extension_lock(Relation rel);
extern void sift_extension_unlock(Relation rel);

/* Whether this backend holds the lock taken by sift_extension_lock now (an error or a rolled back subtransaction released it without a call to the unlock) */
extern bool sift_extension_held(void);

/* The blocks of the relation up to nblocks zeroed, the last one returned locked: ExtendBufferedRelTo, which takes the lock itself */
extern Buffer sift_extend_zeroed_to(Relation rel, BlockNumber nblocks);
/* The same through a buffer access strategy (a bulk write ring keeps the extension of a big build out of the shared buffers) */
extern Buffer sift_extend_zeroed_to_bas(Relation rel, BlockNumber nblocks, BufferAccessStrategy bas);

/*
 * A run of fresh pages written with smgr_bulk_write (WAL-logged and checksummed by it) under the extension lock, which no other writer of the file holds meanwhile: the pages past the end of the file
 * are queued in bulk memory and the file is extended once; a block that is already in the file may be cached in shared buffers and is never written this way (sift_bulk_run_fresh says which). A
 * relation with local buffers (a temporary one) has no run: sift_bulk_run_begin gives NULL and its pages go through the buffers. The lock is released by sift_bulk_run_end or by the abort of the
 * transaction.
 */
typedef struct SiftBulkRun SiftBulkRun;

extern SiftBulkRun *sift_bulk_run_begin(Relation rel);
extern bool sift_bulk_run_fresh(const SiftBulkRun *run, BlockNumber blk);

/* A zeroed page to build, for sift_bulk_run_write */
extern Page sift_bulk_run_page(SiftBulkRun *run);

/* Seals the page (core/f2/meta.h) and queues it as block blk */
extern void sift_bulk_run_write(SiftBulkRun *run, BlockNumber blk, Page page);
extern void sift_bulk_run_end(SiftBulkRun *run);

#endif							/* SIFT_EXTLOCK_H */
