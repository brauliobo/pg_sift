/*
 * bm25/semi_cache.h - the key sets of the small tables a ranked scan tests an INCLUDE column against (SIFT_PRIV_SEMI), kept by the backend for the next statement.
 */
#ifndef SIFT_SEMI_CACHE_H
#define SIFT_SEMI_CACHE_H

#include "postgres.h"

#include "access/attnum.h"
#include "utils/snapshot.h"

/* the nset of an entry for a table that holds more keys than a scan tests (SIFT_SEMI_MAX_SET): there is no set to test */
#define SIFT_SEMI_OVER	(-1)

/* The set of the keys of attno of the rows dim has that pass quals (their node string), copied into memory of into; false when the cache has none for this snapshot */
extern bool sift_semi_cache_get(Oid dim, AttrNumber attno, const char *quals, Snapshot snapshot, MemoryContext into, int64 **set, int *nset);

/* Keeps the set a scan read under snapshot, when what a later snapshot sees of dim can be told from the number that the snapshot has: nothing is kept otherwise */
extern void sift_semi_cache_put(Oid dim, AttrNumber attno, const char *quals, Snapshot snapshot, const int64 *set, int nset);

#endif							/* SIFT_SEMI_CACHE_H */
