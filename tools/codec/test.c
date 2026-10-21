/* Randomized property tests of sift_bitmap against sorted-array reference implementations. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/bm25/bitmap.h"
#include "../../src/core/cmp.h"

#define MAXN 200000

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void)
{
	rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
	return rng;
}
static uint32_t rndn(uint32_t n) { return (uint32_t)(rnd() % n); }

static long fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { if (fails++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

typedef struct { sift_tid *t; uint8_t *tf; uint32_t n; } set;

/* random set with a shape: base block, page span, offsets 1..maxoff, density */
static void gen(set *s, uint32_t n, uint64_t base, uint32_t span, uint32_t maxoff)
{
	for (uint32_t i = 0; i < n; i++) {
		uint64_t blk = base + rndn(span);
		if (blk > 0xffffffffull) blk = 0xffffffffull;
		uint32_t off = rnd() % 4 == 0 ? maxoff : 1 + rndn(maxoff);
		s->t[i] = SIFT_TID(blk, off);
	}
	qsort(s->t, n, 8, sift_cmp_u64);
	uint32_t k = 0;
	for (uint32_t i = 0; i < n; i++)
		if (!k || s->t[i] != s->t[k - 1]) s->t[k++] = s->t[i];
	s->n = k;
	for (uint32_t i = 0; i < k; i++) s->tf[i] = (uint8_t)(rnd() % 8 ? 1 + rndn(3) : rndn(256));
}

static void gen_shape(set *s)
{
	static const uint32_t offs[] = { 1, 7, 8, 9, 16, 17, 32, 33, 64, 65, 100, 180, 255, 256, 257, 291, 400, 450, 512 };
	static const uint64_t bases[] = { 0, 12345, 0xffffffffull - 3000, 0xfffffe00ull, 1u << 24 };
	uint32_t maxoff = offs[rndn(19)], span = 1 + rndn(rnd() % 2 ? 600 : 100000);
	uint32_t n = rnd() % 10 == 0 ? rndn(3) : rndn(rnd() % 3 ? 3000 : 60000);
	if (rnd() % 8 == 0) { /* full pages */
		uint64_t b0 = bases[rndn(5)];
		uint32_t k = 0, pages = 1 + rndn(300);
		for (uint32_t p = 0; p < pages && k < MAXN; p++)
			for (uint32_t o = 1; o <= maxoff && k < MAXN; o++) s->t[k] = SIFT_TID(b0 + p > 0xffffffffull ? 0xffffffffull : b0 + p, o), k++;
		qsort(s->t, k, 8, sift_cmp_u64);
		uint32_t u = 0;
		for (uint32_t i = 0; i < k; i++)
			if (!u || s->t[i] != s->t[u - 1]) s->t[u++] = s->t[i];
		s->n = u;
		for (uint32_t i = 0; i < u; i++) s->tf[i] = (uint8_t)(1 + rndn(20));
		return;
	}
	gen(s, n, bases[rndn(5)] + rndn(1000), span, maxoff);
}

static uint32_t ref_and(const set *a, const set *b, sift_tid *o)
{
	uint32_t i = 0, j = 0, n = 0;
	while (i < a->n && j < b->n)
		if (a->t[i] < b->t[j]) i++;
		else if (a->t[i] > b->t[j]) j++;
		else o[n++] = a->t[i++], j++;
	return n;
}

static uint32_t ref_or(const set *a, const set *b, sift_tid *o)
{
	uint32_t i = 0, j = 0, n = 0;
	while (i < a->n || j < b->n)
		if (j == b->n || (i < a->n && a->t[i] < b->t[j])) o[n++] = a->t[i++];
		else if (i == a->n || b->t[j] < a->t[i]) o[n++] = b->t[j++];
		else o[n++] = a->t[i++], j++;
	return n;
}

static uint32_t ref_andnot(const set *a, const set *b, sift_tid *o)
{
	uint32_t i = 0, j = 0, n = 0;
	while (i < a->n)
		if (j == b->n || a->t[i] < b->t[j]) o[n++] = a->t[i++];
		else if (a->t[i] > b->t[j]) j++;
		else i++, j++;
	return n;
}

static sift_tid *outbuf;
static uint8_t *blob[4];
static uint8_t cbuf[8][SIFT_CUR_BUF];

static int same(const sift_tid *x, size_t nx, const sift_tid *y, size_t ny)
{
	return nx == ny && !memcmp(x, y, nx * 8);
}

