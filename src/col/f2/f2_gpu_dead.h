/*
 * col/f2/f2_gpu_dead.h - the rows of a granule the snapshot sees deleted, as the mask of dead rows the device takes (SiftVkGranule.dead, 1024 bytes, a bit for each row).
 *
 * A deletion vector (col/f2/delvec.h) and the pending deletes of the partition (col/f2/pend_del.h) both end up in the one mask, so that a part that has either is read by the device with the
 * rows the snapshot sees deleted left out, exactly as the CPU scan leaves them out. Pure: no server call.
 */
#ifndef SIFT_COL_F2_GPU_DEAD_H
#define SIFT_COL_F2_GPU_DEAD_H

#include "postgres.h"

#include "col/f2/delvec.h"
#include "col/f2/pend_del.h"

/* The mask of 128 words with the bit of every row whose address is in px (tids: the addresses of the nrows rows of the granule); returns the bits set in the mask, those it held before included */
extern uint32 sift_gpu_dead_add_tids(uint64 *mask, const uint64 *tids, uint32 nrows, const SiftPendDelIndex *px);

/* The bits set in the mask of SIFT_DV_MASK_WORDS words */
extern uint32 sift_gpu_dead_count(const uint64 *mask);

#endif							/* SIFT_COL_F2_GPU_DEAD_H */
