/*
 * bm25/blob.c - term blob encoder, size bound and exhaustive checker (LIST and GROUPED).
 */
#include <stdlib.h>
#include "bm25/bitmap_kern.h"
#include "bm25/bitmap_int.h"
#include "core/varint.h"

/* ---- encoding ---- */

size_t sift_encode_bound(uint32_t n) { return 40 + (size_t)n * 112; }

typedef struct { uint32_t i, j, npages, w, ob; int array, packed; } grp_info;

static inline uint32_t packed_type(uint32_t w)
{
	uint32_t i = 0;
	while (packed_w[i] != w) i++;
	return PACKED_TYPE | i;
}

/* group [i, j) of tids: same gid */
static grp_info grp_scan(const sift_tid *t, uint32_t i, uint32_t n, int ctype)
{
	grp_info g = { i, i, 0, 0, 0, 0, 0 };
	uint32_t gid = SIFT_GID(t[i]), maxoff = 0, last = UINT32_MAX;
	for (; g.j < n && SIFT_GID(t[g.j]) == gid; g.j++) {
		if ((SIFT_BLK(t[g.j]) & (SIFT_GROUP_PAGES - 1)) != last) g.npages++, last = SIFT_BLK(t[g.j]) & (SIFT_GROUP_PAGES - 1);
		if (SIFT_OFF(t[g.j]) > maxoff) maxoff = SIFT_OFF(t[g.j]);
	}
	g.w = sift_w_for(maxoff);
	g.ob = nbits(maxoff - 1) ? nbits(maxoff - 1) : 1;
	size_t bm = 32 + (size_t)g.npages * g.w, ar = (size_t)(g.j - g.i) * (g.w <= 32 ? 2 : 4);
	g.array = ctype == SIFT_CT_ARRAY || ((ctype == SIFT_CT_AUTO || ctype == SIFT_CT_PACKED) && ar < bm);
	if (ctype == SIFT_CT_PACKED && g.array && packed_bytes(g.j - g.i, g.ob) < ar) g.array = 0, g.packed = 1;
	return g;
}

static size_t grp_bytes(const grp_info *g)
{
	return g->packed ? packed_bytes(g->j - g->i, g->ob) : g->array ? (size_t)(g->j - g->i) * (g->w <= 32 ? 2 : 4) : 32 + (size_t)g->npages * g->w;
}

static inline uint64_t key_of(sift_tid t, uint32_t ob) { return (uint64_t)SIFT_BLK(t) << ob | (SIFT_OFF(t) - 1); }
static inline uint32_t vlen(uint64_t x) { uint32_t n = 1; while (x >>= 7) n++; return n; }

static uint32_t list_ob(const sift_tid *t, uint32_t n)
{
	uint32_t m = 1;
	for (uint32_t i = 0; i < n; i++) m = SIFT_OFF(t[i]) > m ? SIFT_OFF(t[i]) : m;
	return nbits(m - 1);
}

static uint64_t popcount_n(const uint8_t *p, size_t n)
{
	uint64_t c = 0;
	while (n--) c += sift_pc64(*p++);
	return c;
}

/* the cnt postings of one EF block are strictly increasing, start at 0 and end at u - 1 */
static int ef_block_ok(const uint8_t *lows, const uint8_t *highs, size_t nhigh, uint64_t u, uint32_t cnt, uint32_t l)
{
	uint64_t prev = 0;
	uint32_t k = 0;
	for (size_t byte = 0; byte < nhigh; byte++)
		for (unsigned bits = highs[byte]; bits; bits &= bits - 1, k++) {
			uint64_t pos = (uint64_t)k * l, low = 0;
			for (uint32_t b = 0; b < l; b++, pos++) low |= (uint64_t)(lows[pos >> 3] >> (pos & 7) & 1) << b;
			uint64_t v = ((uint64_t)byte * 8 + __builtin_ctz(bits) - k) << l | low;
			if (k ? v <= prev : v != 0) return 0;
			prev = v;
		}
	return k == cnt && prev == u - 1;
}

