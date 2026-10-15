/* sift_bitmap: posting formats, cursors and operator trees.  See sift_bitmap.h. */
#include <stdlib.h>
#include "sift_bitmap_kern.h"

const sift_kernels *sift_k = &sift_kern_scalar;
static int cur_isa;

static const sift_kernels *const kern_by_isa[SIFT_NISA] = { &sift_kern_scalar, &sift_kern_avx2, &sift_kern_avx512 };

static int isa_ok(int isa)
{
	__builtin_cpu_init();
	int v3 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi") && __builtin_cpu_supports("bmi2") &&
		 __builtin_cpu_supports("popcnt");
	switch (isa) {
	case SIFT_ISA_SCALAR: return 1;
	case SIFT_ISA_AVX2: return v3;
	case SIFT_ISA_AVX512:
		return v3 && __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
		       __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512vbmi2") &&
		       __builtin_cpu_supports("avx512vpopcntdq");
	}
	return 0;
}

int sift_set_isa(int isa)
{
	if (isa < 0 || isa >= SIFT_NISA || !isa_ok(isa)) return 0;
	sift_k = kern_by_isa[isa];
	cur_isa = isa;
	return 1;
}

int sift_init(void)
{
	for (int isa = SIFT_NISA - 1; isa > 0; isa--)
		if (sift_set_isa(isa)) return isa;
	sift_set_isa(SIFT_ISA_SCALAR);
	return SIFT_ISA_SCALAR;
}

int sift_isa(void) { return cur_isa; }

const char *sift_isa_name(int isa)
{
	static const char *n[SIFT_NISA] = { "scalar", "avx2", "avx512" };
	return isa >= 0 && isa < SIFT_NISA ? n[isa] : "?";
}

uint64_t sift_popcount(const uint8_t *p, size_t n) { return sift_k->popcount(p, n); }
uint64_t sift_grp_count(const sift_grp *g) { return g->n ? g->n : sift_k->count_mask(g, NULL); }
uint64_t sift_grp_count_mask(const sift_grp *g, const uint64_t mask[4]) { return sift_k->count_mask(g, mask); }
uint32_t sift_grp_tids(const sift_grp *g, const uint64_t *mask, sift_tid *out) { return sift_k->tids(g, mask, out); }
void sift_grp_and(const sift_grp *a, const sift_grp *b, const uint64_t *mask, sift_grp *o, uint8_t *buf) { sift_k->and2(a, b, mask, o, buf); }
void sift_grp_or(const sift_grp *a, const sift_grp *b, sift_grp *o, uint8_t *buf) { sift_k->or2(a, b, o, buf); }
void sift_grp_andnot(const sift_grp *a, const sift_grp *b, sift_grp *o, uint8_t *buf) { sift_k->andnot(a, b, o, buf); }
uint64_t sift_grp_and_count(const sift_grp *a, const sift_grp *b) { return sift_k->and_count(a, b); }
uint32_t sift_grp_ranks(const sift_grp *src, const sift_grp *sub, uint32_t *rank) { return sift_k->ranks(src, sub, rank); }

/* ---- encoding ---- */

static const uint8_t *get_varint(const uint8_t *p, uint64_t *x);
static inline uint32_t ld32(const uint8_t *p) { uint32_t x; memcpy(&x, p, 4); return x; }
static inline void st32(uint8_t *p, uint32_t x) { memcpy(p, &x, 4); }
static inline uint64_t ld64(const uint8_t *p) { uint64_t x; memcpy(&x, p, 8); return x; }
static inline uint32_t nbits(uint64_t x) { return x ? 64 - (uint32_t)__builtin_clzll(x) : 0; }
static inline int pages_empty(const uint64_t *p) { return !(p[0] | p[1] | p[2] | p[3]); }
static inline uint32_t pages_pc(const uint64_t *p)
{
	return sift_pc64(p[0]) + sift_pc64(p[1]) + sift_pc64(p[2]) + sift_pc64(p[3]);
}

uint32_t sift_w_for(uint32_t maxoff)
{
	uint32_t b = (maxoff + 7) / 8;
	return b <= 1 ? 1 : b <= 2 ? 2 : b <= 4 ? 4 : (b + 7) & ~7u;
}

uint32_t sift_tf_quant(uint32_t tf) { return tf > 15 ? 15 : tf ? tf : 1; }
int sift_blob_kind(const uint8_t *blob) { return blob[0] & 0xf; }

uint32_t sift_blob_n(const uint8_t *blob)
{
	uint64_t n;
	if (sift_blob_kind(blob) == SIFT_GROUPED) return ld32(blob + 4);
	get_varint(blob + 2, &n);
	return (uint32_t)n;
}

size_t sift_encode_bound(uint32_t n) { return 40 + (size_t)n * 112; }

typedef struct { uint32_t i, j, npages, w; int array; } grp_info;

/* group [i, j) of tids: same gid */
static grp_info grp_scan(const sift_tid *t, uint32_t i, uint32_t n, int ctype)
{
	grp_info g = { i, i, 0, 0, 0 };
	uint32_t gid = SIFT_GID(t[i]), maxoff = 0, last = UINT32_MAX;
	for (; g.j < n && SIFT_GID(t[g.j]) == gid; g.j++) {
		if ((SIFT_BLK(t[g.j]) & (SIFT_GROUP_PAGES - 1)) != last) g.npages++, last = SIFT_BLK(t[g.j]) & (SIFT_GROUP_PAGES - 1);
		if (SIFT_OFF(t[g.j]) > maxoff) maxoff = SIFT_OFF(t[g.j]);
	}
	g.w = sift_w_for(maxoff);
	size_t bm = 32 + (size_t)g.npages * g.w, ar = (size_t)(g.j - g.i) * (g.w <= 32 ? 2 : 4);
	g.array = ctype == SIFT_CT_ARRAY || (ctype == SIFT_CT_AUTO && ar < bm);
	return g;
}

