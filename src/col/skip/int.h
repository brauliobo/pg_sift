/*
 * col/skip/int.h - what the files of the skipping indexes share (col/skip_index.h): the hashes of the keys and the folded windows of the strings, which the writer and the probe must agree on.
 */
#ifndef SIFT_SKIP_INT_H
#define SIFT_SKIP_INT_H

#include "core/host.h"

#include "col/granule/int.h"
#include "col/skip/skip.h"
#include "core/hash.h"
#include "core/sbbf.h"

/* Bit 0 of every hash is set: the tables of distinct hashes of the writer use 0 for a free slot */
static inline uint64
sift_skip_hash_i64(int64 v)
{
	return sift_sbbf_hash_i64(v) | 1;
}

static inline uint64
sift_skip_hash_bytes(const void *p, uint32 len)
{
	return sift_sbbf_hash_bytes(p, len) | 1;
}

static inline uint8
sift_skip_fold(uint8 b)
{
	return (b >= 'A' && b <= 'Z') ? b | 0x20 : b;
}

/* The three bytes at p as one number, the ASCII capitals folded to lower case */
static inline uint32
sift_skip_trigram(const char *p)
{
	return ((uint32) sift_skip_fold((uint8) p[0]) << 16) | ((uint32) sift_skip_fold((uint8) p[1]) << 8) | sift_skip_fold((uint8) p[2]);
}

static inline uint64
sift_skip_hash_trigram(uint32 t)
{
	return sift_fmix64((uint64) t + SIFT_FIB64) | 1;
}

#endif							/* SIFT_SKIP_INT_H */
