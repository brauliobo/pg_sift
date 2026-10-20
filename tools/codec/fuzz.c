/*
 * Corrupted-blob fuzzer for the codec readers.
 *
 * Property: sift_blob_check() never reads outside its blob, and every blob it
 * accepts decodes (all cursors, operators, kernels, every ISA) without leaving
 * the blob or the cursor buffers, within a bounded number of steps, to strictly
 * increasing tids that match the stored count.  Run under ASan+UBSan:
 *
 *   fuzz [-s SEED] [-n ITERS] [-i ITER]    (-i replays one iteration)
 *
 * Every iteration derives its own PRNG state from (SEED, ITER).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../src/bm25/bitmap.h"

#define PAD 64
#define MAXN 20000

static uint64_t rng;
static uint64_t rnd(void)
{
	rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
	return rng;
}
static uint32_t rndn(uint32_t n) { return (uint32_t)(rnd() % n); }

static long cur_iter, cur_seed;

static void on_alarm(int sig)
{
	(void)sig;
	dprintf(2, "HANG: seed %ld iteration %ld (replay: fuzz -s %ld -i %ld)\n", cur_seed, cur_iter, cur_seed, cur_iter);
	_exit(3);
}

static int cmp64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }

static uint32_t gen_set(sift_tid *t, uint8_t *tf)
{
	static const uint32_t offs[] = { 1, 7, 8, 9, 17, 33, 65, 100, 255, 257, 291, 450, 512 };
	uint32_t maxoff = offs[rndn(13)], span = 1 + rndn(rnd() % 2 ? 700 : 200000), n = rnd() % 8 == 0 ? rndn(4) : rndn(rnd() % 3 ? 400 : MAXN);
	uint64_t base = rnd() % 4 == 0 ? 0xffffffffull - 3000 : rndn(5000);
	for (uint32_t i = 0; i < n; i++) {
		uint64_t blk = base + rndn(span);
		t[i] = SIFT_TID(blk > 0xffffffffull ? 0xffffffffull : blk, 1 + rndn(maxoff));
	}
	qsort(t, n, 8, cmp64);
	uint32_t k = 0;
	for (uint32_t i = 0; i < n; i++)
		if (!k || t[i] != t[k - 1]) t[k++] = t[i];
	for (uint32_t i = 0; i < k; i++) tf[i] = (uint8_t)(1 + rndn(rnd() % 8 ? 3 : 255));
	return k;
}

/* an encoded valid blob in a fresh allocation of exactly *len bytes */
static uint8_t *gen_blob(size_t *len, uint32_t *n)
{
	static sift_tid t[MAXN];
	static uint8_t tf[MAXN], ub[MAXN];
	*n = gen_set(t, tf);
	if (!*n) t[0] = SIFT_TID(1, 1), *n = 1;
	for (uint32_t i = 0; i < *n; i++) ub[i] = (uint8_t)rndn(256);
	sift_enc_opts o = { rnd() % 3 ? (int)(1 + rndn(2)) : 0, (int)rndn(4), 0, rnd() % 2 };
	*len = sift_encode(t, tf, ub, *n, &o, NULL);
	uint8_t *b = malloc(*len);
	sift_encode(t, tf, ub, *n, &o, b);
	return b;
}

static const uint32_t magic[] = { 0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0xffff, 0x10000, 0x7fffffff, 0x80000000u, 0xffffffffu, 0xfffffff0u };

static void mutate(uint8_t *b, size_t *len, const uint8_t *other, size_t olen)
{
	for (int rounds = 1 + rndn(4); rounds--;) {
		size_t i = rndn((uint32_t)*len), j;
		switch (rndn(9)) {
		case 0: b[i] ^= (uint8_t)(1u << rndn(8)); break;
		case 1: b[i] = (uint8_t)rnd(); break;
		case 2: b[i] = rnd() % 2 ? 0 : 0xff; break;
		case 3: if (i + 4 <= *len) { uint32_t v = magic[rndn(13)] + (uint32_t)rndn(3) - 1; memcpy(b + i, &v, 4); } break;
		case 4: if (*len > 8) *len -= 1 + rndn(rnd() % 2 ? 4 : (uint32_t)(*len / 2)); break; /* truncated */
		case 5: j = rndn((uint32_t)olen); memcpy(b + i, other + j, (*len - i < olen - j ? *len - i : olen - j) < 16 ? (*len - i < olen - j ? *len - i : olen - j) : 16); break;
		case 6: for (j = 0; j < 8 && i + j < *len; j++) b[i + j] = (uint8_t)rnd(); break;
		case 7: if (i + 8 <= *len) { uint64_t v = rnd() % 2 ? ~0ull : 0; memcpy(b + i, &v, 8); } break;
		case 8: b[i % 16] ^= (uint8_t)(1u << rndn(8)); break; /* header */
		}
	}
}

