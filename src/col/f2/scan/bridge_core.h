/*
 * col/f2/scan/bridge_core.h - what a scan of an index of format major 2 decides from a TID bridge before it reads a column: whether a part, a granule or a slab holds a row of the bridge.
 * bridge.c binds it to the scan of f2_scan.c; the rules are here so that a program with no database can test them. Portable (core/host.h).
 *
 * The order of a scan with a bridge (Spec 33 section 3.3.3):
 *   1. a part whose key envelope misses the bridge is not opened (a remote part costs a round trip to open);
 *   2. the TID index of the part is loaded whenever there is a bridge, not only when a pending delete is (a part that no delete touched has the index too);
 *   3. a granule whose envelope, blocks or pages miss the bridge is not read;
 *   4. of a granule that is read, only the _tid chunk is decoded first: the dead rows leave, then the rows of the bridge, and when none is left no other chunk is loaded.
 * The addresses of a row are those the column delta chain gave it (col/f2/col_delta/idx.h), never the ones the part stored: a part or a granule with a moved row is judged on the rows' addresses.
 */
#ifndef SIFT_F2_SCAN_BRIDGE_CORE_H
#define SIFT_F2_SCAN_BRIDGE_CORE_H

#include "core/host.h"
#include "core/tidbridge.h"

typedef bool (*SiftBridgeDeadFn) (void *arg, uint64 tid);

/* The part carries a TID index that the scan needs: a pending delete names rows of it, or a bridge asks which granules hold its TIDs */
static inline bool
sift_f2_bridge_needs_tix(bool pending_deletes, const SiftTidBridge *br)
{
	return pending_deletes || br != NULL;
}

/*
 * No row of a part whose addresses lie in [part_tid_min, part_tid_max] is in the bridge; a part with tid_max 0 has no envelope and is never excluded. remapped: the column delta chain moved
 * rows of the part to addresses its envelope does not hold, so the envelope tells nothing.
 */
extern bool sift_f2_bridge_part_excluded(const SiftTidBridge *br, uint64 part_tid_min, uint64 part_tid_max, bool remapped);

/* The granule is ruled out by the bridge: the TID index is known (have_tix), no row of the granule has moved (remapped), and the envelope, blocks and pages of the granule miss the bridge */
extern bool sift_f2_bridge_granule_excluded(const SiftTidBridge *br, bool have_tix, bool remapped, uint64 tid_min, uint64 tid_max, uint64 blk_bloom);

/*
 * active (a bit for each of the n rows whose addresses are tids) keeps the rows that are alive and in the bridge: dead (bit r: row r is deleted; NULL for none) and pend_dead (a pending
 * delete names the tid; NULL for none) leave first. scores[r] is the score of a kept row (n entries). Returns the rows kept.
 */
extern int	sift_f2_bridge_probe_rows(const SiftTidBridge *br, const uint64 *tids, int n, const uint64 *dead, SiftBridgeDeadFn pend_dead, void *pend_arg, uint64 *active, float4 *scores);

/* The rows of a slab of the L0 that are in the bridge: mask gets a bit for each of the n rows, scores[r] their scores; returns the rows kept */
extern int	sift_f2_bridge_slab_rows(const SiftTidBridge *br, const uint64 *tids, int n, uint64 *mask, float4 *scores);

#endif							/* SIFT_F2_SCAN_BRIDGE_CORE_H */
