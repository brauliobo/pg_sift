/*
 * col/f2/f2_part_pin_scan.h - the part pin (col/f2/f2_gbuf.h) as the scan of an index reads it: the part, its granule directory, its deletion vector and its TID index are those of the pin, and what the
 * scan reads of the part before a granule (the exceptions, the zones of a leaf) is read once for the life of the pin, not once for each statement.
 */
#ifndef SIFT_F2_PART_PIN_SCAN_H
#define SIFT_F2_PART_PIN_SCAN_H

#include "col/f2/f2_gbuf.h"

/* The part of the descriptor pinned for the scan of the index; give it back with sift_f2_pin_release */
extern SiftF2PartPin *sift_f2_scan_pin(Relation rel, const SiftPartDesc *d, uint32 part_id);

/* The exceptions of the part (SEC_VARIANT_EXC), NULL when it has none */
extern const SiftVariantExcSummary *sift_f2_pin_exc(Relation rel, SiftF2PartPin *pin, int *nexc);

/* Whether the leaf of the granule holds a value the chunk cannot judge (the summaries of SEC_VARIANT_EXC are ordered by granule, then leaf) */
extern bool sift_f2_exc_has(const SiftVariantExcSummary *ex, int nex, uint32 g, int col);

/* The zones of every granule of a leaf (SEC_ZONES), NULL when the part has none */
extern const SiftZone *sift_f2_pin_zones(Relation rel, SiftF2PartPin *pin, int leaf);

/* The point directory of the part (SEC_POINT), NULL when it has none */
struct SiftPointDir;
extern const struct SiftPointDir *sift_f2_pin_point(Relation rel, SiftF2PartPin *pin);

#endif							/* SIFT_F2_PART_PIN_SCAN_H */
