/*
 * Internal kernel table and the generic group kernels.  Each ISA file sets a
 * target pragma, defines KFN(name) plus the wide-bitmap primitives
 * (w >= 16, multiple of 8 bytes) and includes this file with SIFT_KERN_TMPL
 * defined; it may then override table entries with hand-written kernels.
 */
#ifndef SIFT_BITMAP_KERN_H
#define SIFT_BITMAP_KERN_H

#include <string.h>
#include "sift_bitmap.h"

typedef struct {
	uint64_t (*popcount)(const uint8_t *p, size_t n);
	void (*and2)(const sift_grp *a, const sift_grp *b, const uint64_t *mask, sift_grp *o, uint8_t *buf);
	void (*or2)(const sift_grp *a, const sift_grp *b, sift_grp *o, uint8_t *buf);
	void (*andnot)(const sift_grp *a, const sift_grp *b, sift_grp *o, uint8_t *buf);
	uint64_t (*and_count)(const sift_grp *a, const sift_grp *b);
	uint64_t (*count_mask)(const sift_grp *g, const uint64_t *mask);
	uint32_t (*tids)(const sift_grp *g, const uint64_t *mask, sift_tid *out);
	uint32_t (*ranks)(const sift_grp *src, const sift_grp *sub, uint32_t *rank);
	void (*vop)(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t nwords, int op);
	void (*bm25)(float *score, const float *kn, const float *tf, const uint32_t *pos, uint32_t n, float w);
} sift_kernels;

enum { SIFT_VOP_AND, SIFT_VOP_OR, SIFT_VOP_ANDNOT };

extern const sift_kernels sift_kern_scalar, sift_kern_avx2, sift_kern_avx512;
extern const sift_kernels *sift_k;

#define SIFT_INLINE static inline __attribute__((always_inline))

SIFT_INLINE uint64_t sift_ld8(const uint8_t *p) { uint64_t x; memcpy(&x, p, 8); return x; }
SIFT_INLINE void sift_st8(uint8_t *p, uint64_t x) { memcpy(p, &x, 8); }

/* load/store a narrow (1, 2, 4, 8 byte) offset bitmap */
SIFT_INLINE uint64_t sift_ldn(const uint8_t *p, uint32_t w)
{
	uint16_t h;
	uint32_t s;
	switch (w) {
	case 1: return *p;
	case 2: memcpy(&h, p, 2); return h;
	case 4: memcpy(&s, p, 4); return s;
	default: return sift_ld8(p);
	}
}

SIFT_INLINE void sift_stn(uint8_t *p, uint32_t w, uint64_t x)
{
	uint16_t h = (uint16_t)x;
	uint32_t s = (uint32_t)x;
	switch (w) {
	case 1: *p = (uint8_t)x; break;
	case 2: memcpy(p, &h, 2); break;
	case 4: memcpy(p, &s, 4); break;
	default: sift_st8(p, x);
	}
}

/* 64-bit word q of a bitmap of w bytes, zero past its end or when p is NULL */
SIFT_INLINE uint64_t sift_ldq(const uint8_t *p, uint32_t w, uint32_t q)
{
	return !p || q >= w ? 0 : w < 8 ? sift_ldn(p, w) : sift_ld8(p + q);
}

/* portable wide-bitmap primitives (w >= 16, multiple of 8) */
SIFT_INLINE int sift_and_wide_gen(uint8_t *d, const uint8_t *a, const uint8_t *b, uint32_t w)
{
	uint64_t acc = 0;
	for (uint32_t q = 0; q < w; q += 8) {
		uint64_t x = sift_ld8(a + q) & sift_ld8(b + q);
		sift_st8(d + q, x);
		acc |= x;
	}
	return acc != 0;
}

SIFT_INLINE void sift_or_wide_gen(uint8_t *d, const uint8_t *a, uint32_t wa, const uint8_t *b, uint32_t wb, uint32_t w)
{
	for (uint32_t q = 0; q < w; q += 8) sift_st8(d + q, sift_ldq(a, wa, q) | sift_ldq(b, wb, q));
}