static void open_set(sift_cur *c, const set *s, int mode, int slot)
{
	static const sift_enc_opts opts[] = { { 0, 0, 0, 1 }, { SIFT_LIST, 0, 0, 1 }, { SIFT_GROUPED, SIFT_CT_BITMAP, 0, 1 },
					      { SIFT_GROUPED, SIFT_CT_ARRAY, 0, 0 }, { SIFT_GROUPED, SIFT_CT_PACKED, 0, 1 } };
	if (mode == 5) {
		sift_cur_tids(c, s->t, s->n, cbuf[slot]);
		return;
	}
	size_t len = sift_encode(s->t, s->tf, s->tf, s->n, &opts[mode], blob[slot]);
	CHECK(len <= sift_encode_bound(s->n), "bound %zu > %zu", len, sift_encode_bound(s->n));
	CHECK(len == sift_encode(s->t, s->tf, s->tf, s->n, &opts[mode], NULL), "size query mismatch mode %d: %zu vs %zu n %u", mode, len, sift_encode(s->t, s->tf, s->tf, s->n, &opts[mode], NULL), s->n);
	CHECK(sift_blob_n(blob[slot]) == s->n, "n");
	sift_cur_blob(c, blob[slot], len, cbuf[slot]);
}

static void check_tf(const set *s, int mode, int slot, sift_cur *src, const sift_grp *res, const sift_tid *rt)
{
	static uint32_t rank[SIFT_GRP_MAXTIDS];
	if (mode == 5 || mode == 3 || !src->tf) return;
	uint32_t n = sift_grp_ranks(&src->g, res, rank);
	for (uint32_t i = 0; i < n; i++) {
		CHECK(rank[i] < s->n && s->t[rank[i]] == rt[i], "rank %u -> %u", i, rank[i]);
		if (rank[i] < s->n) CHECK(sift_tf_at(src, rank[i]) == sift_tf_quant(s->tf[rank[i]]), "tf");
	}
	(void)slot;
}