static uint8_t *aligned(size_t n)
{
	uint8_t *p = aligned_alloc(64, (n + 63) & ~(size_t)63);
	memset(p, 0, (n + 63) & ~(size_t)63);
	return p;
}

static long bad, accepted, rejected;
#define BAD(...) do { bad++; printf("FAIL seed %ld iter %ld: ", cur_seed, cur_iter); printf(__VA_ARGS__); printf("\n"); } while (0)

/* drain a cursor: strictly increasing tids, groups consistent, returns the number of tids */
static size_t drain(sift_cur *c, sift_tid *out, size_t cap, uint64_t *cnt)
{
	size_t n = 0;
	const sift_grp *g;
	uint32_t last_gid = 0;
	int first = 1;
	*cnt = 0;
	while ((g = sift_next(c))) {
		if (!first && g->gid <= last_gid) BAD("group ids not increasing: %u after %u", g->gid, last_gid);
		first = 0, last_gid = g->gid;
		*cnt += sift_grp_count(g);
		uint64_t k = sift_grp_count_mask(g, (const uint64_t[4]){ ~0ull, ~0ull, ~0ull, ~0ull });
		if (n + k > cap) { BAD("tids overflow"); break; }
		if (k != sift_grp_tids(g, NULL, out + n)) BAD("count_mask != tids written");
		n += k;
	}
	return n;
}

static void exercise(const uint8_t *blob, size_t len, uint32_t want_n, const uint8_t *oblob, size_t olen, int have_o)
{
	static sift_tid out[3][SIFT_GRP_MAXTIDS * 4 + 8];
	size_t tot[SIFT_NISA];
	static sift_tid *ref;
	(void)len;
	if (!ref) ref = malloc(sizeof(out[0]));
	for (int isa = 0; isa < SIFT_NISA; isa++) {
		if (!sift_set_isa(isa)) continue;
		uint8_t *buf[6];
		for (int i = 0; i < 6; i++) buf[i] = aligned(SIFT_CUR_BUF);
		sift_cur c, o, a, b2;
		uint64_t cnt;
		sift_cur_blob(&c, blob, len, buf[0]);
		size_t cap = sizeof(out[0]) / 8;
		size_t n = drain(&c, out[0], cap, &cnt);
		tot[isa] = n;
		if (cnt != n) BAD("isa %d: group counts %llu != tids %zu", isa, (unsigned long long)cnt, n);
		if (n != want_n) BAD("isa %d: decoded %zu tids, header says %u", isa, n, want_n);
		for (size_t i = 1; i < n; i++)
			if (out[0][i] <= out[0][i - 1]) { BAD("isa %d: tids not strictly increasing at %zu", isa, i); break; }
		for (size_t i = 0; i < n; i++)
			if (SIFT_OFF(out[0][i]) < 1 || SIFT_OFF(out[0][i]) > SIFT_MAX_OFF) { BAD("isa %d: offset %u out of range", isa, SIFT_OFF(out[0][i])); break; }
		if (isa == 0) memcpy(ref, out[0], n * 8);
		else if (memcmp(ref, out[0], n * 8) || tot[isa] != tot[0]) BAD("isa %d decodes differently from scalar", isa);
		/* seeks and counts */
		sift_cur_blob(&c, blob, len, buf[0]);
		for (int s = 0; s < 6; s++) {
			uint32_t target = n ? SIFT_GID(ref[rndn((uint32_t)n)]) : 0;
			const sift_grp *g = sift_seek(&c, target + rndn(2));
			if (g && g->gid < target) BAD("seek(%u) returned group %u (kind %d, n %zu, mutated blob)", target, g->gid, sift_blob_kind(blob), n);
		}
		sift_cur_blob(&c, blob, len, buf[0]);
		if (sift_count(&c) != n) BAD("isa %d: sift_count != tids", isa);
		/* operators against the other blob (when it passed the check) and against a raw tid cursor */
		if (have_o) {
			sift_cur *kid[2] = { &a, &b2 };
			sift_cur_blob(&a, blob, len, buf[0]);
			sift_cur_blob(&b2, oblob, olen, buf[1]);
			sift_cur_and(&o, kid, 2, buf[2]);
			uint64_t c1;
			size_t na = drain(&o, out[1], cap, &c1);
			sift_cur_blob(&a, blob, len, buf[0]);
			sift_cur_blob(&b2, oblob, olen, buf[1]);
			sift_cur_and(&o, kid, 2, buf[2]);
			if (sift_count(&o) != na) BAD("isa %d: AND count != tids", isa);
			sift_cur_blob(&a, blob, len, buf[0]);
			sift_cur_blob(&b2, oblob, olen, buf[1]);
			sift_cur_or(&o, kid, 2, buf[2]);
			size_t no = drain(&o, out[1], cap, &c1);
			sift_cur_blob(&a, blob, len, buf[0]);
			sift_cur_blob(&b2, oblob, olen, buf[1]);
			sift_cur_or(&o, kid, 2, buf[2]);
			if (sift_count(&o) != no) BAD("isa %d: OR count != tids", isa);
			sift_cur_blob(&a, blob, len, buf[0]);
			sift_cur_blob(&b2, oblob, olen, buf[1]);
			sift_cur_andnot(&o, kid, buf[2]);
			size_t nn = drain(&o, out[1], cap, &c1);
			sift_cur_blob(&a, blob, len, buf[0]);
			sift_cur_blob(&b2, oblob, olen, buf[1]);
			sift_cur_andnot(&o, kid, buf[2]);
			if (sift_count(&o) != nn) BAD("isa %d: ANDNOT count != tids", isa);
			if (na > no || nn > n) BAD("isa %d: AND/OR/ANDNOT sizes inconsistent", isa);
		}
		/* ranks of a group within itself */
		sift_cur_blob(&c, blob, len, buf[0]);
		const sift_grp *g = sift_next(&c);
		if (g) {
			uint32_t *rank = malloc(SIFT_GRP_MAXTIDS * 4);
			uint32_t k = sift_grp_ranks(g, g, rank);
			for (uint32_t i = 0; i < k; i++)
				if (rank[i] != i) { BAD("isa %d: self rank %u at %u", isa, rank[i], i); break; }
			free(rank);
		}
		for (int i = 0; i < 6; i++) free(buf[i]);
	}
	sift_set_isa(SIFT_ISA_SCALAR);
}

