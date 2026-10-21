/* sift_bitmap: posting formats, cursors and operator trees.  See bm25/bitmap.h. */
#include <stdlib.h>
#include "bm25/bitmap_kern.h"
#include "bm25/bitmap_int.h"
#include "core/varint.h"

const sift_kernels *sift_k = &sift_kern_scalar;
static int cur_isa;

static const sift_kernels *const kern_by_isa[SIFT_NISA] = { &sift_kern_scalar, &sift_kern_avx2, &sift_kern_avx512 };

static int isa_ok(int isa)
{
#if defined(__x86_64__)
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
#else
	return isa == SIFT_ISA_SCALAR;
#endif
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

static inline uint64_t ld64(const uint8_t *p) { uint64_t x; memcpy(&x, p, 8); return x; }
static inline uint32_t pages_pc(const uint64_t *p)
{
	return sift_pc64(p[0]) + sift_pc64(p[1]) + sift_pc64(p[2]) + sift_pc64(p[3]);
}

uint32_t sift_w_for(uint32_t maxoff)
{
	uint32_t b = (maxoff + 7) / 8;
	return b <= 1 ? 1 : b <= 2 ? 2 : b <= 4 ? 4 : (b + 7) & ~7u;
}

/* varint at p of at most 56 bits (a key or a count has 41 at most, so a sum of three cannot wrap); a blob is followed by SIFT_KERN_PAD zero bytes, which end a run that the blob does not */
static const uint8_t *get_varint(const uint8_t *p, uint64_t *x)
{
	*x = 0;
	for (int s = 0; s < 56; s += 7) {
		*x |= (uint64_t)(*p & 0x7f) << s;
		if (!(*p++ & 0x80)) return p;
	}
	sift_codec_corrupt("varint longer than 56 bits");
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

/* a group's offset bitmap is 1, 2, 4, 8, 16, ..., 64 bytes per page */
static inline int group_width_ok(uint32_t w) { return w && ((w <= 8 && !(w & (w - 1))) || (w <= SIFT_MAX_W && !(w & 7))); }

/* incremental builder of a decoded group in c->buf; narrow bitmaps accumulate in cur */
typedef struct { uint64_t pages[4], cur; uint32_t r, last, w; uint8_t *off; } builder;

static inline void build_flush(builder *b)
{
	if (b->w <= 8 && b->last != UINT32_MAX) sift_stn(b->off + (b->r - 1) * b->w, b->w, b->cur);
}

static inline void build_add(builder *b, uint32_t pg, uint32_t o)
{
	if (pg != b->last) {
		if (b->last != UINT32_MAX && pg < b->last) sift_codec_corrupt("postings of a group are not in page order");
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

/* the first index >= from of a set bit in the n bits at bits, n if there is none (the blob is followed by SIFT_KERN_PAD zero bytes) */
static uint32_t next_set(const uint8_t *bits, uint32_t from, uint32_t n)
{
	while (from < n) {
		uint64_t x = ld64(bits + (from >> 3)) >> (from & 7);
		if (x) return from + (uint32_t)__builtin_ctzll(x) < n ? from + (uint32_t)__builtin_ctzll(x) : n;
		from += 64 - (from & 7);
	}
	return n;
}

/*
 * A PACKED group (see grp_info): each page of the mask in turn takes its postings, up to the next set bit of the page-end bits, and their
 * offsets are read ob bits at a time from a 64-bit window into the page's bitmap row. The group is checked as it is read (sizes, the
 * page ends, offsets inside the width), so a damaged group raises before it indexes outside the rows.
 */
static const sift_grp *packed_seek(sift_cur *c, uint32_t gid, uint32_t w, const uint8_t *p, uint32_t len, uint32_t base, uint32_t n)
{
	uint32_t ob = len ? p[0] : 0, k = 0, bad = 0, olim = 8 * w;
	uint64_t pages[4], bit = 0, omask;
	if (len < 33 || ob == 0 || ob > 8 * w || ob > 16 || len != packed_bytes(n, ob)) sift_codec_corrupt("GROUPED packed group has the wrong size");
	const uint8_t *offs = p + 33, *last = offs + (((uint64_t)n * ob + 7) >> 3);
	uint8_t *row = c->buf;
	memcpy(pages, p + 1, 32);
	omask = (1ull << ob) - 1;
	for (uint32_t wi = 0; wi < 4; wi++)
		for (uint64_t m = pages[wi]; m; m &= m - 1, row += w) {
			uint32_t e = next_set(last, k, n);
			if (k >= n) sift_codec_corrupt("GROUPED packed group has more pages than postings");
			if (w <= 8) {
				uint64_t cur = 0;
				for (; k <= e && k < n; k++, bit += ob) {
					uint32_t o = (uint32_t)(ld64(offs + (bit >> 3)) >> (bit & 7) & omask);
					bad |= o >= olim;
					cur |= 1ull << (o & 63);
				}
				sift_stn(row, w, cur);
			} else {
				memset(row, 0, w);
				for (; k <= e && k < n; k++, bit += ob) {
					uint32_t o = (uint32_t)(ld64(offs + (bit >> 3)) >> (bit & 7) & omask);
					bad |= o >= olim;
					row[(o & 511) >> 3] |= (uint8_t)(1u << (o & 7));
				}
			}
		}
	if (bad || k != n) sift_codec_corrupt("GROUPED packed group does not match its page mask");
	return found(c, gid, w, pages, c->buf, base, n);
}

static const sift_grp *gr_seek(sift_cur *c, uint32_t target)
{
	if (c->state == 2) return NULL;
	if (c->state == 1 && c->g.gid >= target) return &c->g;
	uint32_t i = gallop32(c->gr.key, c->gr.ng, c->state ? c->gr.i + 1 : 0, (uint64_t)target << 8);
	if (i == c->gr.ng) return ended(c);
	c->gr.i = i;
	uint32_t key = c->gr.key[i], packed = (key & 0xff) >= PACKED_TYPE, w = packed ? packed_w_of(key) : key & 0x7f;
	uint32_t base = c->gr.base[i], n = c->gr.base[i + 1] - base;
	uint32_t lo = c->gr.poff[i], hi = c->gr.poff[i + 1], plen = c->gr.plen;
	const uint8_t *p = c->gr.payload + lo;
	/* the group lies inside the payload and its width and posting count are possible: what follows reads only [p, p + hi - lo) */
	if (!group_width_ok(w) || lo > hi || hi > plen || c->gr.base[i + 1] <= base ||
	    c->gr.base[i + 1] > c->gr.base[c->gr.ng])
		sift_codec_corrupt("GROUPED group extents are out of order");
	/* software prefetch 4 groups ahead: groups are small and visited in order */
	if (i + 4 < c->gr.ng)
		for (uint32_t q = c->gr.poff[i + 4], e = c->gr.poff[i + 5] < plen ? c->gr.poff[i + 5] : plen; q < e; q += 64) __builtin_prefetch(c->gr.payload + q);
	if (packed) return packed_seek(c, key >> 8, w, p, hi - lo, base, n);
	if (!(key & 0x80)) {
		uint64_t pages[4];
		if (hi - lo < 32) sift_codec_corrupt("GROUPED bitmap group is shorter than its page mask");
		memcpy(pages, p, 32);
		if (hi - lo - 32 != sift_popcount((const uint8_t *)pages, 32) * w) sift_codec_corrupt("GROUPED bitmap group does not match its page mask");
		return found(c, key >> 8, w, pages, p + 32, base, n);
	}
	if (hi - lo != (uint64_t)n * (w <= 32 ? 2 : 4)) sift_codec_corrupt("GROUPED array group has the wrong size");
	/* the entries are page << 8 | offset (u16) or page << 16 | offset (u32): they are masked into the builder's arrays and range checked
	 * once for the group, so a damaged entry raises after the loop and never indexes outside them */
	builder b = { { 0 }, 0, 0, UINT32_MAX, w, c->buf };
	uint32_t bad = 0, olim = 8 * w, omask = w <= 8 ? 63 : 511;
	for (uint32_t k = 0; k < n; k++) {
		uint32_t e = w <= 32 ? (uint32_t)p[2 * k] | (uint32_t)p[2 * k + 1] << 8 : ld32(p + 4 * k);
		uint32_t pg = w <= 32 ? e >> 8 : e >> 16, o = w <= 32 ? e & 0xff : e & 0xffff;
		bad |= (pg >= SIFT_GROUP_PAGES) | (o >= olim);
		build_add(&b, pg & (SIFT_GROUP_PAGES - 1), o & omask);
	}
	if (bad) sift_codec_corrupt("GROUPED array entry lies outside its page bitmap");
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

/*
 * The header of the block of cnt postings at p, whose keys are below lim (a power of two: a key's group number then fits 24 bits, and
 * sift_next never wraps). chk: the block lies inside the blob that ends at end and its keys are below lim. A cursor checks a block
 * once: when it opens a blob that has a tf stream, it walks every header to find the stream and later reads without chk; otherwise
 * each block is checked as it is read. A varint of at most 9 bytes that starts before end stays inside the zero bytes after the blob.
 */
static inline void ef_hdr(const uint8_t *p, const uint8_t *end, uint64_t lim, uint64_t prev, uint32_t cnt, int chk, ef_blk *b)
{
	uint64_t d, u1;
	p = get_varint(get_varint(p, &d), &u1);
	if (chk && (p > end || u1 + 1 < cnt || prev + d + u1 >= lim)) sift_codec_corrupt("LIST block header is out of bounds");
	b->x0 = prev + d, b->u = u1 + 1, b->cnt = cnt, b->l = ef_l(b->u, cnt);
	size_t lows = ef_lows(cnt, b->l), highs = ef_highs(b->u, cnt, b->l);
	if (chk && (size_t)(end - p) < lows + highs) sift_codec_corrupt("LIST block overruns the blob");
	b->lows = p, b->highs = p + lows, b->end = b->highs + highs;
}

static void ef_decode(const ef_blk *b, uint32_t ob, sift_tid *out)
{
	uint32_t l = b->l, k = 0;
	uint64_t mask = (1ull << l) - 1, omask = (1ull << ob) - 1;
	size_t nw = (size_t)(b->end - b->highs + 7) / 8;
	/* the high bits are the block's own bytes: a block whose bits run out before cnt postings is damaged */
	for (uint32_t wi = 0; wi < nw && k < b->cnt; wi++)
		for (uint64_t w = ld_upto8(b->highs + 8 * wi, b->end); w && k < b->cnt; w &= w - 1, k++) {
			uint64_t pos = (uint64_t)k * l, h = 64 * wi + (uint32_t)__builtin_ctzll(w) - k;
			uint64_t x = b->x0 + (h << l | ((ld_upto8(b->lows + (pos >> 3), b->highs) >> (pos & 7)) & mask));
			out[k] = SIFT_TID(x >> ob, (x & omask) + 1);
		}
	if (k != b->cnt) sift_codec_corrupt("LIST block high bits do not count its postings");
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
		ef_hdr(c->ls.p, c->ls.end, c->ls.lim, c->ls.prev, cnt, !c->tf, &b);
		uint64_t last = b.x0 + b.u - 1, d;
		c->ls.p = b.end, c->ls.prev = last;
		/* the next block starts at last + d: when that is before the target group, skip this block undecoded */
		if (*i <= start && start + cnt < c->ls.n) {
			get_varint(b.end, &d);
			if (((last + d) >> c->ls.ob >> SIFT_GROUP_SHIFT) < target) {
				c->ls.bi = start + cnt, c->ls.bn = 0, *i = start + cnt;
				continue;
			}
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
					if (b.last != UINT32_MAX && pg < b.last) sift_codec_corrupt("postings of a group are not in page order");
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
			ef_hdr(c->ls.p, c->ls.end, c->ls.lim, c->ls.prev, cnt, !c->tf, &b);
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

/* the first and last gid of an unpositioned, non-empty LIST cursor: only the block headers are read */
void sift_cur_gid_span(const sift_cur *c, uint32_t *lo, uint32_t *hi)
{
	uint32_t n = c->ls.n;
	if (c->ls.t) {
		*lo = SIFT_GID(c->ls.t[0]), *hi = SIFT_GID(c->ls.t[n - 1]);
		return;
	}
	const uint8_t *p = c->ls.p;
	uint64_t prev = c->ls.prev, first = 0;
	for (uint32_t i = 0; i < n; i += SIFT_LIST_BLOCK) {
		ef_blk b;
		ef_hdr(p, c->ls.end, c->ls.lim, prev, blk_cnt(n, i), !c->tf, &b);
		if (i == 0) first = b.x0;
		p = b.end, prev = b.x0 + b.u - 1;
	}
	*lo = (uint32_t)(first >> c->ls.ob >> SIFT_GROUP_SHIFT), *hi = (uint32_t)(prev >> c->ls.ob >> SIFT_GROUP_SHIFT);
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

void sift_cur_blob(sift_cur *c, const uint8_t *blob, size_t len, uint8_t *buf)
{
	const uint8_t *end = blob + len;
	if (len < 3 || blob[0] >> 4 & ~SIFT_F_TF) sift_codec_corrupt("blob header is invalid");
	int wtf = blob[0] >> 4 & SIFT_F_TF;
	if (sift_blob_kind(blob) == SIFT_LIST) {
		uint64_t nn;
		const uint8_t *p;
		if (blob[1] > SIFT_LIST_OB_MAX) sift_codec_corrupt("LIST offset bits out of range");
		if (!(p = sift_get_varint_n(blob + 2, end, &nn)) || nn > UINT32_MAX || (wtf && p >= end)) sift_codec_corrupt("LIST header overruns the blob");
		uint32_t n = (uint32_t)nn;
		cur_init(c, ls_seek, buf);
		c->ls.t = NULL, c->ls.n = n, c->ls.i = c->ls.bi = c->ls.bn = 0;
		if (wtf) c->ub1 = *p++;
		c->ls.p = p, c->ls.end = end, c->ls.lim = 1ull << (32 + blob[1]);
		c->ls.ob = blob[1], c->ls.w = sift_w_for(1u << blob[1]), c->ls.prev = 0;
		if (wtf) {
			uint64_t prev = 0;
			for (uint32_t i = 0; i < n; i += SIFT_LIST_BLOCK) {
				ef_blk b;
				ef_hdr(p, end, c->ls.lim, prev, blk_cnt(n, i), 1, &b);
				p = b.end, prev = b.x0 + b.u - 1;
			}
			if ((size_t)(end - p) < ((size_t)n + 1) / 2) sift_codec_corrupt("LIST tf stream overruns the blob");
			c->tf = p;
		}
		return;
	}
	if (sift_blob_kind(blob) != SIFT_GROUPED) sift_codec_corrupt("blob kind is unknown");
	if (len < 12) sift_codec_corrupt("GROUPED header overruns the blob");
	uint64_t n = ld32(blob + 4), ng = ld32(blob + 8), tables = 12 + 4 * (3 * ng + 2), plen;
	if (ng > n) sift_codec_corrupt("GROUPED group count is out of range");
	if (tables > len) sift_codec_corrupt("GROUPED tables overrun the blob");
	cur_init(c, gr_seek, buf);
	c->gr.ng = (uint32_t)ng, c->gr.i = 0;
	c->gr.key = (const uint32_t *)(blob + 12);
	c->gr.poff = c->gr.key + ng;
	c->gr.base = c->gr.poff + ng + 1;
	c->gr.payload = blob + tables;
	plen = c->gr.poff[ng];
	if (plen > len - tables || (wtf && len - tables - plen < 9 * ng + (n + 1) / 2)) sift_codec_corrupt("GROUPED payload overruns the blob");
	if (c->gr.poff[0] != 0 || c->gr.base[0] != 0 || c->gr.base[ng] != n) sift_codec_corrupt("GROUPED offsets do not start at zero and end at the count");
	c->gr.plen = (uint32_t)plen;
	if (wtf) c->ub = c->gr.payload + plen, c->blk = c->ub + ng, c->tf = c->blk + 8 * ng;
}

/* ---- operator cursors ---- */

/*
 * leapfrog all kids to a common gid >= *target whose page bitmaps intersect into m; once the
 * intersection is empty the remaining kids are not sought for that gid
 */
/* gids above this are not wanted (sift_seek_eq): operators stop looking for them and stay usable */
static uint32_t seek_limit = UINT32_MAX;

int and_align(sift_cur **kid, int k, uint32_t *target, uint64_t m[4])
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
uint32_t sift_chunk_width(const uint8_t *chunk) { return ld32(chunk) & 0x7f; }

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

uint32_t sift_keylist_decode(const uint8_t *blob, size_t len, sift_tid *out)
{
	const uint8_t *end = blob + len, *p;
	uint64_t n, prev = 0, lim;
	if (len < 3 || sift_blob_kind(blob) != SIFT_LIST || blob[1] > SIFT_LIST_OB_MAX) sift_codec_corrupt("blob header is invalid");
	lim = 1ull << (32 + blob[1]);
	if (!(p = sift_get_varint_n(blob + 2, end, &n)) || n > UINT32_MAX || ((blob[0] >> 4 & SIFT_F_TF) && p++ >= end)) sift_codec_corrupt("LIST header overruns the blob");
	for (uint32_t i = 0; i < n; i += SIFT_LIST_BLOCK) {
		ef_blk b;
		ef_hdr(p, end, lim, prev, blk_cnt((uint32_t)n, i), 1, &b);
		ef_decode(&b, blob[1], out + i);
		p = b.end, prev = b.x0 + b.u - 1;
	}
	return (uint32_t)n;
}

void sift_offmask_and(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t np) { sift_k->vop(o, a, b, np * SIFT_OFFMASK_WORDS, SIFT_VOP_AND); }
void sift_offmask_or(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t np) { sift_k->vop(o, a, b, np * SIFT_OFFMASK_WORDS, SIFT_VOP_OR); }
void sift_offmask_andnot(uint64_t *o, const uint64_t *a, const uint64_t *b, size_t np) { sift_k->vop(o, a, b, np * SIFT_OFFMASK_WORDS, SIFT_VOP_ANDNOT); }
uint64_t sift_offmask_popcount(const uint64_t *a, size_t np) { return sift_k->popcount((const uint8_t *)a, np * SIFT_OFFMASK_WORDS * 8); }
