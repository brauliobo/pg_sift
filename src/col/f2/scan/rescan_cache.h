/*
 * col/f2/scan/rescan_cache.h - the granules an ordered scan of an index of format major 2 reads again and again.
 *
 * A scan rescanned with other keys (the inner side of a parameterized nested loop) finds the granule of each part its
 * key falls in decoded once for all later probes. A part never changes once written, so a decoded granule is kept with
 * the pin of its part (col/f2/f2_gbuf.h) and goes with it, under the budget of core/cachebudget.h. What depends on a
 * snapshot (deletion vector, pending deletes, the slab) is read again by each probe.
 */
#ifndef SIFT_F2_RESCAN_CACHE_H
#define SIFT_F2_RESCAN_CACHE_H

#include "col/f2/f2_gbuf.h"
#include "col/f2/scan/scan_int.h"

typedef struct SiftRescanScan SiftRescanScan;

/* What the scan reads of a decoded granule; NULL while the scan was not rescanned or reads more than a granule holds */
extern SiftRescanScan *sift_rescan_begin(F2Scan *s, const SiftF2Rel *f, const SiftColSchema *schema);
extern void sift_rescan_end(F2Scan *s);

/* The granules of the part can be read decoded: no column delta patches them, no leaf holds an exception */
extern bool sift_rescan_part_ok(Relation rel, const SiftPartitionEntry *e, SiftF2PartPin *pin);

/* The batch of granule g of the pinned part, handed to the sink of the scan as scan_span gives it */
extern void sift_rescan_granule(Relation rel, F2Scan *s, SiftRescanScan *sc, const SiftColSchema *schema,
								SiftF2PartPin *pin, uint32 g, const SiftPendDelIndex *px, Snapshot snap);

#endif							/* SIFT_F2_RESCAN_CACHE_H */