static const char *check_list(const uint8_t *blob, size_t len)
{
	const uint8_t *p = blob + 2, *end = blob + len;
	uint64_t n, prev = 0, lim;
	int wtf = blob[0] >> 4 & SIFT_F_TF;
	if (blob[1] > SIFT_LIST_OB_MAX) return "LIST offset bits out of range";
	lim = 1ull << (32 + blob[1]);
	if (!(p = sift_get_varint_n(p, end, &n)) || n == 0 || n > UINT32_MAX || (wtf && p++ >= end)) return "LIST header overruns the blob";
	for (uint64_t i = 0; i < n; i += SIFT_LIST_BLOCK) {
		uint64_t d, u1;
		uint32_t cnt = blk_cnt((uint32_t)n, (uint32_t)i), l;
		if (!(p = sift_get_varint_n(p, end, &d)) || !(p = sift_get_varint_n(p, end, &u1))) return "LIST block header overruns the blob";
		if (d >= lim || u1 >= lim || u1 + 1 < cnt || prev + d + u1 >= lim || (i && d == 0)) return "LIST block range is out of bounds";
		l = ef_l(u1 + 1, cnt);
		size_t lows = ef_lows(cnt, l), highs = ef_highs(u1 + 1, cnt, l);
		if ((size_t)(end - p) < lows + highs) return "LIST block overruns the blob";
		if (popcount_n(p + lows, highs) != cnt) return "LIST block high bits do not count its postings";
		if (!ef_block_ok(p, p + lows, highs, u1 + 1, cnt, l)) return "LIST block postings are not strictly increasing";
		p += lows + highs, prev += d + u1;
	}
	return wtf && (size_t)(end - p) < (n + 1) / 2 ? "LIST tf stream overruns the blob" : NULL;
}

/* array container entry i: u16 (page << 8 | off - 1) or u32 (page << 16 | off - 1) */
static uint32_t ld_e(const uint8_t *p, uint32_t es, uint32_t i)
{
	return es == 2 ? (uint32_t)p[2 * i] | (uint32_t)p[2 * i + 1] << 8 : ld32(p + 4 * i);
}

/* a PACKED group of n postings in len bytes: its sizes, offsets inside the width, ascending offsets on a page, the pages of its mask */
static const char *check_packed(const uint8_t *p, size_t len, uint32_t n, uint32_t w)
{
	uint32_t ob = len ? p[0] : 0, np = 0, k = 0, pages;
	const uint8_t *offs = p + 33, *last;
	if (len < 33 || ob == 0 || ob > 8 * w || ob > 16 || len != packed_bytes(n, ob)) return "GROUPED packed group has the wrong size";
	last = offs + ((uint64_t)n * ob + 7) / 8;
	pages = (uint32_t)popcount_n(p + 1, 32);
	if (!(last[(n - 1) >> 3] >> ((n - 1) & 7) & 1)) return "GROUPED packed group does not end a page with its last posting";
	for (uint32_t i = 0; i < n; i++) np += last[i >> 3] >> (i & 7) & 1;
	if (np != pages) return "GROUPED packed group does not match its page mask";
	for (uint32_t i = 0, prev = 0; i < n; i++) {
		uint64_t bit = (uint64_t)i * ob, v = 0;
		for (uint32_t b = 0; b < ob; b++, bit++) v |= (uint64_t)(offs[bit >> 3] >> (bit & 7) & 1) << b;
		if (v >= 8 * (uint64_t)w || (k && v <= prev)) return "GROUPED packed offsets are outside their page bitmap or not ascending";
		prev = (uint32_t)v;
		k = !(last[i >> 3] >> (i & 7) & 1);
	}
	return NULL;
}

