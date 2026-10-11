/*
 * AVX-512 (F/BW/VL/VBMI2/VPOPCNTDQ) kernels.  Narrow groups (equal w of
 * 1/2/4/8 bytes) are processed 64/32/16/8 pages per instruction: the packed
 * offset bitmaps are expanded into page lanes with VPEXPAND using the page
 * bitmap as mask, combined, and compacted back with VPCOMPRESS.  Register
 * forms plus masked loads/stores are used (memory-form expand/compress are
 * microcoded on Zen 4).
 */
#if defined(__x86_64__)
#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx512f,avx512bw,avx512vl,avx512vbmi2,avx512vpopcntdq,avx2,bmi,bmi2,popcnt"))), apply_to = function)
#else
#pragma GCC push_options
#pragma GCC target("avx512f,avx512bw,avx512vl,avx512vbmi2,avx512vpopcntdq,avx2,bmi,bmi2,popcnt")
#endif
#include <immintrin.h>

#define KFN(x) x##_avx512
#include "sift_bitmap_kern.h"

#define BZ(n) _bzhi_u64(~0ull, (uint32_t)(n))

SIFT_INLINE __m512i ldb(const uint8_t *p, uint32_t n) { return p ? _mm512_maskz_loadu_epi8(BZ(n), p) : _mm512_setzero_si512(); }

SIFT_INLINE int and_wide_avx512(uint8_t *d, const uint8_t *a, const uint8_t *b, uint32_t w)
{
	__mmask8 k = (__mmask8)BZ(w / 8);
	__m512i x = _mm512_and_si512(_mm512_maskz_loadu_epi64(k, a), _mm512_maskz_loadu_epi64(k, b));
	_mm512_mask_storeu_epi64(d, k, x);
	return _mm512_test_epi64_mask(x, x) != 0;
}

SIFT_INLINE void or_wide_avx512(uint8_t *d, const uint8_t *a, uint32_t wa, const uint8_t *b, uint32_t wb, uint32_t w)
{
	_mm512_mask_storeu_epi8(d, BZ(w), _mm512_or_si512(ldb(a, wa), ldb(b, wb)));
}

SIFT_INLINE int andnot_wide_avx512(uint8_t *d, const uint8_t *a, const uint8_t *b, uint32_t wb, uint32_t w)
{
	__m512i x = _mm512_andnot_si512(ldb(b, wb), ldb(a, w));
	_mm512_mask_storeu_epi8(d, BZ(w), x);
	return _mm512_test_epi64_mask(x, x) != 0;
}

SIFT_INLINE uint64_t cnt_wide_avx512(const uint8_t *a, const uint8_t *b, uint32_t w)
{
	__mmask8 k = (__mmask8)BZ(w / 8);
	return (uint64_t)_mm512_reduce_add_epi64(_mm512_popcnt_epi64(_mm512_and_si512(_mm512_maskz_loadu_epi64(k, a), _mm512_maskz_loadu_epi64(k, b))));
}

#define SIFT_KERN_TMPL
#include "sift_bitmap_kern.h"

static uint64_t popcount_v(const uint8_t *p, size_t n)
{
	__m512i acc = _mm512_setzero_si512();
	size_t i = 0;
	for (; i + 64 <= n; i += 64) acc = _mm512_add_epi64(acc, _mm512_popcnt_epi64(_mm512_loadu_si512(p + i)));
	if (i < n) acc = _mm512_add_epi64(acc, _mm512_popcnt_epi64(_mm512_maskz_loadu_epi8(BZ(n - i), p + i)));
	return (uint64_t)_mm512_reduce_add_epi64(acc);
}

/* L: lane bits = 8 * w, MT: mask type, CH = 512 / L pages per vector */
#define EXPAND(L, MT, P, M) _mm512_maskz_expand_epi##L((MT)(M), _mm512_maskz_loadu_epi##L((MT)BZ(_mm_popcnt_u64(M)), P))
#define STORE(L, MT, D, KEEP, V) _mm512_mask_storeu_epi##L(D, (MT)BZ(_mm_popcnt_u64(KEEP)), _mm512_maskz_compress_epi##L((MT)(KEEP), V))