static void test_ops(set *a, set *b, set *c, sift_tid *ref, sift_tid *tmp)
{
	int ma = (int)rndn(6), mb = (int)rndn(6), mc = (int)rndn(6);
	sift_cur ca, cb, cc, op, op2, *kids[3];
	size_t n, rn;

	/* decode */
	open_set(&ca, a, ma, 0);
	n = sift_tids(&ca, outbuf);
	CHECK(same(outbuf, n, a->t, a->n), "decode mode %d n %u got %zu", ma, a->n, n);
	open_set(&ca, a, ma, 0);
	CHECK(sift_count(&ca) == a->n, "count");

	/* and / or / andnot, materialized and counted */
	for (int op_kind = 0; op_kind < 3; op_kind++) {
		for (int counted = 0; counted < 2; counted++) {
			open_set(&ca, a, ma, 0), open_set(&cb, b, mb, 1);
			kids[0] = &ca, kids[1] = &cb;
			if (op_kind == 0) sift_cur_and(&op, kids, 2, cbuf[4]), rn = ref_and(a, b, ref);
			else if (op_kind == 1) sift_cur_or(&op, kids, 2, cbuf[4]), rn = ref_or(a, b, ref);
			else sift_cur_andnot(&op, kids, cbuf[4]), rn = ref_andnot(a, b, ref);
			if (counted) {
				uint64_t cnt = sift_count(&op);
				CHECK(cnt == rn, "count op %d modes %d/%d: %lu vs %zu", op_kind, ma, mb, cnt, rn);
			} else {
				n = sift_tids(&op, outbuf);
				CHECK(same(outbuf, n, ref, rn), "op %d modes %d/%d: %zu vs %zu", op_kind, ma, mb, n, rn);
			}
		}
	}

	/* tf ranks of AND results in both sources, group by group */
	open_set(&ca, a, ma, 0), open_set(&cb, b, mb, 1);
	kids[0] = &ca, kids[1] = &cb;
	sift_cur_and(&op, kids, 2, cbuf[4]);
	for (const sift_grp *g; (g = sift_next(&op));) {
		uint32_t k = sift_grp_tids(g, NULL, tmp);
		check_tf(a, ma, 0, &ca, g, tmp);
		check_tf(b, mb, 1, &cb, g, tmp);
		if (ca.ub) {
			uint32_t m = 0;
			for (uint32_t i = ca.g.base; i < ca.g.base + ca.g.n; i++) m = m > a->tf[i] ? m : a->tf[i];
			CHECK(sift_grp_ub(&ca) == m, "group ub");
		}
		(void)k;
	}

	/* 3-way and nested trees */
	open_set(&ca, a, ma, 0), open_set(&cb, b, mb, 1), open_set(&cc, c, mc, 2);
	kids[0] = &ca, kids[1] = &cb, kids[2] = &cc;
	sift_cur_and(&op, kids, 3, cbuf[4]);
	set t1 = { tmp, NULL, ref_and(a, b, tmp) };
	rn = ref_and(&t1, c, ref);
	n = sift_tids(&op, outbuf);
	CHECK(same(outbuf, n, ref, rn), "and3 %zu vs %zu", n, rn);

	open_set(&ca, a, ma, 0), open_set(&cb, b, mb, 1), open_set(&cc, c, mc, 2);
	sift_cur_or(&op, kids, 3, cbuf[4]);
	t1.n = ref_or(a, b, tmp);
	rn = ref_or(&t1, c, ref);
	n = sift_tids(&op, outbuf);
	CHECK(same(outbuf, n, ref, rn), "or3 %zu vs %zu", n, rn);

	/* a AND (b OR c), then (that) AND NOT c */
	open_set(&ca, a, ma, 0), open_set(&cb, b, mb, 1), open_set(&cc, c, mc, 2);
	sift_cur *ok[2] = { &cb, &cc }, *ak[2] = { &ca, &op };
	sift_cur_or(&op, ok, 2, cbuf[4]);
	sift_cur_and(&op2, ak, 2, cbuf[5]);
	t1.n = ref_or(b, c, tmp);
	rn = ref_and(a, &t1, ref);
	n = sift_tids(&op2, outbuf);
	CHECK(same(outbuf, n, ref, rn), "nested %zu vs %zu", n, rn);

	/* seeks with random ascending targets */
	open_set(&ca, a, ma, 0);
	uint32_t target = 0;
	for (int s = 0; s < 20 && a->n; s++) {
		target += rndn(1 + (SIFT_GID(a->t[a->n - 1]) - target) / 4 + 1);
		const sift_grp *g = sift_seek(&ca, target);
		uint32_t i = 0;
		while (i < a->n && SIFT_GID(a->t[i]) < target) i++;
		if (i == a->n) {
			CHECK(!g, "seek past end");
			break;
		}
		CHECK(g && g->gid == SIFT_GID(a->t[i]), "seek gid");
		if (!g) break;
		uint32_t j = i;
		while (j < a->n && SIFT_GID(a->t[j]) == g->gid) j++;
		uint32_t k = sift_grp_tids(g, NULL, outbuf);
		CHECK(same(outbuf, k, a->t + i, j - i), "seek content");
		CHECK(sift_grp_count(g) == j - i, "grp count");
		/* page masks: count and tids restricted to random pages */
		uint64_t mask[4] = { rnd() & rnd(), rnd(), rnd() | rnd(), rnd() % 2 ? ~0ull : 0 };
		uint32_t rm = 0;
		for (uint32_t q = i; q < j; q++) {
			uint32_t pg = SIFT_BLK(a->t[q]) & 255;
			if (mask[pg >> 6] >> (pg & 63) & 1) tmp[rm++] = a->t[q];
		}
		CHECK(sift_grp_count_mask(g, mask) == rm, "count_mask");
		k = sift_grp_tids(g, mask, outbuf);
		CHECK(same(outbuf, k, tmp, rm), "tids mask");
		target = g->gid;
	}
}

