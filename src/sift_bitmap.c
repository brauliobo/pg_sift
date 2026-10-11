/* sift_bitmap: posting formats, cursors and operator trees.  See sift_bitmap.h. */
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

static inline uint32_t ld32(const uint8_t *p) { uint32_t x; memcpy(&x, p, 4); return x; }
static inline void st32(uint8_t *p, uint32_t x) { memcpy(p, &x, 4); }
static inline uint32_t nbits(uint64_t x) { return x ? 64 - (uint32_t)__builtin_clzll(x) : 0; }
static inline int pages_empty(const uint64_t *p) { return !(p[0] | p[1] | p[2] | p[3]); }
static inline uint32_t pages_pc(const uint64_t *p)
{
	return (uint32_t)(__builtin_popcountll(p[0]) + __builtin_popcountll(p[1]) + __builtin_popcountll(p[2]) + __builtin_popcountll(p[3]));
}

uint32_t sift_w_for(uint32_t maxoff)
{
	uint32_t b = (maxoff + 7) / 8;
	return b <= 1 ? 1 : b <= 2 ? 2 : b <= 4 ? 4 : (b + 7) & ~7u;
}

uint32_t sift_tf_quant(uint32_t tf) { return tf > 15 ? 15 : tf ? tf : 1; }
uint32_t sift_blob_n(const uint8_t *blob) { return ld32(blob + 4); }
int sift_blob_kind(const uint8_t *blob) { return blob[0]; }

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

typedef struct { uint32_t bb, ob, cnt; } blk_info;

static blk_info list_blk(const sift_tid *t, uint32_t i, uint32_t n)
{
	blk_info b = { 0, 0, n - i < SIFT_LIST_BLOCK ? n - i : SIFT_LIST_BLOCK };
	uint32_t md = 0, mo = 0;
	for (uint32_t k = i + 1; k < i + b.cnt; k++) {
		uint32_t d = SIFT_BLK(t[k]) - SIFT_BLK(t[k - 1]);
		if (d > md) md = d;
		if (SIFT_OFF(t[k]) - 1 > mo) mo = SIFT_OFF(t[k]) - 1;
	}
	b.bb = nbits(md), b.ob = nbits(mo);
	return b;
}

static size_t blk_bytes(const blk_info *b) { return 8 + ((size_t)(b->cnt - 1) * (b->bb + b->ob) + 7) / 8; }