static const char *check_grouped(const uint8_t *blob, size_t len)
{
	uint64_t n, ng, tables, plen;
	int wtf = blob[0] >> 4 & SIFT_F_TF;
	if (len < 12) return "GROUPED header overruns the blob";
	n = ld32(blob + 4), ng = ld32(blob + 8);
	if (n == 0 || ng == 0 || ng > n) return "GROUPED group count is out of range";
	tables = 12 + 4 * (3 * ng + 2);
	if (tables > len) return "GROUPED tables overrun the blob";
	const uint8_t *key = blob + 12, *poff = key + 4 * ng, *base = poff + 4 * (ng + 1), *payload = blob + tables;
	plen = ld32(poff + 4 * ng);
	if (plen > len - tables || (wtf && len - tables - plen < 9 * ng + (n + 1) / 2)) return "GROUPED payload overruns the blob";
	if (ld32(poff) != 0 || ld32(base) != 0 || ld32(base + 4 * ng) != n) return "GROUPED offsets do not start at zero and end at the count";
	for (uint64_t g = 0; g < ng; g++) {
		uint32_t k = ld32(key + 4 * g), packed = (k & 0xff) >= PACKED_TYPE, w = packed ? packed_w_of(k) : k & 0x7f,
			lo = ld32(poff + 4 * g), hi = ld32(poff + 4 * (g + 1));
		uint32_t cnt = ld32(base + 4 * (g + 1)) - ld32(base + 4 * g);
		if (g > 0 && k >> 8 <= ld32(key + 4 * (g - 1)) >> 8) return "GROUPED groups are out of order";
		if (!(w == 1 || w == 2 || w == 4 || (w % 8 == 0 && w > 0 && w <= SIFT_MAX_W))) return "GROUPED group width is invalid";
		if (lo > hi || hi > plen || cnt == 0 || cnt > n || ld32(base + 4 * (g + 1)) > n || ld32(base + 4 * (g + 1)) < ld32(base + 4 * g))
			return "GROUPED group extents are out of order";
		if (packed) {
			const char *bad = check_packed(payload + lo, hi - lo, cnt, w);
			if (bad) return bad;
		} else if (k & 0x80) {
			uint32_t es = w <= 32 ? 2 : 4;
			if (hi - lo != (uint64_t)cnt * es) return "GROUPED array group has the wrong size";
			for (uint32_t i = 0; i < cnt; i++) {
				uint32_t e = ld_e(payload + lo, es, i), o = es == 2 ? e & 0xff : e & 0xffff, pg = es == 2 ? e >> 8 : e >> 16;
				if (o >= 8 * w || pg >= SIFT_GROUP_PAGES) return "GROUPED array entry lies outside its page bitmap";
				if (i && e <= ld_e(payload + lo, es, i - 1)) return "GROUPED array entries are not strictly increasing";
			}
		} else {
			if (hi - lo < 32) return "GROUPED bitmap group is shorter than its page mask";
			uint64_t np = popcount_n(payload + lo, 32), bits = 0;
			if (hi - lo - 32 != np * w) return "GROUPED bitmap group does not match its page mask";
			for (const uint8_t *q = payload + lo + 32; np--; q += w) {
				uint64_t c = popcount_n(q, w);
				if (c == 0) return "GROUPED bitmap group has an empty page";
				bits += c;
			}
			if (bits != cnt) return "GROUPED bitmap group does not hold its posting count";
		}
	}
	return NULL;
}

const char *sift_blob_check(const uint8_t *blob, size_t len)
{
	if (len < 4 || blob[0] >> 4 & ~SIFT_F_TF) return "blob header is invalid";
	return sift_blob_kind(blob) == SIFT_LIST ? check_list(blob, len) : sift_blob_kind(blob) == SIFT_GROUPED ? check_grouped(blob, len) : "blob kind is unknown";
}

static size_t list_size(const sift_tid *t, uint32_t n)
{
	uint32_t ob = list_ob(t, n);
	size_t s = 2 + vlen(n);
	uint64_t prev = 0;
	for (uint32_t i = 0; i < n; i += SIFT_LIST_BLOCK) {
		uint32_t cnt = blk_cnt(n, i);
		uint64_t x0 = key_of(t[i], ob), u = key_of(t[i + cnt - 1], ob) - x0 + 1;
		uint32_t l = ef_l(u, cnt);
		s += vlen(x0 - prev) + vlen(u - 1) + ef_lows(cnt, l) + ef_highs(u, cnt, l);
		prev = x0 + u - 1;
	}
	return s;
}