SIFT_INLINE int sift_andnot_wide_gen(uint8_t *d, const uint8_t *a, const uint8_t *b, uint32_t wb, uint32_t w)
{
	uint64_t acc = 0;
	for (uint32_t q = 0; q < w; q += 8) {
		uint64_t x = sift_ld8(a + q) & ~sift_ldq(b, wb, q);
		sift_st8(d + q, x);
		acc |= x;
	}
	return acc != 0;
}

SIFT_INLINE uint64_t sift_cnt_wide_gen(const uint8_t *a, const uint8_t *b, uint32_t w)
{
	uint64_t c = 0;
	for (uint32_t q = 0; q < w; q += 8) c += (uint64_t)__builtin_popcountll(sift_ld8(a + q) & sift_ld8(b + q));
	return c;
}

SIFT_INLINE void sift_grp_set(sift_grp *o, uint32_t gid, uint32_t w, const uint64_t *pages, const uint8_t *off)
{
	o->gid = gid, o->w = w, o->base = 0, o->n = 0, o->off = off;
	memcpy(o->pages, pages, 32);
}

#endif

#ifdef SIFT_KERN_TMPL

SIFT_INLINE uint32_t KFN(pc)(uint64_t x) { return (uint32_t)__builtin_popcountll(x); }

static uint64_t KFN(popcount)(const uint8_t *p, size_t n)
{
	uint64_t c = 0;
	size_t i = 0;
	for (; i + 8 <= n; i += 8) c += KFN(pc)(sift_ld8(p + i));
	for (; i < n; i++) c += KFN(pc)(p[i]);
	return c;
}

SIFT_INLINE uint64_t KFN(pcw)(const uint8_t *p, uint32_t w)
{
	return w <= 8 ? KFN(pc)(sift_ldn(p, w)) : sift_k->popcount(p, w);
}

SIFT_INLINE void KFN(and2_n)(const sift_grp *a, const sift_grp *b, const uint64_t *m, sift_grp *o, uint8_t *buf, const uint32_t w)
{
	uint64_t pages[4] = { 0 };
	uint32_t ra = 0, rb = 0, k = 0, wa = a->w, wb = b->w;
	for (int j = 0; j < 4; j++) {
		uint64_t pa = a->pages[j], pb = b->pages[j];
		for (uint64_t mm = pa & pb & (m ? m[j] : ~0ull); mm; mm &= mm - 1) {
			uint64_t bit = mm & -mm;
			const uint8_t *xa = a->off + (ra + KFN(pc)(pa & (bit - 1))) * wa;
			const uint8_t *xb = b->off + (rb + KFN(pc)(pb & (bit - 1))) * wb;
			if (w <= 8) {
				uint64_t x = sift_ldn(xa, w) & sift_ldn(xb, w);
				if (x) sift_stn(buf + k++ * w, w, x), pages[j] |= bit;
			} else if (KFN(and_wide)(buf + k * w, xa, xb, w))
				k++, pages[j] |= bit;
		}
		ra += KFN(pc)(pa), rb += KFN(pc)(pb);
	}
	sift_grp_set(o, a->gid, w, pages, buf);
}

static void KFN(and2)(const sift_grp *a, const sift_grp *b, const uint64_t *m, sift_grp *o, uint8_t *buf)
{
	switch (a->w < b->w ? a->w : b->w) {
	case 1: KFN(and2_n)(a, b, m, o, buf, 1); break;
	case 2: KFN(and2_n)(a, b, m, o, buf, 2); break;
	case 4: KFN(and2_n)(a, b, m, o, buf, 4); break;
	case 8: KFN(and2_n)(a, b, m, o, buf, 8); break;
	default: KFN(and2_n)(a, b, m, o, buf, a->w < b->w ? a->w : b->w);
	}
}