#define NARROW_KERNELS(L, MT, CH)                                                                                 \
	static void and2_e##L(const sift_grp *a, const sift_grp *b, const uint64_t *m, sift_grp *o, uint8_t *buf)     \
	{                                                                                                         \
		uint64_t pages[4] = { 0 };                                                                        \
		const uint8_t *pa = a->off, *pb = b->off;                                                         \
		uint8_t *d = buf;                                                                                 \
		for (int j = 0; j < 4; j++)                                                                       \
			for (int h = 0; h < 64; h += CH) {                                                        \
				MT ma = (MT)(a->pages[j] >> h), mb = (MT)(b->pages[j] >> h);                      \
				MT mm = (MT)(ma & mb & (m ? (MT)(m[j] >> h) : (MT)~(MT)0));                       \
				if (mm) {                                                                         \
					__m512i v = _mm512_and_si512(EXPAND(L, MT, pa, ma), EXPAND(L, MT, pb, mb)); \
					MT keep = _mm512_mask_test_epi##L##_mask(mm, v, v);                       \
					STORE(L, MT, d, keep, v);                                                 \
					d += _mm_popcnt_u64(keep) * (L / 8);                                      \
					pages[j] |= (uint64_t)keep << h;                                          \
				}                                                                                 \
				pa += _mm_popcnt_u64(ma) * (L / 8), pb += _mm_popcnt_u64(mb) * (L / 8);           \
			}                                                                                         \
		sift_grp_set(o, a->gid, L / 8, pages, buf);                                                       \
	}                                                                                                         \
	static void or2_e##L(const sift_grp *a, const sift_grp *b, sift_grp *o, uint8_t *buf)                     \
	{                                                                                                         \
		uint64_t pages[4];                                                                                \
		const uint8_t *pa = a->off, *pb = b->off;                                                         \
		uint8_t *d = buf;                                                                                 \
		for (int j = 0; j < 4; j++) {                                                                     \
			pages[j] = a->pages[j] | b->pages[j];                                                     \
			for (int h = 0; h < 64; h += CH) {                                                        \
				MT ma = (MT)(a->pages[j] >> h), mb = (MT)(b->pages[j] >> h), mu = (MT)(ma | mb);  \
				if (mu) {                                                                         \
					__m512i v = _mm512_or_si512(EXPAND(L, MT, pa, ma), EXPAND(L, MT, pb, mb)); \
					STORE(L, MT, d, mu, v);                                                   \
					d += _mm_popcnt_u64(mu) * (L / 8);                                        \
				}                                                                                 \
				pa += _mm_popcnt_u64(ma) * (L / 8), pb += _mm_popcnt_u64(mb) * (L / 8);           \
			}                                                                                         \
		}                                                                                                 \
		sift_grp_set(o, a->gid, L / 8, pages, buf);                                                       \
	}                                                                                                         \
	static void andnot_e##L(const sift_grp *a, const sift_grp *b, sift_grp *o, uint8_t *buf)                  \
	{                                                                                                         \
		uint64_t pages[4] = { 0 };                                                                        \
		const uint8_t *pa = a->off, *pb = b->off;                                                         \
		uint8_t *d = buf;                                                                                 \
		for (int j = 0; j < 4; j++)                                                                       \
			for (int h = 0; h < 64; h += CH) {                                                        \
				MT ma = (MT)(a->pages[j] >> h), mb = (MT)(b->pages[j] >> h);                      \
				if (ma) {                                                                         \
					__m512i va = EXPAND(L, MT, pa, ma);                                       \
					__m512i v = mb & ma ? _mm512_andnot_si512(EXPAND(L, MT, pb, mb), va) : va; \
					MT keep = _mm512_mask_test_epi##L##_mask(ma, v, v);                       \
					STORE(L, MT, d, keep, v);                                                 \
					d += _mm_popcnt_u64(keep) * (L / 8);                                      \
					pages[j] |= (uint64_t)keep << h;                                          \
				}                                                                                 \
				pa += _mm_popcnt_u64(ma) * (L / 8), pb += _mm_popcnt_u64(mb) * (L / 8);           \
			}                                                                                         \
		sift_grp_set(o, a->gid, L / 8, pages, buf);                                                       \
	}                                                                                                         \
	static uint64_t and_count_e##L(const sift_grp *a, const sift_grp *b)                                      \
	{                                                                                                         \
		__m512i acc = _mm512_setzero_si512();                                                             \
		const uint8_t *pa = a->off, *pb = b->off;                                                         \
		for (int j = 0; j < 4; j++)                                                                       \
			for (int h = 0; h < 64; h += CH) {                                                        \
				MT ma = (MT)(a->pages[j] >> h), mb = (MT)(b->pages[j] >> h);                      \
				if (ma & mb)                                                                      \
					acc = _mm512_add_epi64(acc, _mm512_popcnt_epi64(_mm512_and_si512(          \
						EXPAND(L, MT, pa, ma), EXPAND(L, MT, pb, mb))));                  \
				pa += _mm_popcnt_u64(ma) * (L / 8), pb += _mm_popcnt_u64(mb) * (L / 8);           \
			}                                                                                         \
		return (uint64_t)_mm512_reduce_add_epi64(acc);                                                    \
	}                                                                                                         \
	static uint64_t count_mask_e##L(const sift_grp *g, const uint64_t *m)                                     \
	{                                                                                                         \
		__m512i acc = _mm512_setzero_si512();                                                             \
		const uint8_t *p = g->off;                                                                        \
		for (int j = 0; j < 4; j++)                                                                       \
			for (int h = 0; h < 64; h += CH) {                                                        \
				MT mg = (MT)(g->pages[j] >> h), mk = (MT)(mg & (MT)(m[j] >> h));                  \
				if (mk)                                                                           \
					acc = _mm512_add_epi64(acc, _mm512_popcnt_epi64(                          \
						_mm512_maskz_mov_epi##L(mk, EXPAND(L, MT, p, mg))));              \
				p += _mm_popcnt_u64(mg) * (L / 8);                                                \
			}                                                                                         \
		return (uint64_t)_mm512_reduce_add_epi64(acc);                                                    \
	}

NARROW_KERNELS(8, __mmask64, 64)
NARROW_KERNELS(16, __mmask32, 32)
NARROW_KERNELS(32, __mmask16, 16)
NARROW_KERNELS(64, __mmask8, 8)

#define NARROW_SWITCH(W, CALL)                                                                                  \
	switch (W) {                                                                                            \
	case 1: CALL(8); case 2: CALL(16); case 4: CALL(32); case 8: CALL(64);                                  \
	}

static void and2_v(const sift_grp *a, const sift_grp *b, const uint64_t *m, sift_grp *o, uint8_t *buf)
{
#define C(L) return and2_e##L(a, b, m, o, buf)
	if (a->w == b->w) NARROW_SWITCH(a->w, C)
#undef C
	and2_avx512(a, b, m, o, buf);
}

static void or2_v(const sift_grp *a, const sift_grp *b, sift_grp *o, uint8_t *buf)
{
#define C(L) return or2_e##L(a, b, o, buf)
	if (a->w == b->w) NARROW_SWITCH(a->w, C)
#undef C
	or2_avx512(a, b, o, buf);
}

static void andnot_v(const sift_grp *a, const sift_grp *b, sift_grp *o, uint8_t *buf)
{
#define C(L) return andnot_e##L(a, b, o, buf)
	if (a->w == b->w) NARROW_SWITCH(a->w, C)
#undef C
	andnot_avx512(a, b, o, buf);
}

static uint64_t and_count_v(const sift_grp *a, const sift_grp *b)
{
#define C(L) return and_count_e##L(a, b)
	if (a->w == b->w) NARROW_SWITCH(a->w, C)
#undef C
	return and_count_avx512(a, b);
}

static uint64_t count_mask_v(const sift_grp *g, const uint64_t *m)
{
	if (!m) return popcount_v(g->off, (size_t)(_mm_popcnt_u64(g->pages[0]) + _mm_popcnt_u64(g->pages[1]) + _mm_popcnt_u64(g->pages[2]) + _mm_popcnt_u64(g->pages[3])) * g->w);
#define C(L) return count_mask_e##L(g, m)
	NARROW_SWITCH(g->w, C)
#undef C
	return count_mask_avx512(g, m);
}

const sift_kernels sift_kern_avx512 = { popcount_v, and2_v, or2_v, andnot_v, and_count_v, count_mask_v, tids_avx512, ranks_avx512 };

#if defined(__clang__)
#pragma clang attribute pop
#else
#pragma GCC pop_options
#endif
#else
#include "sift_bitmap_kern.h"
const sift_kernels sift_kern_avx512 = { 0 };
#endif
