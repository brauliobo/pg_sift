/*
 * core/vis_f2.h - the heap's visibility map for the rows of a granule of format major 2.
 *
 * A scan that answers from the index alone (a count, an index-only scan) asks the heap only about the rows on pages that are not all-visible. A granule is sorted by the index's key
 * and not by address, so its rows are on pages in no order: one granule is checked against the map by the range of its addresses (a granule whose range holds no page that is not
 * all-visible costs no address), else block by block, grouped so that each map page is read once (one covers 32,672 heap pages, 255 MB), and only the rows on pages that are not
 * all-visible reach the heap, their pages read ahead.
 *
 * The map is trusted only while the granule is current: a VACUUM that removed dead rows from the index marks their pages all-visible, and a scan that still reads the part from before
 * the removal would count them. VACUUM removes the rows of the index in a new deletion vector (a commit of the manifest) before it touches the map, so the scan reads the map first and
 * the manifest after: the part it reads is still in the manifest with its deletion vector (sift_vis_f2_part), or the map is not trusted for the part and the heap decides every row.
 */
#ifndef SIFT_VIS_F2_H
#define SIFT_VIS_F2_H

#include "core/host.h"

#include "core/f2/manifest/manifest.h"
#include "core/visibility/visibility.h"

typedef struct SiftVisF2Guard
{
	Relation	index;
	uint64		manifest_ver;	/* the version of the manifest the parts were read from, or the last one the part was found current in */
	uint64		part_id;
	uint8		key_lo[SIFT_BOUND_BYTES];
	uint64		part_gen;
	uint64		dv_gen;
	BlockNumber dv_blk;
	uint32		dv_len;			/* the bytes of a vector that takes delta tails when the part was begun: a delta changes them and not the manifest */
	bool		moved;			/* the part is no longer current: the map is not trusted for the rest of it */
} SiftVisF2Guard;

/* The scan reads the parts of the manifest of version manifest_ver (what it read the manifest at, before it read any part) */
extern void sift_vis_f2_begin(SiftVisF2Guard *g, Relation index, uint64 manifest_ver);

/* The part of partition e that the next granules are of (as the manifest listed it) */
extern void sift_vis_f2_part(SiftVisF2Guard *g, const SiftPartitionEntry *e, const SiftPartDesc *d);

/*
 * Keeps in sel (128 words, a bit for each row of the granule) the rows whose heap tuple the snapshot of v sees; tids: the address of each of the nrows rows; tid_min, tid_max: those
 * of the granule (its entry in SEC_TID_INDEX). v is begun by sift_vis_begin with no index. Returns the rows left.
 */
extern uint16 sift_vis_f2_granule_filter(SiftVis *v, SiftVisF2Guard *g, uint64 tid_min, uint64 tid_max, const uint64 *tids, uint16 nrows, uint64 *sel);

#endif							/* SIFT_VIS_F2_H */