static uint64_t KFN(and_count)(const sift_grp *a, const sift_grp *b)
{
	uint64_t c = 0;
	uint32_t ra = 0, rb = 0, wa = a->w, wb = b->w, w = wa < wb ? wa : wb;
	for (int j = 0; j < 4; j++) {
		uint64_t pa = a->pages[j], pb = b->pages[j];
		for (uint64_t mm = pa & pb; mm; mm &= mm - 1) {
			uint64_t below = (mm & -mm) - 1;
			const uint8_t *xa = a->off + (ra + KFN(pc)(pa & below)) * wa;
			const uint8_t *xb = b->off + (rb + KFN(pc)(pb & below)) * wb;
			c += w <= 8 ? KFN(pc)(sift_ldn(xa, w) & sift_ldn(xb, w)) : KFN(cnt_wide)(xa, xb, w);
		}
		ra += KFN(pc)(pa), rb += KFN(pc)(pb);
	}
	return c;
}

static void KFN(or2)(const sift_grp *a, const sift_grp *b, sift_grp *o, uint8_t *buf)
{
	uint64_t pages[4];
	uint32_t ia = 0, ib = 0, k = 0, wa = a->w, wb = b->w, w = wa > wb ? wa : wb;
	for (int j = 0; j < 4; j++) {
		uint64_t pa = a->pages[j], pb = b->pages[j];
		pages[j] = pa | pb;
		for (uint64_t mm = pa | pb; mm; mm &= mm - 1, k++) {
			uint64_t bit = mm & -mm;
			const uint8_t *xa = pa & bit ? a->off + ia++ * wa : NULL;
			const uint8_t *xb = pb & bit ? b->off + ib++ * wb : NULL;
			if (w <= 8)
				sift_stn(buf + k * w, w, (xa ? sift_ldn(xa, wa) : 0) | (xb ? sift_ldn(xb, wb) : 0));
			else
				KFN(or_wide)(buf + k * w, xa, wa, xb, wb, w);
		}
	}
	sift_grp_set(o, a->gid, w, pages, buf);
}

static void KFN(andnot)(const sift_grp *a, const sift_grp *b, sift_grp *o, uint8_t *buf)
{
	uint64_t pages[4] = { 0 };
	uint32_t ia = 0, rb = 0, k = 0, w = a->w, wb = b->w, wm = w < wb ? w : wb;
	for (int j = 0; j < 4; j++) {
		uint64_t pa = a->pages[j], pb = b->pages[j];
		for (uint64_t mm = pa; mm; mm &= mm - 1, ia++) {
			uint64_t bit = mm & -mm;
			const uint8_t *xa = a->off + ia * w;
			const uint8_t *xb = pb & bit ? b->off + (rb + KFN(pc)(pb & (bit - 1))) * wb : NULL;
			if (w <= 8) {
				uint64_t x = sift_ldn(xa, w) & ~(xb ? sift_ldn(xb, wm) : 0);
				if (x) sift_stn(buf + k++ * w, w, x), pages[j] |= bit;
			} else if (KFN(andnot_wide)(buf + k * w, xa, xb, wm, w))
				k++, pages[j] |= bit;
		}
		rb += KFN(pc)(pb);
	}
	sift_grp_set(o, a->gid, w, pages, buf);
}

static uint64_t KFN(count_mask)(const sift_grp *g, const uint64_t *m)
{
	uint64_t c = 0;
	uint32_t r = 0, w = g->w;
	for (int j = 0; j < 4; j++) {
		uint64_t p = g->pages[j], mm = m ? p & m[j] : p;
		if (mm == p)
			c += sift_k->popcount(g->off + r * w, (size_t)KFN(pc)(p) * w);
		else
			for (; mm; mm &= mm - 1)
				c += sift_k->popcount(g->off + (r + KFN(pc)(p & ((mm & -mm) - 1))) * w, w);
		r += KFN(pc)(p);
	}
	return c;
}

SIFT_INLINE uint32_t KFN(emit)(const uint8_t *x, uint32_t w, uint64_t blk, sift_tid *out)
{
	uint32_t n = 0;
	for (uint32_t q = 0; q < w; q += 8)
		for (uint64_t v = w < 8 ? sift_ldn(x, w) : sift_ld8(x + q); v; v &= v - 1)
			out[n++] = blk | (q * 8 + (uint32_t)__builtin_ctzll(v) + 1);
	return n;
}

