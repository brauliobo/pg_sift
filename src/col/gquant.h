/*
 * col/gquant.h - the quantized keys of a compact chunk of a global order
 *
 * A compact chunk (col/gorder.h, version 4) keeps the exact key of its first entry in the directory and, for every other entry i, the bucket
 * q_i = (key_i - first) >> sh of its key (non-decreasing): the key lies in [kmin_i, kmax_i] = first + (q_i << sh) + [0, 2^sh - 1].
 * Blob: byte 0 sh, then one mini-block for every 32 entries (the last one padded with zero differences): a byte w (bit 7: some differences are patched), the differences
 * d_i = q_i - q_(i-1) (d_0 = 0) of its 32 entries in w bits each, LSB first, 4 * w bytes, and, if patched, the number n of patched ones, the width e of what they hold above the w bits, their
 * positions (n bytes) and what they hold (n * e bits, LSB first). A writer picks w for each mini-block so as to be shortest, so a run of equal keys is one byte and the buckets of
 * a block of keys that spread evenly take about 5 bits an entry. q is a 32 bit sum (it is below 2^15 for the writer's choice of sh), and the decode of a damaged blob is the same
 * at every SIMD level. What the decoders guarantee for an entry is its bucket and nothing of its row: kmin_i is a lower bound of the key the writer saw (the key itself for entry 0, and for
 * an entry alone in its bucket or a block written with sh = 0), kmax_i an upper bound, the same bits at every level and for a damaged blob (or false). An entry whose row is gone has no key
 * in the heap to read; what it reads as is the reader's rule, from its bucket. The decoders read 16 bytes past the last one of the blob (SIFT_COL_PAD of the block), and write the keys of whole mini-blocks: kmin has room for m rounded up to 32.
 */
#ifndef SIFT_COL_GQUANT_H
#define SIFT_COL_GQUANT_H

#include "core/host.h"
#include "core/cpu.h"

#define SIFT_GQ_MB		32			/* entries of a mini-block */
#define SIFT_GQ_Q		4			/* buckets per mean gap of the keys of a chunk: the writer's choice, nothing reads it */

/* Appends the blob of key[0..m) (ascending) to out and returns its bytes; q buckets per mean gap */
extern uint32 sift_gq_encode(const int64 *key, uint32 m, uint32 q, StringInfo out);
/* kmin[0..m) of a blob of len bytes whose chunk starts at first, and its shift; false when the blob is malformed */
typedef bool (*SiftGqDecode) (const uint8 *b, uint32 len, uint32 m, int64 first, int64 *kmin, uint32 *sh);
extern bool sift_gq_decode(const uint8 *b, uint32 len, uint32 m, int64 first, int64 *kmin, uint32 *sh);
/* The decoder of a SIMD level (scalar, AVX2 or NEON; the levels above it run its decoder), for the tests that compare them */
extern SiftGqDecode sift_gq_decode_for(SiftSimdLevel level);
/* The largest key of the bucket that starts at kmin (saturating at INT64_MAX) */
extern int64 sift_gq_kmax(int64 kmin, uint32 sh);

#endif							/* SIFT_COL_GQUANT_H */
