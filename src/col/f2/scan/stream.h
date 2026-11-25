/*
 * col/f2/scan/stream.h - the addresses of an unordered scan that returns no column (an index scan, or an index-only scan of no attribute), held in memory and handed to the executor in the order
 * of the addresses, in place of a sort that exists to spill. Past work_mem the scan moves to that sort (col/f2/scan/ret.c) with the addresses it holds.
 */
#ifndef SIFT_F2_SCAN_STREAM_H
#define SIFT_F2_SCAN_STREAM_H

#include "col/f2/scan/int.h"

/* key is the address and the flag of recheck as the sort has them (address << 1 | recheck); false when the stream holds as many as work_mem allows and key was not taken */
extern bool sift_f2_stream_put(F2Scan *s, uint64 key);

/* Every key held, in order, to put; the stream is empty after it */
extern void sift_f2_stream_drain(F2Scan *s, void (*put) (F2Scan *s, uint64 key));

/* Ends the collection: the keys are put in order */
extern void sift_f2_stream_finish(F2Scan *s);

/* The next key in the direction, false past the last */
extern bool sift_f2_stream_next(F2Scan *s, ScanDirection dir, uint64 *key);

#endif							/* SIFT_F2_SCAN_STREAM_H */