static uint8_t *write_list(const sift_tid *t, uint32_t n, uint32_t ob, uint8_t *p)
{
	uint64_t prev = 0;
	for (uint32_t i = 0; i < n; i += SIFT_LIST_BLOCK) {
		uint32_t cnt = blk_cnt(n, i);
		uint64_t x0 = key_of(t[i], ob), u = key_of(t[i + cnt - 1], ob) - x0 + 1, acc = 0;
		uint32_t l = ef_l(u, cnt), nb = 0;
		p = sift_put_varint(sift_put_varint(p, x0 - prev), u - 1);
		uint8_t *highs = p + ef_lows(cnt, l);
		memset(highs, 0, ef_highs(u, cnt, l));
		for (uint32_t k = 0; k < cnt; k++) {
			uint64_t v = key_of(t[i + k], ob) - x0, h = (v >> l) + k;
			acc |= (v & ((1ull << l) - 1)) << nb;
			for (nb += l; nb >= 8; nb -= 8, acc >>= 8) *p++ = (uint8_t)acc;
			highs[h >> 3] |= (uint8_t)(1u << (h & 7));
		}
		if (nb) *p++ = (uint8_t)acc;
		p = highs + ef_highs(u, cnt, l);
		prev = x0 + u - 1;
	}
	return p;
}

static size_t grouped_size(const sift_tid *t, uint32_t n, int ctype, uint32_t *ng)
{
	size_t s = 20;
	*ng = 0;
	for (uint32_t i = 0; i < n;) {
		grp_info g = grp_scan(t, i, n, ctype);
		s += 12 + grp_bytes(&g);
		(*ng)++;
		i = g.j;
	}
	return s;
}

static uint8_t *write_packed(const sift_tid *t, const grp_info *g, uint8_t *p)
{
	uint32_t n = g->j - g->i;
	uint64_t pages[4] = { 0 };
	uint8_t *offs = p + 33, *last = offs + (size_t)(((uint64_t)n * g->ob + 7) / 8), *end = last + (n + 7) / 8;
	memset(offs, 0, (size_t)(end - offs));
	p[0] = (uint8_t)g->ob;
	for (uint32_t k = 0; k < n; k++) {
		sift_tid x = t[g->i + k];
		uint32_t pg = SIFT_BLK(x) & (SIFT_GROUP_PAGES - 1), o = SIFT_OFF(x) - 1;
		uint64_t bit = (uint64_t)k * g->ob;
		pages[pg >> 6] |= 1ull << (pg & 63);
		for (uint32_t b = 0; b < g->ob; b++, bit++) offs[bit >> 3] |= (uint8_t)((o >> b & 1) << (bit & 7));
		if (k + 1 == n || (SIFT_BLK(t[g->i + k + 1]) & (SIFT_GROUP_PAGES - 1)) != pg) last[k >> 3] |= (uint8_t)(1u << (k & 7));
	}
	memcpy(p + 1, pages, 32);
	return end;
}

static uint8_t *write_group(const sift_tid *t, const grp_info *g, uint8_t *p)
{
	if (g->packed) return write_packed(t, g, p);
	if (g->array) {
		for (uint32_t k = g->i; k < g->j; k++) {
			uint32_t pg = SIFT_BLK(t[k]) & (SIFT_GROUP_PAGES - 1), o = SIFT_OFF(t[k]) - 1;
			if (g->w <= 32) {
				uint16_t e = (uint16_t)(pg << 8 | o);
				memcpy(p, &e, 2), p += 2;
			} else
				st32(p, pg << 16 | o), p += 4;
		}
		return p;
	}
	uint64_t pages[4] = { 0 };
	uint8_t *off = p + 32 - g->w;
	uint32_t last = UINT32_MAX;
	for (uint32_t k = g->i; k < g->j; k++) {
		uint32_t pg = SIFT_BLK(t[k]) & (SIFT_GROUP_PAGES - 1), o = SIFT_OFF(t[k]) - 1;
		if (pg != last) memset(off += g->w, 0, g->w), last = pg, pages[pg >> 6] |= 1ull << (pg & 63);
		off[o >> 3] |= (uint8_t)(1u << (o & 7));
	}
	memcpy(p, pages, 32);
	return off + g->w;
}