static size_t grp_bytes(const grp_info *g) { return g->array ? (size_t)(g->j - g->i) * (g->w <= 32 ? 2 : 4) : 32 + (size_t)g->npages * g->w; }

/* LIST: Elias-Fano blocks over key = blk << ob | (off - 1) */
typedef struct { uint64_t x0, u; uint32_t cnt, l; const uint8_t *lows, *highs, *end; } ef_blk;

static inline uint64_t key_of(sift_tid t, uint32_t ob) { return (uint64_t)SIFT_BLK(t) << ob | (SIFT_OFF(t) - 1); }
static inline uint32_t ef_l(uint64_t u, uint32_t cnt) { return u > cnt ? nbits(u / cnt) - 1 : 0; }
static inline size_t ef_lows(uint32_t cnt, uint32_t l) { return ((size_t)cnt * l + 7) / 8; }
static inline size_t ef_highs(uint64_t u, uint32_t cnt, uint32_t l) { return (cnt + ((u - 1) >> l) + 7) / 8; }
static inline uint32_t vlen(uint64_t x) { uint32_t n = 1; while (x >>= 7) n++; return n; }

static uint8_t *put_varint(uint8_t *p, uint64_t x)
{
	for (; x >= 0x80; x >>= 7) *p++ = (uint8_t)(x | 0x80);
	*p++ = (uint8_t)x;
	return p;
}

static const uint8_t *get_varint(const uint8_t *p, uint64_t *x)
{
	*x = 0;
	for (int s = 0;; s += 7) {
		*x |= (uint64_t)(*p & 0x7f) << s;
		if (!(*p++ & 0x80)) return p;
	}
}

static uint32_t list_ob(const sift_tid *t, uint32_t n)
{
	uint32_t m = 1;
	for (uint32_t i = 0; i < n; i++) m = SIFT_OFF(t[i]) > m ? SIFT_OFF(t[i]) : m;
	return nbits(m - 1);
}

static inline uint32_t blk_cnt(uint32_t n, uint32_t i) { return n - i < SIFT_LIST_BLOCK ? n - i : SIFT_LIST_BLOCK; }