/* chunk API, keylist and canonical mask kernels on the groups of a */
static void test_chunks(const set *a, sift_tid *tmp)
{
	static uint64_t om[SIFT_GROUP_PAGES * 8], om2[SIFT_GROUP_PAGES * 8], om3[SIFT_GROUP_PAGES * 8];
	static uint8_t chunk[4 + SIFT_GRP_MAXTIDS * 4], gb[SIFT_GRP_BYTES];
	size_t len = sift_keylist_encode(a->t, a->n, blob[3]);
	CHECK(len > 0 && sift_keylist_decode(blob[3], len, tmp) == a->n && !memcmp(tmp, a->t, a->n * 8ull), "keylist");
	for (uint32_t i = 0; i < a->n;) {
		uint32_t j = i, gid = SIFT_GID(a->t[i]), np = 0, last = UINT32_MAX;
		uint64_t pm[4] = { 0 };
		memset(om, 0, sizeof om);
		for (; j < a->n && SIFT_GID(a->t[j]) == gid; j++) {
			uint32_t pg = SIFT_BLK(a->t[j]) & 255, o = SIFT_OFF(a->t[j]) - 1;
			if (pg != last) np++, last = pg, pm[pg >> 6] |= 1ull << (pg & 63);
			om[(np - 1) * 8 + o / 64] |= 1ull << (o & 63);
		}
		int ct = (int)rndn(3);
		size_t cl = sift_chunk_encode(pm, om, ct, chunk);
		CHECK(cl == sift_chunk_encode(pm, om, ct, NULL) && (ct == SIFT_CT_ARRAY || cl <= sift_chunk_bound(np)), "chunk size");
		CHECK(sift_chunk_count(chunk) == j - i, "chunk count");
		uint64_t pm2[4], sel[4] = { rnd(), rnd() | rnd(), ~0ull, rnd() & rnd() };
		sift_chunk_pagemask(chunk, pm2);
		CHECK(!memcmp(pm, pm2, 32), "chunk pagemask");
		uint32_t ns = sift_chunk_offsets(chunk, sel, om2), k = 0, r = 0;
		int ok = 1;
		for (int w = 0; w < 4; w++)
			for (uint64_t m = pm[w]; m; m &= m - 1, r++)
				if (sel[w] & m & -m) ok &= !memcmp(om2 + k++ * 8, om + r * 8, 64);
		CHECK(ok && ns == k, "chunk offsets");
		sift_grp g;
		sift_chunk_grp(chunk, gid, &g, gb);
		uint32_t nt = sift_grp_tids(&g, NULL, tmp);
		CHECK(same(tmp, nt, a->t + i, j - i), "chunk grp");
		/* canonical mask kernels against plain loops */
		for (uint32_t q = 0; q < np * 8; q++) om2[q] = rnd() & rnd();
		uint64_t pc = 0;
		for (uint32_t q = 0; q < np * 8; q++) pc += (uint64_t)__builtin_popcountll(om[q] & om2[q]);
		sift_offmask_and(om3, om, om2, np);
		CHECK(sift_offmask_popcount(om3, np) == pc, "offmask and");
		sift_offmask_or(om3, om, om2, np);
		sift_offmask_andnot(om3, om3, om2, np);
		CHECK(sift_offmask_popcount(om3, np) == j - i - pc, "offmask or/andnot");
		i = j;
	}
}

