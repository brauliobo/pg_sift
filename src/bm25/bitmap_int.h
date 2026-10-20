/*
 * bm25/bitmap_int.h - shared between bitmap.c, blob.c and topk.c.
 */
#ifndef SIFT_BITMAP_INT_H
#define SIFT_BITMAP_INT_H

#include "bm25/bitmap_kern.h"

static inline uint32_t ld32(const uint8_t *p) { uint32_t x; memcpy(&x, p, 4); return x; }
static inline void st32(uint8_t *p, uint32_t x) { memcpy(p, &x, 4); }
static inline uint32_t nbits(uint64_t x) { return x ? 64 - (uint32_t)__builtin_clzll(x) : 0; }
static inline int pages_empty(const uint64_t *p) { return !(p[0] | p[1] | p[2] | p[3]); }

/*
 * PACKED container (type byte 0xE0 | index of w in packed_w), for a group of n postings: u8 ob, u8 pages[32] (the pages that have
 * postings), n offsets (off - 1) of ob bits each in posting order, then n bits, bit k set when posting k is the last of its page
 * (a page of c postings is c - 1 zeros and a one). 33 + ceil(n * ob / 8) + ceil(n / 8) bytes: about 9 bits per posting where an
 * ARRAY takes 16.
 */
static const uint8_t packed_w[] = { 1, 2, 4, 8, 16, 24, 32, 40, 48, 56, 64 };
#define PACKED_TYPE 0xE0

static inline uint32_t packed_w_of(uint32_t type) { return (type & 0x1f) < sizeof packed_w ? packed_w[type & 0x1f] : 0; }

static inline size_t packed_bytes(uint32_t n, uint32_t ob) { return 33 + ((size_t)n * ob + 7) / 8 + (n + 7) / 8; }

/* LIST: Elias-Fano blocks over key = blk << ob | (off - 1) */
typedef struct { uint64_t x0, u; uint32_t cnt, l; const uint8_t *lows, *highs, *end; } ef_blk;

static inline uint32_t ef_l(uint64_t u, uint32_t cnt) { return u > cnt ? nbits(u / cnt) - 1 : 0; }
static inline size_t ef_lows(uint32_t cnt, uint32_t l) { return ((size_t)cnt * l + 7) / 8; }
static inline size_t ef_highs(uint64_t u, uint32_t cnt, uint32_t l) { return (cnt + ((u - 1) >> l) + 7) / 8; }
static inline uint32_t blk_cnt(uint32_t n, uint32_t i) { return n - i < SIFT_LIST_BLOCK ? n - i : SIFT_LIST_BLOCK; }

int and_align(sift_cur **kid, int k, uint32_t *target, uint64_t m[4]);

#endif
