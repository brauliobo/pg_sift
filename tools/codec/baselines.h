/*
 * Comparison baselines on doc ordinals (doc-id mapped engines):
 * Lucene-style FOR blocks of 128 deltas with a skip entry per block, and
 * CRoaring (optional: build with ROARING=dir holding roaring.c/roaring.h).
 */
#ifndef SIFT_CODEC_BASELINES_H
#define SIFT_CODEC_BASELINES_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FOR_BLOCK 128

typedef struct {
	uint32_t n, nb;
	uint32_t *last, *off;   /* skip entry per block: last doc, data offset */
	uint8_t *bits, *data;
} forlist;

static inline uint32_t for_nbits(uint32_t x) { return x ? 32 - (uint32_t)__builtin_clz(x) : 0; }

static size_t for_encode(forlist *f, const uint32_t *d, uint32_t n)
{
	f->n = n, f->nb = (n + FOR_BLOCK - 1) / FOR_BLOCK;
	f->last = malloc(f->nb * 4), f->off = malloc(f->nb * 4), f->bits = malloc(f->nb);
	f->data = calloc((size_t)n * 4 + 16, 1);
	size_t pos = 0;
	for (uint32_t k = 0; k < f->nb; k++) {
		uint32_t i0 = k * FOR_BLOCK, cnt = n - i0 < FOR_BLOCK ? n - i0 : FOR_BLOCK, prev = k ? f->last[k - 1] : 0, b = 0;
		for (uint32_t i = 0; i < cnt; i++) b = for_nbits(d[i0 + i] - (i0 + i ? d[i0 + i - 1] : 0)) > b ? for_nbits(d[i0 + i] - (i0 + i ? d[i0 + i - 1] : 0)) : b;
		f->bits[k] = (uint8_t)b, f->off[k] = (uint32_t)pos, f->last[k] = d[i0 + cnt - 1];
		uint64_t bit = pos * 8;
		for (uint32_t i = 0; i < cnt; i++, bit += b) {
			uint64_t v = d[i0 + i] - (i ? d[i0 + i - 1] : prev), x;
			memcpy(&x, f->data + (bit >> 3), 8);
			x |= v << (bit & 7);
			memcpy(f->data + (bit >> 3), &x, 8);
		}
		pos += ((size_t)cnt * b + 7) / 8;
	}
	/* data + 1 B width + ~6 B skip entry (vint last doc + vint pointer) per block, as Lucene */
	return pos + (size_t)f->nb * 7;
}

static inline __attribute__((always_inline)) void for_unpack_b(const uint8_t *p, uint32_t cnt, uint32_t base, uint32_t *out, const uint32_t b)
{
	uint64_t mask = (1ull << b) - 1, bit = 0;
	for (uint32_t i = 0; i < cnt; i++, bit += b) {
		uint64_t x;
		memcpy(&x, p + (bit >> 3), 8);
		base += (uint32_t)((x >> (bit & 7)) & mask);
		out[i] = base;
	}
}

/* decode block k into out (docs), returns count */
static uint32_t for_block(const forlist *f, uint32_t k, uint32_t *out)
{
	uint32_t cnt = f->n - k * FOR_BLOCK < FOR_BLOCK ? f->n - k * FOR_BLOCK : FOR_BLOCK, base = k ? f->last[k - 1] : 0;
	const uint8_t *p = f->data + f->off[k];
	switch (f->bits[k]) {
#define B(n) case n: for_unpack_b(p, cnt, base, out, n); break;
	B(0) B(1) B(2) B(3) B(4) B(5) B(6) B(7) B(8) B(9) B(10) B(11) B(12) B(13) B(14) B(15) B(16)
	B(17) B(18) B(19) B(20) B(21) B(22) B(23) B(24) B(25) B(26) B(27) B(28) B(29) B(30) B(31) B(32)
#undef B
	}
	return cnt;
}

typedef struct { const forlist *f; uint32_t k, i, cnt; uint32_t buf[FOR_BLOCK]; } for_it;

static inline void for_it_init(for_it *it, const forlist *f) { it->f = f, it->k = 0, it->i = 0, it->cnt = f->nb ? for_block(f, 0, it->buf) : 0; }

/* first doc >= target (skip whole blocks via their skip entry), UINT32_MAX at end */
static inline uint32_t for_advance(for_it *it, uint32_t target)
{
	const forlist *f = it->f;
	if (it->k >= f->nb) return UINT32_MAX;
	if (f->last[it->k] < target) {
		uint32_t k = it->k + 1;
		while (k < f->nb && f->last[k] < target) k++;
		if (k == f->nb) return it->k = k, UINT32_MAX;
		it->k = k, it->i = 0, it->cnt = for_block(f, k, it->buf);
	}
	while (it->buf[it->i] < target) it->i++;
	return it->buf[it->i];
}

static inline uint32_t for_next(for_it *it)
{
	if (++it->i < it->cnt) return it->buf[it->i];
	if (++it->k >= it->f->nb) return UINT32_MAX;
	it->i = 0, it->cnt = for_block(it->f, it->k, it->buf);
	return it->buf[0];
}

static inline uint32_t for_cur(const for_it *it) { return it->k < it->f->nb ? it->buf[it->i] : UINT32_MAX; }

/* conjunction of k lists led by the shortest (Lucene ConjunctionDISI); out may be NULL */
static uint64_t for_and(const forlist **l, int k, const uint64_t *doc2tid, uint64_t *out)
{
	for_it it[4];
	for (int i = 0; i < k; i++) for_it_init(&it[i], l[i]);
	uint64_t n = 0;
	for (uint32_t d = for_cur(&it[0]); d != UINT32_MAX;) {
		int i = 1;
		for (; i < k; i++) {
			uint32_t x = for_advance(&it[i], d);
			if (x != d) break;
		}
		if (i == k) {
			if (out) out[n] = doc2tid[d];
			n++;
			d = for_next(&it[0]);
		} else {
			uint32_t x = for_cur(&it[i]);
			if (x == UINT32_MAX) break;
			d = for_advance(&it[0], x);
		}
	}
	return n;
}

static uint64_t for_or(const forlist *a, const forlist *b, const uint64_t *doc2tid, uint64_t *out)
{
	for_it x, y;
	for_it_init(&x, a), for_it_init(&y, b);
	uint64_t n = 0;
	for (uint32_t u = for_cur(&x), v = for_cur(&y); u != UINT32_MAX || v != UINT32_MAX; n++) {
		uint32_t d = u < v ? u : v;
		if (out) out[n] = doc2tid[d];
		if (u == d) u = for_next(&x);
		if (v == d) v = for_next(&y);
	}
	return n;
}

static uint64_t for_decode(const forlist *f, const uint64_t *doc2tid, uint64_t *out)
{
	uint32_t buf[FOR_BLOCK];
	uint64_t n = 0;
	for (uint32_t k = 0; k < f->nb; k++) {
		uint32_t cnt = for_block(f, k, buf);
		for (uint32_t i = 0; i < cnt; i++) out[n++] = doc2tid[buf[i]];
	}
	return n;
}

#endif