/* top-k and facets on small block ranges against exhaustive float scoring */
static void test_topk(set *s, sift_tid *ref, sift_tid *tmp)
{
	static uint32_t rowstart[3000];
	static float knorm[3000 * 512];
	static uint8_t tkbuf[SIFT_TOPK_BUF];
	static sift_hit hits[64], rh[MAXN * 3];
	static uint8_t ub[3][MAXN];
	uint64_t base = rnd() % 2 ? 0 : 0xffffffffull - 2999;
	for (int i = 0; i < 3; i++) gen(&s[i], 1 + rndn(rnd() % 2 ? 300 : 20000), base, 1 + rndn(2999), 1 + rndn(291));
	for (uint32_t p = 0; p < 3000; p++) rowstart[p] = p * 512;
	for (uint32_t i = 0; i < 3000 * 512; i++) knorm[i] = 0.3f + (float)(rnd() % 1000) / 400.0f;
	sift_norms nm = { rowstart, knorm, (uint32_t)base };
	float w[3] = { 1.5f, 0.7f, 2.25f };
	/* exact bounds: ub = ceil(255 * tf / (tf + K)) */
	for (int i = 0; i < 3; i++)
		for (uint32_t j = 0; j < s[i].n; j++) {
			float tf = (float)sift_tf_quant(s[i].tf[j]), kn = knorm[rowstart[SIFT_BLK(s[i].t[j]) - base] + SIFT_OFF(s[i].t[j]) - 1];
			ub[i][j] = (uint8_t)ceilf(255.0f * (tf / (tf + kn)));
		}
	for (int conj = 0; conj < 2; conj++) {
		int nt = 2 + (int)rndn(2), filt = rnd() % 3 == 0;
		sift_cur c[4];
		sift_qterm qt[3];
		static const sift_enc_opts eo[] = { { 0, 0, 0, 1 }, { SIFT_GROUPED, 0, 0, 1 }, { SIFT_LIST, 0, 0, 1 } };
		for (int i = 0; i < nt; i++) {
			size_t blen = sift_encode(s[i].t, s[i].tf, ub[i], s[i].n, &eo[rndn(3)], blob[i]);
			sift_cur_blob(&c[i], blob[i], blen, cbuf[i]);
			qt[i] = (sift_qterm){ &c[i], w[i], 0 };
		}
		/* filter: the last set when unused as a term, else a random superset-ish set */
		set *fs = &s[2];
		if (filt) sift_cur_tids(&c[3], fs->t, fs->n, cbuf[3]);
		uint32_t k = 1 + rndn(40);
		sift_topk_q q = { qt, nt, conj, filt ? &c[3] : NULL, &nm, k, 0 };
		sift_topk_stats st = { 0, 0, 0 };
		uint32_t nh = sift_topk(&q, hits, tkbuf, &st);
		/* reference: candidate set, float scores in term order */
		set u = { ref, NULL, 0 };
		memcpy(ref, s[0].t, s[0].n * 8ull), u.n = s[0].n;
		for (int i = 1; i < nt; i++) {
			uint32_t m = conj ? ref_and(&u, &s[i], tmp) : ref_or(&u, &s[i], tmp);
			memcpy(ref, tmp, m * 8ull), u.n = m;
		}
		if (filt) u.n = ref_and(&u, fs, tmp), memcpy(ref, tmp, u.n * 8ull);
		for (uint32_t x = 0; x < u.n; x++) {
			float sc = 0;
			float kn = knorm[rowstart[SIFT_BLK(ref[x]) - base] + SIFT_OFF(ref[x]) - 1];
			for (int i = 0; i < nt; i++) {
				sift_tid *f = bsearch(&ref[x], s[i].t, s[i].n, 8, sift_cmp_u64);
				if (f) {
					float tf = (float)sift_tf_quant(s[i].tf[f - s[i].t]);
					sc += w[i] * (tf / (tf + kn));
				}
			}
			rh[x] = (sift_hit){ ref[x], sc };
		}
		/* sort reference best first: score desc, tid asc */
		for (uint32_t x = 1; x < u.n; x++) {
			sift_hit v = rh[x];
			uint32_t y = x;
			for (; y && (rh[y - 1].score < v.score || (rh[y - 1].score == v.score && rh[y - 1].tid > v.tid)); y--) rh[y] = rh[y - 1];
			rh[y] = v;
			if (x > 4000) break; /* bounded insertion sort; skip check below for huge sets */
		}
		if (u.n <= 4001) {
			uint32_t want = u.n < k ? u.n : k;
			int ok = nh == want;
			for (uint32_t x = 0; ok && x < want; x++) ok = hits[x].tid == rh[x].tid && hits[x].score == rh[x].score;
			CHECK(ok, "topk conj %d nt %d filt %d k %u: %u vs %u", conj, nt, filt, k, nh, want);
		}
	}
	/* facets: counts of s[0] AND each value set */
	sift_cur qc, vc[2], *vk[2] = { &vc[0], &vc[1] };
	uint64_t cnt[2] = { 0, 0 };
	size_t qlen = sift_encode(s[0].t, NULL, NULL, s[0].n, NULL, blob[0]);
	sift_cur_blob(&qc, blob[0], qlen, cbuf[0]);
	sift_cur_tids(&vc[0], s[1].t, s[1].n, cbuf[1]);
	sift_cur_tids(&vc[1], s[2].t, s[2].n, cbuf[2]);
	sift_facets(&qc, vk, 2, cnt);
	CHECK(cnt[0] == ref_and(&s[0], &s[1], tmp) && cnt[1] == ref_and(&s[0], &s[2], tmp), "facets");
}

int main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 3000;
	set s[3];
	for (int i = 0; i < 3; i++) s[i].t = malloc(MAXN * 8), s[i].tf = malloc(MAXN);
	for (int i = 0; i < 4; i++) blob[i] = malloc(sift_encode_bound(MAXN));
	outbuf = malloc(4 * MAXN * 8);
	sift_tid *ref = malloc(4 * MAXN * 8), *tmp = malloc(4 * MAXN * 8);
	sift_init();
	for (int isa = 0; isa < SIFT_NISA; isa++) {
		if (!sift_set_isa(isa)) {
			printf("%s: unsupported, skipped\n", sift_isa_name(isa));
			continue;
		}
		long f0 = fails, c0 = checks;
		for (int it = 0; it < iters; it++) {
			for (int i = 0; i < 3; i++) gen_shape(&s[i]);
			if (rnd() % 3 == 0) memcpy(s[1].t, s[0].t, s[0].n * 8), s[1].n = s[0].n; /* identical sets */
			test_ops(&s[0], &s[1], &s[2], ref, tmp);
			test_chunks(&s[0], tmp);
			if (it % 4 == 0) test_topk(s, ref, tmp);
		}
		printf("%s: %ld checks, %ld failures\n", sift_isa_name(isa), checks - c0, fails - f0);
	}
	for (int i = 0; i < 3; i++) free(s[i].t), free(s[i].tf);
	free(ref), free(tmp);
	return fails != 0;
}