SIFT_INLINE uint32_t KFN(tids_w)(const sift_grp *g, const uint64_t *m, sift_tid *out, const uint32_t w)
{
	uint32_t r = 0, n = 0;
	uint64_t gbase = (uint64_t)g->gid << (SIFT_GROUP_SHIFT + 16);
	for (int j = 0; j < 4; j++) {
		uint64_t p = g->pages[j], mm = m ? p & m[j] : p;
		if (mm == p)
			for (; mm; mm &= mm - 1, r++)
				n += KFN(emit)(g->off + r * w, w, gbase | (uint64_t)(j * 64 + __builtin_ctzll(mm)) << 16, out + n);
		else {
			for (; mm; mm &= mm - 1)
				n += KFN(emit)(g->off + (r + KFN(pc)(p & ((mm & -mm) - 1))) * w, w,
					       gbase | (uint64_t)(j * 64 + __builtin_ctzll(mm)) << 16, out + n);
			r += KFN(pc)(p);
		}
	}
	return n;
}

static uint32_t KFN(tids)(const sift_grp *g, const uint64_t *m, sift_tid *out)
{
	switch (g->w) {
	case 1: return KFN(tids_w)(g, m, out, 1);
	case 2: return KFN(tids_w)(g, m, out, 2);
	case 4: return KFN(tids_w)(g, m, out, 4);
	case 8: return KFN(tids_w)(g, m, out, 8);
	default: return KFN(tids_w)(g, m, out, g->w);
	}
}

static uint32_t KFN(ranks)(const sift_grp *src, const sift_grp *sub, uint32_t *rank)
{
	uint32_t rs = 0, done = 0, n = 0, w = src->w, ws = sub->w, ks = 0;
	uint64_t cnt = src->base;
	for (int j = 0; j < 4; j++) {
		uint64_t p = src->pages[j];
		for (uint64_t mm = sub->pages[j]; mm; mm &= mm - 1, ks++) {
			uint32_t is = rs + KFN(pc)(p & ((mm & -mm) - 1));
			const uint8_t *xs = src->off + is * w, *xu = sub->off + ks * ws;
			cnt += sift_k->popcount(src->off + done * w, (size_t)(is - done) * w);
			done = is;
			uint32_t before = 0;
			for (uint32_t q = 0; q < ws; q += 8) {
				uint64_t vs = ws < 8 ? sift_ldn(xs, ws) : sift_ld8(xs + q);
				for (uint64_t v = ws < 8 ? sift_ldn(xu, ws) : sift_ld8(xu + q); v; v &= v - 1)
					rank[n++] = (uint32_t)(cnt + before + KFN(pc)(vs & ((v & -v) - 1)));
				before += KFN(pc)(vs);
			}
		}
		rs += KFN(pc)(p);
	}
	return n;
}

static void KFN(vop)(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t n, int op)
{
	switch (op) {
	case SIFT_VOP_AND: for (size_t i = 0; i < n; i++) o[i] = a[i] & b[i]; break;
	case SIFT_VOP_OR: for (size_t i = 0; i < n; i++) o[i] = a[i] | b[i]; break;
	default: for (size_t i = 0; i < n; i++) o[i] = a[i] & ~b[i];
	}
}

/* score[pos[i]] (or score[i]) += w * (tf[i] / (tf[i] + kn[...])): the exact BM25 op order */
static void KFN(bm25)(float *score, const float *kn, const float *tf, const uint32_t *pos, uint32_t n, float w)
{
	if (pos)
		for (uint32_t i = 0; i < n; i++) score[pos[i]] += w * (tf[i] / (tf[i] + kn[pos[i]]));
	else
		for (uint32_t i = 0; i < n; i++) score[i] += w * (tf[i] / (tf[i] + kn[i]));
}

#define SIFT_KERN_TABLE                                                                                       \
	{ KFN(popcount), KFN(and2), KFN(or2), KFN(andnot), KFN(and_count), KFN(count_mask), KFN(tids), KFN(ranks), \
	  KFN(vop), KFN(bm25) }

#endif
