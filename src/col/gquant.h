/*
 * col/gquant.h - the quantized keys of a compact chunk of a global order
 *
 * A compact chunk (col/gorder.h, version 4) keeps the exact key of its first entry in the directory and, for every other entry i, the bucket
 * q_i = (key_i - first) >> sh of its key (non-decreasing), as the Rice coded difference of q_i and q_(i-1): the key lies in [kmin_i, kmax_i] = first + (q_i << sh) + [0, 2^sh - 1].
 * Blob: byte 0 sh, byte 1 rb (the Rice parameter), then from bit 16 for each entry after the first u = min(d >> rb, SIFT_GQ_ESC) ones and a zero, and then the rb low bits of d, or, for
 * u = SIFT_GQ_ESC, 6 bits of bitlen(d) - 1 and d in bitlen(d) bits. LSB first. The decoder reads 8 bytes at a time: it needs 8 bytes of the block past the last bit.
 */
#ifndef SIFT_COL_GQUANT_H
#define SIFT_COL_GQUANT_H

#include "core/host.h"

#define SIFT_GQ_ESC		20			/* the longest unary run: a difference whose quotient reaches it is escaped */
#define SIFT_GQ_HDR		2
#define SIFT_GQ_Q		4			/* buckets per mean gap of the keys of a chunk: the writer's choice, nothing reads it */

/* Appends the blob of key[0..m) (ascending) to out and returns its bytes; q buckets per mean gap */
extern uint32 sift_gq_encode(const int64 *key, uint32 m, uint32 q, StringInfo out);
/* kmin[0..m) of a blob of len bytes whose chunk starts at first, and its shift; false when the blob is malformed */
extern bool sift_gq_decode(const uint8 *b, uint32 len, uint32 m, int64 first, int64 *kmin, uint32 *sh);
/* The largest key of the bucket that starts at kmin (saturating at INT64_MAX) */
extern int64 sift_gq_kmax(int64 kmin, uint32 sh);

#endif							/* SIFT_COL_GQUANT_H */
