/*
 * col/f2/f2_uniq_parts.h - what a writer of a UNIQUE index of format major 2 knows of the keys in the small parts of the partitions without reading them (f2_uniq_parts.c).
 *
 * Parts are never changed once they are published, so a backend reads each small part once, keeps a 32-bit tag of the 64-bit hash of every key it holds (the hash of the registry,
 * f2_uniq_shm.h), and leaves the part out of the manifest the scan of a key reads when the tag of the key is not among them: a probe costs a lookup of a tag, not a pin of every part
 * the writers before it left in level 0, and when a key is there, the parts whose own filter holds its tag are the ones to read. A part that the tags do not cover (a large one, one that came
 * after they were full) stays in the manifest.
 */
#ifndef SIFT_F2_UNIQ_PARTS_H
#define SIFT_F2_UNIQ_PARTS_H

#include "postgres.h"

#include "utils/rel.h"

#include "col/f2/f2_cache.h"
#include "col/f2/f2_uniq_slab.h"

/*
 * The manifest of plan without the covered parts that hold no key of the hash fp64: the parts of plan not yet seen are read first (their keys from the heap tuples they name, by key_fp).
 * It is a copy in cx, or the manifest of the index without the covered parts, which lives until the next call for the index: a scan of it must not outlive that call.
 */
extern const SiftPlanSnapshot *sift_uniq_parts_prune(Relation index, Relation heap, const SiftPlanSnapshot *plan, uint64 fp64, SiftUniqSlabKeyFn key_fp, void *arg, MemoryContext cx);

#endif							/* SIFT_F2_UNIQ_PARTS_H */
