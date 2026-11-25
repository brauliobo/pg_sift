/*
 * agg/gj_bits.h - one bit for each probe row of a partition of the Grace join (v6_grace_join.h) that is joined in chunks, in a temporary file with one window of it in memory: the bits are read
 * and set in the order of the rows, each pass over the probe file from its first row, so a window is read once for each pass whatever the number of rows.
 */
#ifndef SIFT_AGG_V6_GJ_BITS_H
#define SIFT_AGG_V6_GJ_BITS_H

#include "core/host.h"

typedef struct SiftGjBits SiftGjBits;

/* All bits clear, in a file that is closed with the bits */
extern SiftGjBits *sift_gj_bits_open(MemoryContext cx);
extern bool sift_gj_bits_get(SiftGjBits *b, uint64 ord);
extern void sift_gj_bits_set(SiftGjBits *b, uint64 ord);
extern void sift_gj_bits_close(SiftGjBits *b);

#endif							/* SIFT_AGG_V6_GJ_BITS_H */
