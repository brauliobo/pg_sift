/*
 * col/f2/uniq/slab.h - what a writer of a UNIQUE index of format major 2 knows of the keys in the L0 slabs of the partitions without reading their rows (f2_uniq_slab.c).
 *
 * The slab of an index is a chain of references to heap tuples: a reader finds the key of each in the heap. A writer that looks one key up cannot read every row of a slab of thousands, so
 * it keeps, for each slab, the 64-bit hash of the key of each reference (the one of the registry, f2_uniq_shm.h), made once for each reference and kept while the slab grows; a slab
 * that cannot hold the key is left out of the manifest the scan of the key reads.
 */
#ifndef SIFT_F2_UNIQ_SLAB_H
#define SIFT_F2_UNIQ_SLAB_H

#include "postgres.h"

#include "executor/tuptable.h"
#include "utils/rel.h"

#include "col/f2/cache.h"

/* The 64-bit hash of the key of the row in the slot, 0 for a row whose key holds no row of the unique index (a NULL that is distinct) */
typedef uint64 (*SiftUniqSlabKeyFn) (void *arg, TupleTableSlot *slot);

/* Takes the 64-bit hash of one key; false when the table that keeps them has no room left: the fold stops */
typedef bool (*SiftUniqFpPut) (void *arg, uint64 fp);

/* The hashes of the keys of the heap tuples at tids[0..n) (what a slab or a part names), offered to put in order; one that is gone (an aborted insert the heap pruned) holds no key. False when put stopped it */
extern bool sift_uniq_fold_tids(Relation heap, const ItemPointerData *tids, uint32 n, SiftUniqSlabKeyFn key_fp, void *arg, SiftUniqFpPut put, void *put_arg);

/*
 * The manifest of plan without the slabs that hold no key of the hash fp64 (a copy in cx; plan itself when it has no slab to leave out). A slab that holds a row of the hash stays: the scan of the
 * key reads its rows and decides.
 */
extern const SiftPlanSnapshot *sift_uniq_slab_prune(Relation index, Relation heap, const SiftPlanSnapshot *plan, uint64 fp64, SiftUniqSlabKeyFn key_fp, void *arg, MemoryContext cx);

#endif							/* SIFT_F2_UNIQ_SLAB_H */