/* varint at p, NULL when it runs past end or past 63 bits */
static const uint8_t *get_varint_n(const uint8_t *p, const uint8_t *end, uint64_t *x)
{
	*x = 0;
	for (int s = 0; p < end && s < 63; s += 7) {
		*x |= (uint64_t)(*p & 0x7f) << s;
		if (!(*p++ & 0x80)) return p;
	}
	return NULL;
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
	if (blob[1] > nbits(SIFT_MAX_OFF - 1)) return "LIST offset bits out of range";
	lim = 1ull << (32 + blob[1]);
	if (!(p = get_varint_n(p, end, &n)) || n == 0 || n > UINT32_MAX || (wtf && p++ >= end)) return "LIST header overruns the blob";
	for (uint64_t i = 0; i < n; i += SIFT_LIST_BLOCK) {
		uint64_t d, u1;
		uint32_t cnt = blk_cnt((uint32_t)n, (uint32_t)i), l;
		if (!(p = get_varint_n(p, end, &d)) || !(p = get_varint_n(p, end, &u1))) return "LIST block header overruns the blob";
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
		uint32_t k = ld32(key + 4 * g), w = k & 0x7f, lo = ld32(poff + 4 * g), hi = ld32(poff + 4 * (g + 1));
		uint32_t cnt = ld32(base + 4 * (g + 1)) - ld32(base + 4 * g);
		if (g > 0 && k >> 8 <= ld32(key + 4 * (g - 1)) >> 8) return "GROUPED groups are out of order";
		if (!(w == 1 || w == 2 || w == 4 || (w % 8 == 0 && w > 0 && w <= SIFT_MAX_W))) return "GROUPED group width is invalid";
		if (lo > hi || hi > plen || cnt == 0 || cnt > n || ld32(base + 4 * (g + 1)) > n || ld32(base + 4 * (g + 1)) < ld32(base + 4 * g))
			return "GROUPED group extents are out of order";
		if (k & 0x80) {
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
		p = put_varint(put_varint(p, x0 - prev), u - 1);
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

static uint8_t *write_group(const sift_tid *t, const grp_info *g, uint8_t *p)
{
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
		p = put_varint(out + 2, n);
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
			st32(key + 4 * gi, SIFT_GID(t[i]) << 8 | g.w | (g.array ? 0x80 : 0));
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

/* ---- source cursors ---- */

/* first index >= i with key[idx] >= v (galloping) */
static uint32_t gallop32(const uint32_t *key, uint32_t n, uint32_t i, uint64_t v)
{
	uint32_t step = 1, lo = i, hi = i;
	while (hi < n && key[hi] < v) lo = hi + 1, hi += step, step <<= 1;
	if (hi > n) hi = n;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (key[mid] < v) lo = mid + 1; else hi = mid;
	}
	return lo;
}

static uint32_t gallop_tid(const sift_tid *t, uint32_t n, uint32_t i, uint32_t gid)
{
	uint32_t step = 1, lo = i, hi = i;
	while (hi < n && SIFT_GID(t[hi]) < gid) lo = hi + 1, hi += step, step <<= 1;
	if (hi > n) hi = n;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (SIFT_GID(t[mid]) < gid) lo = mid + 1; else hi = mid;
	}
	return lo;
}

/* incremental builder of a decoded group in c->buf; narrow bitmaps accumulate in cur */
typedef struct { uint64_t pages[4], cur; uint32_t r, last, w; uint8_t *off; } builder;

static inline void build_flush(builder *b)
{
	if (b->w <= 8 && b->last != UINT32_MAX) sift_stn(b->off + (b->r - 1) * b->w, b->w, b->cur);
}

static inline void build_add(builder *b, uint32_t pg, uint32_t o)
{
	if (pg != b->last) {
		build_flush(b);
		b->cur = 0, b->r++, b->last = pg, b->pages[pg >> 6] |= 1ull << (pg & 63);
		if (b->w > 8) memset(b->off + (b->r - 1) * b->w, 0, b->w);
	}
	if (b->w <= 8) b->cur |= 1ull << o;
	else b->off[(b->r - 1) * b->w + (o >> 3)] |= (uint8_t)(1u << (o & 7));
}

static const sift_grp *found(sift_cur *c, uint32_t gid, uint32_t w, const uint64_t *pages, const uint8_t *off, uint32_t base, uint32_t n)
{
	sift_grp_set(&c->g, gid, w, pages, off);
	c->g.base = base, c->g.n = n;
	c->state = 1;
	return &c->g;
}

static const sift_grp *ended(sift_cur *c)
{
	c->state = 2;
	return NULL;
}

static const sift_grp *gr_seek(sift_cur *c, uint32_t target)
{
	if (c->state == 2) return NULL;
	if (c->state == 1 && c->g.gid >= target) return &c->g;
	uint32_t i = gallop32(c->gr.key, c->gr.ng, c->state ? c->gr.i + 1 : 0, (uint64_t)target << 8);
	if (i == c->gr.ng) return ended(c);
	c->gr.i = i;
	uint32_t key = c->gr.key[i], w = key & 0x7f, base = c->gr.base[i], n = c->gr.base[i + 1] - base;
	const uint8_t *p = c->gr.payload + c->gr.poff[i];
	/* software prefetch 4 groups ahead: groups are small and visited in order */
	if (i + 4 < c->gr.ng)
		for (uint32_t q = c->gr.poff[i + 4], e = c->gr.poff[i + 5]; q < e; q += 64) __builtin_prefetch(c->gr.payload + q);
	if (!(key & 0x80)) {
		uint64_t pages[4];
		memcpy(pages, p, 32);
		return found(c, key >> 8, w, pages, p + 32, base, n);
	}
	builder b = { { 0 }, 0, 0, UINT32_MAX, w, c->buf };
	for (uint32_t k = 0; k < n; k++) {
		uint32_t e = w <= 32 ? (uint32_t)p[2 * k] | (uint32_t)p[2 * k + 1] << 8 : ld32(p + 4 * k);
		build_add(&b, w <= 32 ? e >> 8 : e >> 16, w <= 32 ? e & 0xff : e & 0xffff);
	}
	build_flush(&b);
	return found(c, key >> 8, w, b.pages, c->buf, base, n);
}

static inline uint64_t ld_upto8(const uint8_t *p, const uint8_t *end)
{
	uint64_t x = 0;
	if (end - p >= 8) return sift_ld8(p);
	memcpy(&x, p, (size_t)(end - p));
	return x;
}

static void ef_hdr(const uint8_t *p, uint64_t prev, uint32_t cnt, ef_blk *b)
{
	uint64_t d, u1;
	p = get_varint(get_varint(p, &d), &u1);
	b->x0 = prev + d, b->u = u1 + 1, b->cnt = cnt, b->l = ef_l(b->u, cnt);
	b->lows = p, b->highs = p + ef_lows(cnt, b->l), b->end = b->highs + ef_highs(b->u, cnt, b->l);
}

static void ef_decode(const ef_blk *b, uint32_t ob, sift_tid *out)
{
	uint32_t l = b->l, k = 0;
	uint64_t mask = (1ull << l) - 1, omask = (1ull << ob) - 1;
	for (uint32_t wi = 0; k < b->cnt; wi++)
		for (uint64_t w = ld_upto8(b->highs + 8 * wi, b->end); w && k < b->cnt; w &= w - 1, k++) {
			uint64_t pos = (uint64_t)k * l, h = 64 * wi + (uint32_t)__builtin_ctzll(w) - k;
			uint64_t x = b->x0 + (h << l | ((ld_upto8(b->lows + (pos >> 3), b->highs) >> (pos & 7)) & mask));
			out[k] = SIFT_TID(x >> ob, (x & omask) + 1);
		}
}

/* decode the LIST block b (postings [start, start + cnt)) into ls.blk */
static void ls_decode(sift_cur *c, const ef_blk *b, uint32_t start)
{
	ef_decode(b, c->ls.ob, c->ls.blk);
	c->ls.bi = start, c->ls.bn = b->cnt;
}

/* tid at index i (monotonic access); LIST blocks before the one holding gid >= target are skipped undecoded */
static inline sift_tid ls_at(sift_cur *c, uint32_t *i, uint32_t target)
{
	if (c->ls.t) return c->ls.t[*i];
	while (*i >= c->ls.bi + c->ls.bn) {
		uint32_t start = c->ls.bi + c->ls.bn, cnt = blk_cnt(c->ls.n, start);
		ef_blk b;
		ef_hdr(c->ls.p, c->ls.prev, cnt, &b);
		uint64_t last = b.x0 + b.u - 1, d;
		c->ls.p = b.end, c->ls.prev = last;
		if (*i <= start && start + cnt < c->ls.n && ((last + (get_varint(b.end, &d), d)) >> c->ls.ob >> SIFT_GROUP_SHIFT) < target) {
			c->ls.bi = start + cnt, c->ls.bn = 0, *i = start + cnt;
			continue;
		}
		ls_decode(c, &b, start);
	}
	return c->ls.blk[*i - c->ls.bi];
}

static const sift_grp *ls_seek(sift_cur *c, uint32_t target)
{
	if (c->state == 2) return NULL;
	if (c->state == 1 && c->g.gid >= target) return &c->g;
	uint32_t i = c->ls.i, n = c->ls.n;
	if (c->ls.t) i = gallop_tid(c->ls.t, n, i, target);
	while (i < n && SIFT_GID(ls_at(c, &i, target)) < target) i++;
	if (i >= n) return ended(c);
	builder b = { { 0 }, 0, 0, UINT32_MAX, c->ls.w, c->buf };
	uint32_t base = i, gid = SIFT_GID(ls_at(c, &i, target));
	for (;;) {
		/* tight loop over the decoded window (or the raw array) */
		const sift_tid *t = c->ls.t ? c->ls.t : c->ls.blk - c->ls.bi;
		uint32_t end = c->ls.t ? n : c->ls.bi + c->ls.bn;
		if (b.w <= 8)
			for (; i < end && SIFT_GID(t[i]) == gid; i++) {
				uint32_t pg = SIFT_BLK(t[i]) & (SIFT_GROUP_PAGES - 1);
				if (pg != b.last) {
					build_flush(&b);
					b.cur = 0, b.r++, b.last = pg, b.pages[pg >> 6] |= 1ull << (pg & 63);
				}
				b.cur |= 1ull << (SIFT_OFF(t[i]) - 1);
			}
		else
			for (; i < end && SIFT_GID(t[i]) == gid; i++) build_add(&b, SIFT_BLK(t[i]) & (SIFT_GROUP_PAGES - 1), SIFT_OFF(t[i]) - 1);
		if (i < end || i == n || SIFT_GID(ls_at(c, &i, target)) != gid) break;
	}
	build_flush(&b);
	c->ls.i = i;
	return found(c, gid, b.w, b.pages, c->buf, base, i - base);
}

/* the distinct gids of an unpositioned LIST cursor, ascending: blocks are decoded, no group bitmaps are built */
uint32_t sift_cur_gids(sift_cur *c, uint32_t *out)
{
	uint32_t ng = 0, n = c->ls.n, last = UINT32_MAX;
	sift_tid blk[SIFT_LIST_BLOCK];
	const sift_tid *t = c->ls.t;
	for (uint32_t i = 0; i < n; i += SIFT_LIST_BLOCK) {
		uint32_t cnt = blk_cnt(n, i);
		if (!t) {
			ef_blk b;
			ef_hdr(c->ls.p, c->ls.prev, cnt, &b);
			c->ls.p = b.end, c->ls.prev = b.x0 + b.u - 1;
			ef_decode(&b, c->ls.ob, blk);
		}
		for (uint32_t k = 0; k < cnt; k++) {
			uint32_t gid = SIFT_GID((t ? t + i : blk)[k]);
			if (gid != last) out[ng++] = last = gid;
		}
	}
	return ng;
}

static void cur_init(sift_cur *c, const sift_grp *(*seek)(sift_cur *, uint32_t), uint8_t *buf)
{
	memset(c, 0, offsetof(sift_cur, gr));
	c->seek = seek, c->buf = buf, c->ub1 = 255;
}

void sift_cur_tids(sift_cur *c, const sift_tid *t, uint32_t n, uint8_t *buf)
{
	uint32_t maxoff = 1;
	cur_init(c, ls_seek, buf);
	for (uint32_t i = 0; i < n; i++)
		if (SIFT_OFF(t[i]) > maxoff) maxoff = SIFT_OFF(t[i]);
	c->ls.t = t, c->ls.n = n, c->ls.i = c->ls.bi = c->ls.bn = 0, c->ls.w = sift_w_for(maxoff);
}

void sift_cur_blob(sift_cur *c, const uint8_t *blob, uint8_t *buf)
{
	uint32_t n = sift_blob_n(blob);
	int wtf = blob[0] >> 4 & SIFT_F_TF;
	if (sift_blob_kind(blob) == SIFT_LIST) {
		cur_init(c, ls_seek, buf);
		uint64_t nn;
		c->ls.t = NULL, c->ls.p = get_varint(blob + 2, &nn), c->ls.n = n, c->ls.i = c->ls.bi = c->ls.bn = 0;
		if (wtf) c->ub1 = *c->ls.p++;
		c->ls.ob = blob[1], c->ls.w = sift_w_for(1u << blob[1]), c->ls.prev = 0;
		if (wtf) {
			const uint8_t *p = c->ls.p;
			uint64_t prev = 0;
			for (uint32_t i = 0; i < n; i += SIFT_LIST_BLOCK) {
				ef_blk b;
				ef_hdr(p, prev, blk_cnt(n, i), &b);
				p = b.end, prev = b.x0 + b.u - 1;
			}
			c->tf = p;
		}
		return;
	}
	cur_init(c, gr_seek, buf);
	uint32_t ng = ld32(blob + 8);
	c->gr.ng = ng, c->gr.i = 0;
	c->gr.key = (const uint32_t *)(blob + 12);
	c->gr.poff = c->gr.key + ng;
	c->gr.base = c->gr.poff + ng + 1;
	c->gr.payload = (const uint8_t *)(c->gr.base + ng + 1);
	if (wtf) c->ub = c->gr.payload + c->gr.poff[ng], c->blk = c->ub + ng, c->tf = c->blk + 8 * ng;
}

/* ---- operator cursors ---- */

/*
 * leapfrog all kids to a common gid >= *target whose page bitmaps intersect into m; once the
 * intersection is empty the remaining kids are not sought for that gid
 */
/* gids above this are not wanted (sift_seek_eq): operators stop looking for them and stay usable */
static uint32_t seek_limit = UINT32_MAX;

static int and_align(sift_cur **kid, int k, uint32_t *target, uint64_t m[4])
{
	for (;;) {
		if (*target > seek_limit) return -1;
		const sift_grp *g = sift_seek(kid[0], *target);
		if (!g) return 0;
		*target = g->gid;
		memcpy(m, g->pages, 32);
		int i = 1;
		for (; i < k; i++) {
			if (!(g = sift_seek(kid[i], *target))) return 0;
			if (g->gid != *target) break;
			for (int j = 0; j < 4; j++) m[j] &= g->pages[j];
			if (pages_empty(m)) break;
		}
		if (i == k) return 1;
		*target = g->gid > *target ? g->gid : *target + 1;
	}
}

static const sift_grp *and_seek(sift_cur *c, uint32_t target)
{
	if (c->state == 2) return NULL;
	if (c->state == 1 && c->g.gid >= target) return &c->g;
	sift_cur **kid = c->op.kid;
	uint64_t m[4];
	int found;
	while ((found = and_align(kid, c->op.k, &target, m)) > 0) {
		sift_k->and2(&kid[0]->g, &kid[1]->g, m, &c->g, c->buf);
		for (int i = 2; i < c->op.k && !pages_empty(c->g.pages); i++) sift_k->and2(&c->g, &kid[i]->g, c->g.pages, &c->g, c->buf);
		if (!pages_empty(c->g.pages)) {
			c->state = 1;
			return &c->g;
		}
		target++;
	}
	return found < 0 ? NULL : ended(c);
}

static const sift_grp *or_seek(sift_cur *c, uint32_t target)
{
	if (c->state == 2) return NULL;
	if (c->state == 1 && c->g.gid >= target) return &c->g;
	uint32_t mg = UINT32_MAX;
	for (int i = 0; i < c->op.k; i++) {
		const sift_grp *g = sift_seek(c->op.kid[i], target);
		if (g && g->gid < mg) mg = g->gid;
	}
	if (mg == UINT32_MAX) return seek_limit == UINT32_MAX ? ended(c) : NULL;
	const sift_grp *acc = NULL;
	int nb = 0;
	for (int i = 0; i < c->op.k; i++) {
		sift_cur *k = c->op.kid[i];
		if (k->state != 1 || k->g.gid != mg) continue;
		if (!acc) acc = &k->g;
		else sift_k->or2(acc, &k->g, &c->g, c->buf + (nb++ & 1) * SIFT_GRP_BYTES), acc = &c->g;
	}
	if (acc != &c->g) c->g = *acc;
	c->state = 1;
	return &c->g;
}

static const sift_grp *andnot_seek(sift_cur *c, uint32_t target)
{
	if (c->state == 2) return NULL;
	if (c->state == 1 && c->g.gid >= target) return &c->g;
	for (const sift_grp *a; (a = sift_seek(c->op.kid[0], target)); target = a->gid + 1) {
		if (a->gid > seek_limit) return NULL;
		const sift_grp *b = sift_seek(c->op.kid[1], a->gid);
		if (!b || b->gid != a->gid) {
			c->g = *a;
			c->state = 1;
			return &c->g;
		}
		sift_k->andnot(a, b, &c->g, c->buf);
		if (!pages_empty(c->g.pages)) {
			c->state = 1;
			return &c->g;
		}
	}
	return seek_limit == UINT32_MAX ? ended(c) : NULL;
}

const sift_grp *sift_seek_eq(sift_cur *c, uint32_t gid)
{
	seek_limit = gid;
	const sift_grp *g = c->seek(c, gid);
	seek_limit = UINT32_MAX;
	return g && g->gid == gid ? g : NULL;
}

static void op_init(sift_cur *c, const sift_grp *(*seek)(sift_cur *, uint32_t), sift_cur **kid, int k, uint8_t *buf)
{
	cur_init(c, seek, buf);
	c->op.kid = kid, c->op.k = k;
}

/* postings an unpositioned cursor can yield at most, to put the rarest kid of an AND first */
static uint64_t cur_est(const sift_cur *c)
{
	uint64_t n = 0;
	if (c->seek == gr_seek) return c->gr.base[c->gr.ng];
	if (c->seek == ls_seek) return c->ls.n;
	if (c->seek == or_seek) {
		for (int i = 0; i < c->op.k; i++) n += cur_est(c->op.kid[i]);
		return n;
	}
	if (c->seek == andnot_seek) return cur_est(c->op.kid[0]);
	n = UINT64_MAX;
	for (int i = 0; i < c->op.k; i++) {
		uint64_t e = cur_est(c->op.kid[i]);
		if (e < n) n = e;
	}
	return n;
}

void sift_cur_and(sift_cur *c, sift_cur **kid, int k, uint8_t *buf)
{
	op_init(c, and_seek, kid, k, buf);
	for (int i = 1; i < k; i++) {
		sift_cur *x = kid[i];
		uint64_t e = cur_est(x);
		int j = i;
		for (; j > 0 && cur_est(kid[j - 1]) > e; j--) kid[j] = kid[j - 1];
		kid[j] = x;
	}
}
void sift_cur_or(sift_cur *c, sift_cur **kid, int k, uint8_t *buf) { op_init(c, or_seek, kid, k, buf); }
void sift_cur_andnot(sift_cur *c, sift_cur **kid, uint8_t *buf) { op_init(c, andnot_seek, kid, 2, buf); }

static uint64_t and_count2(sift_cur **kid)
{
	uint64_t m[4], total = 0;
	for (uint32_t t = 0; and_align(kid, 2, &t, m); t++) total += sift_k->and_count(&kid[0]->g, &kid[1]->g);
	return total;
}

/* exact posting count of an unpositioned cursor: O(1) for sources */
static uint64_t cur_total(sift_cur *c)
{
	if (c->state == 0 && c->seek == gr_seek) return c->gr.base[c->gr.ng];
	if (c->state == 0 && c->seek == ls_seek) return c->ls.n;
	return UINT64_MAX;
}

uint64_t sift_count(sift_cur *c)
{
	uint64_t total = 0;
	if ((c->seek == and_seek || c->seek == or_seek || c->seek == andnot_seek) && c->op.k == 2) {
		sift_cur **kid = c->op.kid;
		uint64_t na = cur_total(kid[0]), nb = cur_total(kid[1]);
		if (c->seek == and_seek) return and_count2(kid);
		/* |a| + |b| - |a & b| and |a| - |a & b| from stored counts */
		if (na != UINT64_MAX && nb != UINT64_MAX) return (c->seek == or_seek ? na + nb : na) - and_count2(kid);
	}
	for (const sift_grp *g; (g = sift_next(c));) total += sift_grp_count(g);
	return total;
}

size_t sift_tids(sift_cur *c, sift_tid *out)
{
	size_t n = 0;
	for (const sift_grp *g; (g = sift_next(c));) n += sift_k->tids(g, NULL, out + n);
	return n;
}

/* ---- chunk API ---- */

size_t sift_chunk_bound(uint32_t npages) { return 4 + 32 + (size_t)npages * SIFT_MAX_W; }
uint32_t sift_chunk_count(const uint8_t *chunk) { return ld32(chunk) >> 8; }

size_t sift_chunk_encode(const uint64_t pm[4], const uint64_t *om, int ctype, uint8_t *out)
{
	uint32_t np = pages_pc(pm), count = 0, maxbit = 1;
	for (uint32_t k = 0; k < np * SIFT_OFFMASK_WORDS; k++)
		if (om[k]) count += sift_pc64(om[k]), maxbit = maxbit > (k % 8) * 64 + nbits(om[k]) ? maxbit : (k % 8) * 64 + nbits(om[k]);
	uint32_t w = sift_w_for(maxbit), es = w <= 32 ? 2 : 4;
	int array = ctype == SIFT_CT_ARRAY || (ctype == SIFT_CT_AUTO && (size_t)count * es < 32 + (size_t)np * w);
	size_t size = 4 + (array ? (size_t)count * es : 32 + (size_t)np * w);
	if (!out) return size;
	st32(out, count << 8 | w | (array ? 0x80 : 0));
	uint8_t *p = out + 4;
	if (!array) memcpy(p, pm, 32), p += 32;
	for (uint32_t k = 0, j = 0; j < 4; j++)
		for (uint64_t m = pm[j]; m; m &= m - 1, k++) {
			uint32_t pg = j * 64 + (uint32_t)__builtin_ctzll(m);
			if (!array) {
				memcpy(p, om + k * SIFT_OFFMASK_WORDS, w), p += w;
				continue;
			}
			for (uint32_t q = 0; q < SIFT_OFFMASK_WORDS; q++)
				for (uint64_t v = om[k * SIFT_OFFMASK_WORDS + q]; v; v &= v - 1) {
					uint32_t o = q * 64 + (uint32_t)__builtin_ctzll(v);
					if (es == 2) p[0] = (uint8_t)o, p[1] = (uint8_t)pg, p += 2;
					else st32(p, pg << 16 | o), p += 4;
				}
		}
	return size;
}

void sift_chunk_grp(const uint8_t *chunk, uint32_t gid, sift_grp *g, uint8_t *buf)
{
	uint32_t h = ld32(chunk), w = h & 0x7f, n = h >> 8;
	const uint8_t *p = chunk + 4;
	if (!(h & 0x80)) {
		uint64_t pages[4];
		memcpy(pages, p, 32);
		sift_grp_set(g, gid, w, pages, p + 32);
	} else {
		builder b = { { 0 }, 0, 0, UINT32_MAX, w, buf };
		for (uint32_t k = 0; k < n; k++) {
			uint32_t e = w <= 32 ? (uint32_t)p[2 * k] | (uint32_t)p[2 * k + 1] << 8 : ld32(p + 4 * k);
			build_add(&b, w <= 32 ? e >> 8 : e >> 16, w <= 32 ? e & 0xff : e & 0xffff);
		}
		build_flush(&b);
		sift_grp_set(g, gid, w, b.pages, buf);
	}
	g->n = n;
}

void sift_chunk_pagemask(const uint8_t *chunk, uint64_t pm[4])
{
	uint32_t h = ld32(chunk), n = h >> 8, w = h & 0x7f;
	if (!(h & 0x80)) {
		memcpy(pm, chunk + 4, 32);
		return;
	}
	memset(pm, 0, 32);
	for (uint32_t k = 0; k < n; k++) {
		uint32_t pg = w <= 32 ? chunk[4 + 2 * k + 1] : ld32(chunk + 4 + 4 * k) >> 16;
		pm[pg >> 6] |= 1ull << (pg & 63);
	}
}

uint32_t sift_chunk_offsets(const uint8_t *chunk, const uint64_t sel[4], uint64_t *om)
{
	uint32_t h = ld32(chunk), w = h & 0x7f, n = h >> 8, np = 0, last = UINT32_MAX;
	const uint8_t *p = chunk + 4;
	if (!(h & 0x80)) {
		const uint8_t *off = p + 32;
		for (uint32_t r = 0, j = 0; j < 4; j++)
			for (uint64_t m = ld64(p + 8 * j); m; m &= m - 1, r++)
				if (sel[j] & m & -m) {
					memset(om + np * SIFT_OFFMASK_WORDS, 0, SIFT_OFFMASK_WORDS * 8);
					memcpy(om + np++ * SIFT_OFFMASK_WORDS, off + r * w, w);
				}
		return np;
	}
	for (uint32_t k = 0; k < n; k++) {
		uint32_t e = w <= 32 ? (uint32_t)p[2 * k] | (uint32_t)p[2 * k + 1] << 8 : ld32(p + 4 * k);
		uint32_t pg = w <= 32 ? e >> 8 : e >> 16, o = w <= 32 ? e & 0xff : e & 0xffff;
		if (!(sel[pg >> 6] >> (pg & 63) & 1)) continue;
		if (pg != last) memset(om + np++ * SIFT_OFFMASK_WORDS, 0, SIFT_OFFMASK_WORDS * 8), last = pg;
		om[(np - 1) * SIFT_OFFMASK_WORDS + o / 64] |= 1ull << (o & 63);
	}
	return np;
}

size_t sift_keylist_encode(const sift_tid *t, uint32_t n, uint8_t *out)
{
	static const sift_enc_opts o = { SIFT_LIST, 0, 0, 0 };
	return sift_encode(t, NULL, NULL, n, &o, out);
}

uint32_t sift_keylist_decode(const uint8_t *blob, sift_tid *out)
{
	uint64_t n, prev = 0;
	const uint8_t *p = get_varint(blob + 2, &n) + (blob[0] >> 4 & SIFT_F_TF);
	for (uint32_t i = 0; i < n; i += SIFT_LIST_BLOCK) {
		ef_blk b;
		ef_hdr(p, prev, blk_cnt((uint32_t)n, i), &b);
		ef_decode(&b, blob[1], out + i);
		p = b.end, prev = b.x0 + b.u - 1;
	}
	return (uint32_t)n;
}

void sift_offmask_and(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t np) { sift_k->vop(o, a, b, np * SIFT_OFFMASK_WORDS, SIFT_VOP_AND); }
void sift_offmask_or(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t np) { sift_k->vop(o, a, b, np * SIFT_OFFMASK_WORDS, SIFT_VOP_OR); }
void sift_offmask_andnot(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t np) { sift_k->vop(o, a, b, np * SIFT_OFFMASK_WORDS, SIFT_VOP_ANDNOT); }
uint64_t sift_offmask_popcount(const uint64_t *a, size_t np) { return sift_k->popcount((const uint8_t *)a, np * SIFT_OFFMASK_WORDS * 8); }

/* ---- scoring, top-k, facets ---- */

uint32_t sift_grp_score(const sift_grp *res, const sift_qterm *t, int nt, const sift_norms *nm, float *score, sift_tid *tids, uint8_t *buf)
{
	uint32_t n = sift_k->tids(res, NULL, tids);
	float *kn = (float *)buf, *tf = kn + SIFT_GRP_MAXTIDS;
	uint32_t *rank = (uint32_t *)(tf + SIFT_GRP_MAXTIDS), *pos = rank + SIFT_GRP_MAXTIDS;
	uint8_t *gbuf = (uint8_t *)(pos + SIFT_GRP_MAXTIDS);
	for (uint32_t i = 0; i < n; i++) {
		kn[i] = nm->knorm[nm->rowstart[SIFT_BLK(tids[i]) - nm->blk0] + SIFT_OFF(tids[i]) - 1];
		score[i] = 0;
	}
	for (int j = 0; j < nt; j++) {
		sift_cur *c = t[j].cur;
		if (c->state != 1 || c->g.gid != res->gid) continue;
		if (t[j].rel == SIFT_REL_SUPERSET) {
			sift_k->ranks(&c->g, res, rank);
			for (uint32_t i = 0; i < n; i++) tf[i] = (float)sift_tf_at(c, rank[i]);
			sift_k->bm25(score, kn, tf, NULL, n, t[j].w);
			continue;
		}
		/* positions in res of the term's postings that res holds; their tf ranks in the term */
		sift_grp common = c->g;
		uint32_t m;
		if (t[j].rel == SIFT_REL_ANY) {
			sift_k->and2(res, &c->g, NULL, &common, gbuf);
			if (pages_empty(common.pages)) continue;
			m = sift_k->ranks(&c->g, &common, rank);
		} else {
			m = c->g.n ? c->g.n : (uint32_t)sift_grp_count(&c->g);
			for (uint32_t i = 0; i < m; i++) rank[i] = c->g.base + i;
		}
		sift_k->ranks(res, &common, pos);
		for (uint32_t i = 0; i < m; i++) tf[i] = (float)sift_tf_at(c, rank[i]), pos[i] -= res->base;
		sift_k->bm25(score, kn, tf, pos, m, t[j].w);
	}
	return n;
}

static inline int hit_worse(const sift_hit *a, const sift_hit *b) { return a->score < b->score || (a->score == b->score && a->tid > b->tid); }

static void heap_push(sift_hit *h, uint32_t *n, uint32_t k, sift_hit x)
{
	uint32_t i;
	if (*n < k)
		for (i = (*n)++; i && hit_worse(&x, &h[(i - 1) / 2]); i = (i - 1) / 2) h[i] = h[(i - 1) / 2];
	else if (hit_worse(&h[0], &x))
		for (i = 0;;) {
			uint32_t c = 2 * i + 1;
			if (c >= k) break;
			if (c + 1 < k && hit_worse(&h[c + 1], &h[c])) c++;
			if (!hit_worse(&h[c], &x)) break;
			h[i] = h[c], i = c;
		}
	else
		return;
	h[i] = x;
}

static int hit_cmp(const void *a, const void *b) { return hit_worse(a, b) ? 1 : hit_worse(b, a) ? -1 : 0; }

/* next candidate gid >= target for an OR of terms, aligned with the filter */
static int or_align(const sift_topk_q *q, uint32_t *target)
{
	for (;;) {
		uint32_t mg = UINT32_MAX;
		for (int i = 0; i < q->nt; i++) {
			const sift_grp *g = sift_seek(q->t[i].cur, *target);
			if (g && g->gid < mg) mg = g->gid;
		}
		if (mg == UINT32_MAX) return 0;
		const sift_grp *f = q->filter ? sift_seek(q->filter, mg) : NULL;
		if (q->filter && !f) return 0;
		*target = f ? f->gid : mg;
		if (*target == mg) return 1;
	}
}

/* page mask of the 16-page blocks whose summed bound can still reach theta */
static int blk_mask(const sift_topk_q *q, uint32_t gid, float theta, uint64_t m[4])
{
	float ubb[16] = { 0 };
	for (int i = 0; i < q->nt; i++) {
		sift_cur *c = q->t[i].cur;
		if (c->state != 1 || c->g.gid != gid) continue;
		float w = q->t[i].w / 255.0f;
		for (int b = 0; b < 16; b++) ubb[b] += w * sift_blk_ub(c, b);
	}
	memset(m, 0, 32);
	for (int b = 0; b < 16; b++)
		if (ubb[b] * 1.0001f >= theta) m[b / 4] |= 0xffffull << (b % 4 * 16);
	return !pages_empty(m);
}

uint32_t sift_topk(const sift_topk_q *q, sift_hit *hits, uint8_t *buf, sift_topk_stats *st)
{
	float *score = (float *)buf;
	sift_tid *tids = (sift_tid *)(score + SIFT_GRP_MAXTIDS);
	uint8_t *sbuf = (uint8_t *)(tids + SIFT_GRP_MAXTIDS), *gb = sbuf + SIFT_SCORE_BUF;
	sift_cur *cur[65];
	uint32_t nh = 0, nc = 0;
	for (int i = 0; i < q->nt; i++) {
		cur[nc++] = q->t[i].cur;
		q->t[i].rel = q->conj ? SIFT_REL_SUPERSET : q->filter ? SIFT_REL_ANY : SIFT_REL_SUBSET;
	}
	if (q->filter) cur[nc++] = q->filter;
	for (uint32_t target = 0;; target++) {
		uint64_t m[4], bm[4];
		if (q->conj ? (nc > 1 ? !and_align(cur, (int)nc, &target, m) : !sift_seek(cur[0], target)) : !or_align(q, &target)) break;
		if (q->conj && nc == 1) target = cur[0]->g.gid, memcpy(m, cur[0]->g.pages, 32);
		st->groups++;
		int full = nh == q->k && !q->exhaustive;
		if (full) {
			float ub = 0;
			for (int i = 0; i < q->nt; i++) {
				sift_cur *c = q->t[i].cur;
				if (c->state == 1 && c->g.gid == target) ub += q->t[i].w * ((float)sift_grp_ub(c) / 255.0f);
			}
			if (ub * 1.0001f < hits[0].score || !blk_mask(q, target, hits[0].score, bm)) {
				st->skipped++;
				continue;
			}
		}
		sift_grp res;
		if (q->conj) {
			if (full)
				for (int j = 0; j < 4; j++) m[j] &= bm[j];
			res = cur[0]->g;
			if (nc == 1) sift_k->and2(&res, &res, m, &res, gb);
			for (uint32_t i = 1; i < nc && !pages_empty(res.pages); i++) sift_k->and2(&res, &cur[i]->g, i == 1 ? m : res.pages, &res, gb);
		} else {
			int nb = 0, first = 1;
			for (int i = 0; i < q->nt; i++) {
				sift_cur *c = q->t[i].cur;
				if (c->state != 1 || c->g.gid != target) continue;
				if (first) res = c->g, first = 0;
				else sift_k->or2(&res, &c->g, &res, gb + (nb++ & 1) * SIFT_GRP_BYTES);
			}
			if (q->filter) sift_k->and2(&res, &q->filter->g, full ? bm : NULL, &res, gb + 2 * SIFT_GRP_BYTES);
			else if (full) sift_k->and2(&res, &res, bm, &res, gb + 2 * SIFT_GRP_BYTES);
			for (int i = 0; i < q->nt; i++) q->t[i].rel = q->filter || full ? SIFT_REL_ANY : SIFT_REL_SUBSET;
		}
		if (pages_empty(res.pages)) continue;
		res.base = 0;
		uint32_t n = sift_grp_score(&res, q->t, q->nt, q->nm, score, tids, sbuf);
		st->scored += n;
		for (uint32_t i = 0; i < n; i++)
			if (nh < q->k || score[i] >= hits[0].score) heap_push(hits, &nh, q->k, (sift_hit){ tids[i], score[i] });
	}
	qsort(hits, nh, sizeof(sift_hit), hit_cmp);
	return nh;
}

void sift_facets(sift_cur *q, sift_cur **val, int nv, uint64_t *counts)
{
	for (const sift_grp *g; (g = sift_next(q));)
		for (int v = 0; v < nv; v++) {
			const sift_grp *f = sift_seek(val[v], g->gid);
			if (f && f->gid == g->gid) counts[v] += sift_k->and_count(g, f);
		}
}