static uint8_t *write_tf(const uint8_t *tf, uint32_t n, uint8_t *p)
{
	for (uint32_t i = 0; i < n; i += 2)
		*p++ = (uint8_t)(sift_tf_quant(tf[i]) | (i + 1 < n ? sift_tf_quant(tf[i + 1]) << 4 : 0));
	return p;
}

size_t sift_encode(const sift_tid *t, const uint8_t *tf, const uint8_t *ub, uint32_t n, const sift_enc_opts *o, uint8_t *out)
{
	static const sift_enc_opts dflt = { 0 };
	if (!o) o = &dflt;
	uint32_t ng = 0;
	size_t gs = o->kind == SIFT_LIST ? 0 : grouped_size(t, n, o->ctype, &ng);
	size_t ls = o->kind == SIFT_GROUPED ? 0 : list_size(t, n);
	int kind = o->kind ? o->kind : (double)gs <= (o->bias > 0 ? o->bias : 1.0) * (double)ls ? SIFT_GROUPED : SIFT_LIST;
	int wtf = o->tf && tf;
	if (!out) return (kind == SIFT_LIST ? ls : gs) + (wtf ? (kind == SIFT_GROUPED ? 9 * ng : 1) + (n + 1) / 2 : 0);
	uint8_t *p;
	out[0] = (uint8_t)(kind | (wtf ? SIFT_F_TF << 4 : 0));
	if (kind == SIFT_LIST) {
		uint32_t ob = list_ob(t, n);
		out[1] = (uint8_t)ob;
		p = sift_put_varint(out + 2, n);
		if (wtf) {
			uint32_t m = 0;
			for (uint32_t i = 0; i < n; i++) m = (ub ? ub[i] : 255u) > m ? (ub ? ub[i] : 255u) : m;
			*p++ = (uint8_t)m;
		}
		p = write_list(t, n, ob, p);
	} else {
		out[1] = out[2] = out[3] = 0;
		st32(out + 4, n);
		p = out + 8;
		uint8_t *key = p + 4, *poff = key + 4 * ng, *base = poff + 4 * (ng + 1), *payload = base + 4 * (ng + 1);
		uint32_t gi = 0;
		st32(p, ng);
		p = payload;
		for (uint32_t i = 0; i < n; gi++) {
			grp_info g = grp_scan(t, i, n, o->ctype);
			st32(key + 4 * gi, SIFT_GID(t[i]) << 8 | (g.packed ? packed_type(g.w) : g.w | (g.array ? 0x80 : 0)));
			st32(poff + 4 * gi, (uint32_t)(p - payload));
			st32(base + 4 * gi, i);
			p = write_group(t, &g, p);
			i = g.j;
		}
		st32(poff + 4 * ng, (uint32_t)(p - payload));
		st32(base + 4 * ng, n);
		if (wtf) {
			uint8_t *bq = p + ng;
			for (uint32_t i = 0; i < n; p++, bq += 8) {
				uint32_t j = i, m = 0, bm[16] = { 0 };
				for (; j < n && SIFT_GID(t[j]) == SIFT_GID(t[i]); j++) {
					uint32_t u = ub ? ub[j] : 255u, b = (SIFT_BLK(t[j]) & (SIFT_GROUP_PAGES - 1)) / 16;
					m = u > m ? u : m, bm[b] = u > bm[b] ? u : bm[b];
				}
				*p = (uint8_t)m;
				for (int b = 0; b < 16; b += 2)
					bq[b / 2] = (uint8_t)((m ? (15 * bm[b] + m - 1) / m : 0) | (m ? (15 * bm[b + 1] + m - 1) / m : 0) << 4);
				i = j;
			}
			p = bq;
		}
	}
	if (wtf) p = write_tf(tf, n, p);
	return (size_t)(p - out);
}

size_t sift_keylist_encode(const sift_tid *t, uint32_t n, uint8_t *out)
{
	static const sift_enc_opts o = { SIFT_LIST, 0, 0, 0 };
	return sift_encode(t, NULL, NULL, n, &o, out);
}
