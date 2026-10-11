/* Randomized property tests of sift_bitmap against sorted-array reference implementations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/sift_bitmap.h"

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

static int cmp64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }

/* random set with a shape: base block, page span, offsets 1..maxoff, density */
static void gen(set *s, uint32_t n, uint64_t base, uint32_t span, uint32_t maxoff)
{
	for (uint32_t i = 0; i < n; i++) {
		uint64_t blk = base + rndn(span);
		if (blk > 0xffffffffull) blk = 0xffffffffull;
		uint32_t off = rnd() % 4 == 0 ? maxoff : 1 + rndn(maxoff);
		s->t[i] = SIFT_TID(blk, off);
	}
	qsort(s->t, n, 8, cmp64);
	uint32_t k = 0;
	for (uint32_t i = 0; i < n; i++)
		if (!k || s->t[i] != s->t[k - 1]) s->t[k++] = s->t[i];
	s->n = k;
	for (uint32_t i = 0; i < k; i++) s->tf[i] = (uint8_t)(rnd() % 8 ? 1 + rndn(3) : rndn(256));
}

static void gen_shape(set *s)
{
	static const uint32_t offs[] = { 1, 7, 8, 9, 16, 17, 32, 33, 64, 65, 100, 255, 256, 257, 291, 512 };
	static const uint64_t bases[] = { 0, 12345, 0xffffffffull - 3000, 0xfffffe00ull, 1u << 24 };
	uint32_t maxoff = offs[rndn(16)], span = 1 + rndn(rnd() % 2 ? 600 : 100000);
	uint32_t n = rnd() % 10 == 0 ? rndn(3) : rndn(rnd() % 3 ? 3000 : 60000);
	if (rnd() % 8 == 0) { /* full pages */
		uint64_t b0 = bases[rndn(5)];
		uint32_t k = 0, pages = 1 + rndn(300);
		for (uint32_t p = 0; p < pages && k < MAXN; p++)
			for (uint32_t o = 1; o <= maxoff && k < MAXN; o++) s->t[k] = SIFT_TID(b0 + p > 0xffffffffull ? 0xffffffffull : b0 + p, o), k++;
		qsort(s->t, k, 8, cmp64);
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
					      { SIFT_GROUPED, SIFT_CT_ARRAY, 0, 0 }, { SIFT_GROUPED, 0, 0, 1 } };
	if (mode == 5) {
		sift_cur_tids(c, s->t, s->n, cbuf[slot]);
		return;
	}
	size_t len = sift_encode(s->t, s->tf, s->n, &opts[mode], blob[slot]);
	CHECK(len <= sift_encode_bound(s->n), "bound %zu > %zu", len, sift_encode_bound(s->n));
	CHECK(len == sift_encode(s->t, s->tf, s->n, &opts[mode], NULL), "size query mismatch mode %d: %zu vs %zu n %u", mode, len, sift_encode(s->t, s->tf, s->n, &opts[mode], NULL), s->n);
	CHECK(sift_blob_n(blob[slot]) == s->n, "n");
	sift_cur_blob(c, blob[slot], cbuf[slot]);
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
		if (ca.maxtf) {
			uint32_t m = 0;
			for (uint32_t i = ca.g.base; i < ca.g.base + ca.g.n; i++) m = m > sift_tf_quant(a->tf[i]) ? m : sift_tf_quant(a->tf[i]);
			CHECK(sift_grp_maxtf(&ca) == m, "maxtf");
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
		}
		printf("%s: %ld checks, %ld failures\n", sift_isa_name(isa), checks - c0, fails - f0);
	}
	return fails != 0;
}
