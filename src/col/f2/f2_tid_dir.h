/*
 * col/f2/f2_tid_dir.h - the granules of a part that hold an address, exactly.
 *
 * The envelope of the addresses of a granule (SEC_TID_INDEX) tells nothing when a part is written in the order of the key and not in that of the addresses: the granule of an address
 * is then any of them, and a lookup of one row decodes granule after granule. The directory of a part is the address of every row with its granule, ascending, made from the
 * addresses of the part's granules when a lookup first finds the envelopes of no use, and kept with the part pin under sift.backend_cache_kb.
 */
#ifndef SIFT_F2_TID_DIR_H
#define SIFT_F2_TID_DIR_H

#include "col/f2/f2_gbuf.h"

/* The granules of the part that hold tid, ascending, a palloc'd array of *n; false when the part has no directory: it has not been made and make is false, the budget has no room for one, or the part is the micro-slab of a statement */
extern bool sift_f2_tid_dir_granules(Relation rel, SiftF2PartPin *pin, uint64 tid, bool make, uint32 **gids, int *n);

#endif							/* SIFT_F2_TID_DIR_H */
