/*
 * bm25/bitmap_int.h - shared between bitmap.c and blob.c.
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

static inline uint32_t ef_l(uint64_t u, uint32_t cnt) { return u > cnt ? nbits(u / cnt) - 1 : 0; }
static inline size_t ef_lows(uint32_t cnt, uint32_t l) { return ((size_t)cnt * l + 7) / 8; }
static inline size_t ef_highs(uint64_t u, uint32_t cnt, uint32_t l) { return (cnt + ((u - 1) >> l) + 7) / 8; }
static inline uint32_t blk_cnt(uint32_t n, uint32_t i) { return n - i < SIFT_LIST_BLOCK ? n - i : SIFT_LIST_BLOCK; }

/*
 * OR cursors of more than SIFT_OR_HEAP_MIN kids keep them in a binary min-heap on the group each is at (an unpositioned kid is
 * at 0, an exhausted one is out of the heap), so a seek moves only the kids that are behind the target, and the kids at the
 * smallest group are the ones at the top. The heap is a copy of the pointers: the kid array keeps its order.
 */

static inline int64_t or_cur_gid(const sift_cur *k) { return k->state == 1 ? (int64_t)k->g.gid : k->state == 0 ? -1 : INT64_MAX; }

static inline void or_sift_down(sift_cur **kid, int n, int i)
{
	sift_cur *x = kid[i];
	int64_t key = or_cur_gid(x);
	for (int c; (c = 2 * i + 1) < n; i = c) {
		if (c + 1 < n && or_cur_gid(kid[c + 1]) < or_cur_gid(kid[c])) c++;
		if (or_cur_gid(kid[c]) >= key) break;
		kid[i] = kid[c];
	}
	kid[i] = x;
}

/* The kids not exhausted as a heap in hp; returns how many */
static inline int or_heap_build(sift_cur **hp, sift_cur **kid, int k)
{
	int n = 0;
	for (int i = 0; i < k; i++)
		if (kid[i]->state != 2) hp[n++] = kid[i];
	for (int i = n / 2 - 1; i >= 0; i--) or_sift_down(hp, n, i);
	return n;
}

extern __attribute__((visibility("hidden"))) int and_align(sift_cur **kid, int k, uint32_t *target, uint64_t m[4]);

#endif
