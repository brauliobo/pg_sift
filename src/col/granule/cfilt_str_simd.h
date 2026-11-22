/*
 * col/granule/cfilt_str_simd.h - the string predicates the chunks of a granule answer without a row at a time: <> on the lengths of the strings, and LIKE '%lit%', ILIKE '%lit%' and NOT LIKE '%lit%'
 * on one sweep over the bytes of the whole chunk (every string of it is one slab), the string-predicate kernels of col/granule/granule_filter.c.
 */
#ifndef SIFT_GRANULE_CFILT_STR_SIMD_H
#define SIFT_GRANULE_CFILT_STR_SIMD_H

#include "col/am.h"
#include "col/granule/granule_filter.h"
#include "col/like_simd.h"

/* bit i of bits (ceil(n / 64) words, written whole) is set when len[i] != 0: a string is empty exactly when its length is 0 */
extern void sift_cfs_len_nonzero(const uint32 *len, uint32 n, uint64 *bits);

/* The strings p[i] of length len[i] (i < n) lie one after the other: off gets their n + 1 offsets from *base, the first byte of the first string that has one, and the result is true */
extern bool sift_cfs_slab(const char *const *p, const uint32 *len, uint32 n, uint32 *off, const char **base);

/* The bits of the n strings (ceil(n / 64) words, written whole) that hold needle nd (nd->fold: ILIKE), found in one sweep when the strings are a slab; returns how many */
extern uint32 sift_cfs_contains(const SiftLikeNeedle *nd, uint32 n, const char *const *p, const uint32 *len, uint64 *bits);

/* bits := the complement of bits over its first n bits, the bits past n zero */
extern void sift_cfs_invert(uint64 *bits, uint32 n);

/* The strings that differ from lit (n of them): the complement of those that equal it */
extern void sift_cfs_ne(const char *lit, uint32 litlen, uint32 n, const char *const *p, const uint32 *len, uint64 *bits);

/*
 * The rows of a string chunk that pass a <>, LIKE '%lit%', ILIKE '%lit%' or NOT LIKE '%lit%' predicate, as one bit for each value of the chunk (SIFT_CENC_STR_FSST, _BLOCK, _INLINE and _VIEW
 * chunks, decoded once for the whole chunk). false: the predicate or the chunk is one the caller decodes row by row.
 */
extern bool sift_cfstr_bits(const SiftCfiltPred *cp, const SiftChunk *c, uint64 *dense);

#endif							/* SIFT_GRANULE_CFILT_STR_SIMD_H */