static void one(long seed, long iter)
{
	static const char *how[] = { "valid", "mutated" };
	size_t len, olen;
	uint32_t n, on;
	cur_seed = seed, cur_iter = iter;
	rng = ((uint64_t)seed * 0x9e3779b97f4a7c15ull) ^ ((uint64_t)(iter + 1) * 0xbf58476d1ce4e5b9ull) | 1;
	for (int i = 0; i < 4; i++) rnd();
	uint8_t *g = gen_blob(&len, &n), *og = gen_blob(&olen, &on);
	uint8_t *m = malloc(len + PAD), *om = malloc(olen + PAD);
	memcpy(m, g, len), memset(m + len, 0, PAD);
	memcpy(om, og, olen), memset(om + olen, 0, PAD);
	size_t mlen = len, omlen = olen;
	int mutated = rnd() % 8 != 0;
	if (mutated) mutate(m, &mlen, og, olen);
	if (rnd() % 3 == 0) mutate(om, &omlen, g, len);
	(void)how;
	const char *e = sift_blob_check(m, mlen), *oe = sift_blob_check(om, omlen);
	if (!mutated && e) BAD("valid blob rejected: %s", e);
	if (e) rejected++;
	else {
		accepted++;
		/* kind and header count are trusted once the check passes; the posting layer already compares n with the dictionary */
		exercise(m, mlen, sift_blob_n(m), om, omlen, !oe);
	}
	free(g), free(og), free(m), free(om);
}

int main(int argc, char **argv)
{
	long seed = 1, iters = 20000, only = -1;
	for (int i = 1; i + 1 < argc; i += 2)
		if (!strcmp(argv[i], "-s")) seed = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-n")) iters = atol(argv[i + 1]);
		else if (!strcmp(argv[i], "-i")) only = atol(argv[i + 1]);
	signal(SIGALRM, on_alarm);
	sift_init();
	for (long it = only >= 0 ? only : 0; it < (only >= 0 ? only + 1 : iters); it++) {
		alarm(20);
		one(seed, it);
		if (bad > 20) break;
	}
	alarm(0);
	printf("fuzz: seed %ld, %ld accepted, %ld rejected, %ld failures\n", seed, accepted, rejected, bad);
	return bad != 0;
}