static size_t list_size(const sift_tid *t, uint32_t n)
{
	size_t s = 8;
	for (uint32_t i = 0; i < n; i += SIFT_LIST_BLOCK) {
		blk_info b = list_blk(t, i, n);
		s += blk_bytes(&b);
	}
	return s;
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

static uint8_t *write_list(const sift_tid *t, uint32_t n, uint8_t *p)
{
	for (uint32_t i = 0; i < n; i += SIFT_LIST_BLOCK) {
		blk_info b = list_blk(t, i, n);
		uint32_t f = b.bb + b.ob;
		uint64_t acc = 0;
		int nb = 0;
		st32(p, SIFT_BLK(t[i]));
		p[4] = (uint8_t)SIFT_OFF(t[i]), p[5] = (uint8_t)(SIFT_OFF(t[i]) >> 8), p[6] = (uint8_t)b.bb, p[7] = (uint8_t)b.ob;
		p += 8;
		for (uint32_t k = i + 1; k < i + b.cnt; k++) {
			acc |= ((uint64_t)(SIFT_BLK(t[k]) - SIFT_BLK(t[k - 1])) << b.ob | (SIFT_OFF(t[k]) - 1)) << nb;
			for (nb += (int)f; nb >= 8; nb -= 8, acc >>= 8) *p++ = (uint8_t)acc;
		}
		if (nb) *p++ = (uint8_t)acc;
	}
	return p;
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

size_t sift_encode(const sift_tid *t, const uint8_t *tf, uint32_t n, const sift_enc_opts *o, uint8_t *out)
{
	static const sift_enc_opts dflt = { 0 };
	if (!o) o = &dflt;
	uint32_t ng, maxoff = 0;
	size_t gs = o->kind == SIFT_LIST ? 0 : grouped_size(t, n, o->ctype, &ng);
	size_t ls = o->kind == SIFT_GROUPED ? 0 : list_size(t, n);
	int kind = o->kind ? o->kind : (double)gs <= (o->bias > 0 ? o->bias : 1.0) * (double)ls ? SIFT_GROUPED : SIFT_LIST;
	int wtf = o->tf && tf;
	if (!out) return (kind == SIFT_LIST ? ls : gs) + (wtf ? (kind == SIFT_GROUPED ? ng : 0) + (n + 1) / 2 : 0);
	for (uint32_t i = 0; i < n; i++)
		if (SIFT_OFF(t[i]) > maxoff) maxoff = SIFT_OFF(t[i]);
	out[0] = (uint8_t)kind, out[1] = wtf ? SIFT_F_TF : 0, out[2] = (uint8_t)sift_w_for(maxoff), out[3] = 0;
	st32(out + 4, n);
	uint8_t *p = out + 8;
	if (kind == SIFT_LIST) {
		p = write_list(t, n, p);
	} else {
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
		if (wtf)
			for (uint32_t i = 0; i < n;) {
				uint32_t j = i, m = 0;
				for (; j < n && SIFT_GID(t[j]) == SIFT_GID(t[i]); j++)
					if (sift_tf_quant(tf[j]) > m) m = sift_tf_quant(tf[j]);
				*p++ = (uint8_t)m;
				i = j;
			}
	}
	if (wtf) p = write_tf(tf, n, p);
	return (size_t)(p - out);
}

/* ---- source cursors ---- */

/* first index >= i with key[idx] >= v (galloping) */
static uint32_t gallop32(const uint32_t *key, uint32_t n, uint32_t i, uint32_t v)
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

/* incremental builder of a decoded group in c->buf */
typedef struct { uint64_t pages[4]; uint32_t r, last, w; uint8_t *off; } builder;

static inline void build_add(builder *b, uint32_t pg, uint32_t o)
{
	if (pg != b->last) memset(b->off + b->r++ * b->w, 0, b->w), b->last = pg, b->pages[pg >> 6] |= 1ull << (pg & 63);
	b->off[(b->r - 1) * b->w + (o >> 3)] |= (uint8_t)(1u << (o & 7));
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
	uint32_t i = gallop32(c->gr.key, c->gr.ng, c->state ? c->gr.i + 1 : 0, target << 8);
	if (i == c->gr.ng) return ended(c);
	c->gr.i = i;
	uint32_t key = c->gr.key[i], w = key & 0x7f, base = c->gr.base[i], n = c->gr.base[i + 1] - base;
	const uint8_t *p = c->gr.payload + c->gr.poff[i];
	if (i + 1 < c->gr.ng) __builtin_prefetch(c->gr.payload + c->gr.poff[i + 1]);
	if (!(key & 0x80)) {
		uint64_t pages[4];
		memcpy(pages, p, 32);
		return found(c, key >> 8, w, pages, p + 32, base, n);
	}
	builder b = { { 0 }, 0, UINT32_MAX, w, c->buf };
	for (uint32_t k = 0; k < n; k++) {
		uint32_t e = w <= 32 ? (uint32_t)p[2 * k] | (uint32_t)p[2 * k + 1] << 8 : ld32(p + 4 * k);
		build_add(&b, w <= 32 ? e >> 8 : e >> 16, w <= 32 ? e & 0xff : e & 0xffff);
	}
	return found(c, key >> 8, w, b.pages, c->buf, base, n);
}

/* decode the LIST block at c->ls.p into ls.blk */
static void ls_decode(sift_cur *c, uint32_t start, uint32_t cnt)
{
	const uint8_t *p = c->ls.p;
	uint32_t blk = ld32(p), off = (uint32_t)p[4] | (uint32_t)p[5] << 8, ob = p[7], f = p[6] + ob;
	uint64_t acc = 0, mask = (f < 64 ? 1ull << f : 0) - 1;
	int nb = 0;
	c->ls.blk[0] = SIFT_TID(blk, off);
	p += 8;
	for (uint32_t k = 1; k < cnt; k++) {
		for (; nb < (int)f; nb += 8) acc |= (uint64_t)*p++ << nb;
		uint64_t v = acc & mask;
		acc = f < 64 ? acc >> f : 0, nb -= (int)f;
		blk += (uint32_t)(v >> ob);
		c->ls.blk[k] = SIFT_TID(blk, (v & ((1u << ob) - 1)) + 1);
	}
	c->ls.bi = start, c->ls.bn = cnt;
}

static size_t ls_blk_bytes(const uint8_t *p, uint32_t cnt) { return 8 + ((size_t)(cnt - 1) * (p[6] + p[7]) + 7) / 8; }

/* tid at index i (monotonic access); LIST blocks before the one holding gid >= target are skipped undecoded */
static inline sift_tid ls_at(sift_cur *c, uint32_t *i, uint32_t target)
{
	if (c->ls.t) return c->ls.t[*i];
	while (*i >= c->ls.bi + c->ls.bn) {
		uint32_t start = c->ls.bi + c->ls.bn, cnt = c->ls.n - start < SIFT_LIST_BLOCK ? c->ls.n - start : SIFT_LIST_BLOCK;
		const uint8_t *nx = c->ls.p + ls_blk_bytes(c->ls.p, cnt);
		if (*i <= start && start + cnt < c->ls.n && (ld32(nx) >> SIFT_GROUP_SHIFT) < target) {
			c->ls.p = nx, c->ls.bi = start + cnt, c->ls.bn = 0, *i = start + cnt;
			continue;
		}
		ls_decode(c, start, cnt);
		c->ls.p = nx;
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
	builder b = { { 0 }, 0, UINT32_MAX, c->ls.w, c->buf };
	uint32_t base = i, gid = SIFT_GID(ls_at(c, &i, target));
	for (sift_tid t; i < n && SIFT_GID(t = ls_at(c, &i, target)) == gid; i++)
		build_add(&b, SIFT_BLK(t) & (SIFT_GROUP_PAGES - 1), SIFT_OFF(t) - 1);
	c->ls.i = i;
	return found(c, gid, b.w, b.pages, c->buf, base, i - base);
}

static void cur_init(sift_cur *c, const sift_grp *(*seek)(sift_cur *, uint32_t), uint8_t *buf)
{
	memset(c, 0, offsetof(sift_cur, gr));
	c->seek = seek, c->buf = buf;
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
	uint32_t n = ld32(blob + 4);
	int wtf = blob[1] & SIFT_F_TF;
	if (blob[0] == SIFT_LIST) {
		cur_init(c, ls_seek, buf);
		c->ls.t = NULL, c->ls.p = blob + 8, c->ls.n = n, c->ls.i = c->ls.bi = c->ls.bn = 0, c->ls.w = blob[2];
		if (wtf) {
			const uint8_t *p = blob + 8;
			for (uint32_t i = 0; i < n; i += SIFT_LIST_BLOCK) p += ls_blk_bytes(p, n - i < SIFT_LIST_BLOCK ? n - i : SIFT_LIST_BLOCK);
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
	if (wtf) c->maxtf = c->gr.payload + c->gr.poff[ng], c->tf = c->maxtf + ng;
}

/* ---- operator cursors ---- */

/* leapfrog all kids to a common gid >= *target whose page bitmaps intersect into m */
static int and_align(sift_cur **kid, int k, uint32_t *target, uint64_t m[4])
{
	for (;;) {
		const sift_grp *g = sift_seek(kid[0], *target);
		if (!g) return 0;
		*target = g->gid;
		int i = 1;
		for (; i < k; i++) {
			if (!(g = sift_seek(kid[i], *target))) return 0;
			if (g->gid != *target) break;
		}
		if (i < k) {
			*target = g->gid;
			continue;
		}
		memcpy(m, kid[0]->g.pages, 32);
		for (i = 1; i < k; i++)
			for (int j = 0; j < 4; j++) m[j] &= kid[i]->g.pages[j];
		if (!pages_empty(m)) return 1;
		(*target)++;
	}
}

static const sift_grp *and_seek(sift_cur *c, uint32_t target)
{
	if (c->state == 2) return NULL;
	if (c->state == 1 && c->g.gid >= target) return &c->g;
	sift_cur **kid = c->op.kid;
	uint64_t m[4];
	while (and_align(kid, c->op.k, &target, m)) {
		sift_k->and2(&kid[0]->g, &kid[1]->g, m, &c->g, c->buf);
		for (int i = 2; i < c->op.k && !pages_empty(c->g.pages); i++) sift_k->and2(&c->g, &kid[i]->g, c->g.pages, &c->g, c->buf);
		if (!pages_empty(c->g.pages)) {
			c->state = 1;
			return &c->g;
		}
		target++;
	}
	return ended(c);
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
	if (mg == UINT32_MAX) return ended(c);
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
	return ended(c);
}

static void op_init(sift_cur *c, const sift_grp *(*seek)(sift_cur *, uint32_t), sift_cur **kid, int k, uint8_t *buf)
{
	cur_init(c, seek, buf);
	c->op.kid = kid, c->op.k = k;
}

void sift_cur_and(sift_cur *c, sift_cur **kid, int k, uint8_t *buf) { op_init(c, and_seek, kid, k, buf); }
void sift_cur_or(sift_cur *c, sift_cur **kid, int k, uint8_t *buf) { op_init(c, or_seek, kid, k, buf); }
void sift_cur_andnot(sift_cur *c, sift_cur **kid, uint8_t *buf) { op_init(c, andnot_seek, kid, 2, buf); }

uint64_t sift_count(sift_cur *c)
{
	uint64_t total = 0;
	if (c->seek == and_seek && c->op.k == 2) {
		uint64_t m[4];
		for (uint32_t t = 0; and_align(c->op.kid, 2, &t, m); t++) total += sift_k->and_count(&c->op.kid[0]->g, &c->op.kid[1]->g);
		return total;
	}
	if (c->seek == or_seek && c->op.k == 2) {
		sift_cur *a = c->op.kid[0], *b = c->op.kid[1];
		for (uint32_t t = 0;;) {
			const sift_grp *ga = sift_seek(a, t), *gb = sift_seek(b, t);
			if (!ga && !gb) return total;
			if (ga && gb && ga->gid == gb->gid) total += sift_grp_count(ga) + sift_grp_count(gb) - sift_k->and_count(ga, gb), t = ga->gid + 1;
			else if (ga && (!gb || ga->gid < gb->gid)) total += sift_grp_count(ga), t = ga->gid + 1;
			else total += sift_grp_count(gb), t = gb->gid + 1;
		}
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
