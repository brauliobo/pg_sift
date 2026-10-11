/*
 * bench: codec sizes and operation throughput on the real corpus.
 *   bench dir tpp size        bits/posting by df class and representation
 *   bench dir tpp ops [secs]  op throughput per ISA and representation
 * tpp = 0 uses the real ctids, tpp > 0 remaps docs to tpp tuples per page.
 */
#include <math.h>
#include <time.h>
#include "data.h"
#include "../../src/sift_bitmap.h"

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint64_t rng = 0x2545f4914f6cdd1dull;
static uint64_t rnd(void)
{
	rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
	return rng;
}

static corpus C;
static sift_tid *T;   /* scratch tids of one term */

/* ---------------- sizes ---------------- */

enum { R_AUTO, R_LIST, R_GROUPED, R_BITMAP, R_ARRAY, R_TIN512, R_EF, R_TFNIB, R_TFL1, NREP };
static const char *rep_name[NREP] = { "auto", "list", "grouped", "bitmap", "array", "tin512", "ef-docid", "tf-nib", "tf-L1" };

static void sizes(void)
{
	static const sift_enc_opts eo[] = { { 0, 0, 0, 0 }, { SIFT_LIST, 0, 0, 0 }, { SIFT_GROUPED, 0, 0, 0 }, { SIFT_GROUPED, SIFT_CT_BITMAP, 0, 0 }, { SIFT_GROUPED, SIFT_CT_ARRAY, 0, 0 } };
	double bytes[NCLASS][NREP] = { { 0 } }, grouped_terms[NCLASS] = { 0 };
	uint64_t post[NCLASS] = { 0 }, terms[NCLASS] = { 0 }, gbitmap = 0, garray = 0, wh[65] = { 0 };
	uint8_t *blob = malloc(sift_encode_bound((uint32_t)C.nd));
	for (uint32_t t = 0; t < C.nt; t++) {
		uint32_t n = C.df[t];
		int k = df_class(n);
		corpus_tids(&C, t, T);
		terms[k]++, post[k] += n;
		bytes[k][R_EF] += n * (2 + ceil(log2((double)C.nd / n))) / 8;
		if (n == 1) continue;  /* singletons live inline in the dictionary */
		for (int r = R_AUTO; r <= R_ARRAY; r++) bytes[k][r] += (double)sift_encode(T, NULL, NULL, n, &eo[r], NULL);
		/* TIN-style fixed 512-bit offset masks: 32 B page mask per chunk + 64 B per page */
		uint32_t pages = 0, groups = 0, tf1 = 0, esc = 0, l1 = 0;
		for (uint32_t i = 0; i < n; i++) {
			pages += !i || SIFT_BLK(T[i]) != SIFT_BLK(T[i - 1]);
			groups += !i || SIFT_GID(T[i]) != SIFT_GID(T[i - 1]);
		}
		bytes[k][R_TIN512] += groups * 44.0 + pages * 64.0;
		/* tf side streams: nibble per posting; L1 bit + nibble + escape per chunk, empty if all ones */
		bytes[k][R_TFNIB] += (n + 1) / 2;
		const uint8_t *tf = C.tf + C.start[t];
		for (uint32_t i = 0; i < n;) {
			uint32_t j = i, ones = 1;
			tf1 = esc = 0;
			for (; j < n && SIFT_GID(T[j]) == SIFT_GID(T[i]); j++) {
				if (tf[j] > 1) ones = 0, tf1++;
				if (tf[j] > 16) esc++;
			}
			if (!ones) l1 += (j - i + 7) / 8 + (tf1 + 1) / 2 + esc;
			i = j;
		}
		bytes[k][R_TFL1] += l1;
		size_t len = sift_encode(T, NULL, NULL, n, &eo[R_AUTO], blob);
		(void)len;
		if (sift_blob_kind(blob) == SIFT_GROUPED) {
			uint32_t ng;
			memcpy(&ng, blob + 8, 4);
			grouped_terms[k]++;
			for (uint32_t g = 0; g < ng; g++) {
				uint32_t key;
				memcpy(&key, blob + 12 + 4 * g, 4);
				if (key & 0x80) garray++; else gbitmap++;
				wh[key & 0x7f]++;
			}
		}
	}
	printf("[%s] bits/posting by df class (singletons inline: excluded from codec columns)\n%-9s %8s %10s", C.scen, "df", "terms", "postings");
	for (int r = 0; r < NREP; r++) printf(" %8s", rep_name[r]);
	printf(" %8s\n", "grouped%");
	double tot[NREP] = { 0 };
	uint64_t ptot = 0, p1 = 0;
	for (int k = 0; k < NCLASS; k++) {
		printf("%-9s %8lu %10lu", class_name[k], terms[k], post[k]);
		uint64_t pk = k ? post[k] : 0;
		for (int r = 0; r < NREP; r++) {
			printf(" %8.2f", pk ? bytes[k][r] * 8 / pk : (r == R_EF ? bytes[k][r] * 8 / post[k] : 0));
			tot[r] += bytes[k][r];
		}
		printf(" %7.1f%%\n", terms[k] ? 100.0 * grouped_terms[k] / terms[k] : 0);
		ptot += post[k];
		if (!k) p1 = post[k];
	}
	printf("%-9s %8u %10lu", "all>1", C.nt, ptot - p1);
	for (int r = 0; r < NREP; r++) printf(" %8.2f", tot[r] * 8 / (r == R_EF ? ptot : ptot - p1));
	printf("\ntotal MB: auto %.1f list %.1f bitmap %.1f tin512 %.1f ef-docid %.1f; singletons inline %.1f MB (6 B each)\n",
	       tot[R_AUTO] / 1e6, tot[R_LIST] / 1e6, tot[R_BITMAP] / 1e6, tot[R_TIN512] / 1e6, tot[R_EF] / 1e6, p1 * 6 / 1e6);
	printf("auto GROUPED containers: bitmap %lu array %lu; w histogram:", gbitmap, garray);
	for (int w = 0; w <= 64; w++)
		if (wh[w]) printf(" w%d=%.1f%%", w, 100.0 * wh[w] / (gbitmap + garray));
	printf("\n");
	free(blob);
}

static uint32_t nbits(uint64_t x) { return x ? 64 - (uint32_t)__builtin_clzll(x) : 0; }
static uint32_t varint_len(uint64_t x) { return 1 + nbits(x >> 7 ? x : 0) / 7; }

/* candidate key-list encodings over the combined key (blk - blk0) << ob | (off - 1); sizes in bits, 1 B header */
static void list_alts(void)
{
	enum { A_EF, A_FOR, A_PFOR, A_EFDOC, NA };
	static const char *an[NA] = { "ef-ctid", "for128", "pfor128", "ef-docid" };
	double bits[NCLASS][NA] = { { 0 } };
	uint64_t post[NCLASS] = { 0 };
	uint64_t *key = malloc(C.nd * 8);
	for (uint32_t t = 0; t < C.nt; t++) {
		uint32_t n = C.df[t], maxo = 0;
		int k = df_class(n);
		if (n < 2) continue;
		corpus_tids(&C, t, T);
		post[k] += n;
		for (uint32_t i = 0; i < n; i++) maxo = SIFT_OFF(T[i]) > maxo ? SIFT_OFF(T[i]) : maxo;
		uint32_t ob = nbits(maxo - 1), blk0 = SIFT_BLK(T[0]);
		for (uint32_t i = 0; i < n; i++) key[i] = (uint64_t)(SIFT_BLK(T[i]) - blk0) << ob | (SIFT_OFF(T[i]) - 1);
		double hdr = 8.0 * (1 + varint_len(blk0) + 1);
		uint64_t u = key[n - 1] + 1;
		uint32_t l = u > n ? nbits(u / n) - 1 : 0;
		bits[k][A_EF] += hdr + (double)n * (l + 1) + (double)(u >> l);
		for (uint32_t i = 0; i < n; i += 128) {
			uint32_t cnt = n - i < 128 ? n - i : 128, b = 0, best = UINT32_MAX;
			uint64_t d[128];
			for (uint32_t j = 0; j < cnt; j++) d[j] = key[i + j] - (i + j ? key[i + j - 1] : 0);
			for (uint32_t j = 0; j < cnt; j++) b = nbits(d[j]) > b ? nbits(d[j]) : b;
			bits[k][A_FOR] += 8 + (double)cnt * b;
			for (uint32_t pb = 0; pb <= b; pb++) {
				uint32_t ne = 0, ok = 1;
				for (uint32_t j = 0; j < cnt; j++)
					if (nbits(d[j]) > pb) ne++, ok &= nbits(d[j]) <= pb + 8;
				if (ok && cnt * pb + 16 * ne < best) best = cnt * pb + 16 * ne;
			}
			bits[k][A_PFOR] += 16 + best;
		}
		bits[k][A_FOR] += hdr, bits[k][A_PFOR] += hdr;
		uint32_t ld = C.nd > n ? nbits(C.nd / n) - 1 : 0;
		bits[k][A_EFDOC] += (double)n * (ld + 1) + (double)(C.nd >> ld);
	}
	printf("[%s] key-list alternatives, bits/posting (term header 1 B + varint base)\n%-9s", C.scen, "df");
	for (int a = 0; a < NA; a++) printf(" %9s", an[a]);
	printf("\n");
	double tot[NA] = { 0 };
	uint64_t pt = 0;
	for (int k = 1; k < NCLASS; k++) {
		printf("%-9s", class_name[k]);
		for (int a = 0; a < NA; a++) printf(" %9.2f", bits[k][a] / post[k]), tot[a] += bits[k][a];
		printf("\n");
		pt += post[k];
	}
	printf("%-9s", "all>1");
	for (int a = 0; a < NA; a++) printf(" %9.2f", tot[a] / pt);
	printf("\n");
	free(key);
}

/* ---------------- operations ---------------- */

enum { Q_HH, Q_HM, Q_MM, Q_HR, Q_H3, NQ };
static const char *q_name[NQ] = { "hi&hi", "hi&mid", "mid&mid", "hi&rare", "hi&hi&mid" };
#define NPAIR 64

enum { P_AUTO, P_GROUPED, P_BITMAP, P_LIST, P_ARRAY64, NPR };
static const char *p_name[NPR] = { "auto", "grouped", "bitmap", "list", "u64-array" };

typedef struct { uint32_t t[3]; int k; } query;
static query Q[NQ][NPAIR];
static uint8_t *blobs[NPR][NQ][NPAIR][3];
static sift_tid *arrays[NQ][NPAIR][3];
static sift_tid *OUT;
static uint8_t cb[6][SIFT_CUR_BUF];

static uint32_t pick(uint32_t lo, uint32_t hi)
{
	for (;;) {
		uint32_t t = (uint32_t)(rnd() % C.nt);
		if (C.df[t] >= lo && C.df[t] < hi) return t;
	}
}

static void make_queries(void)
{
	const uint32_t H = 65536, M = 1024, R = 16;
	for (int p = 0; p < NPAIR; p++) {
		Q[Q_HH][p] = (query){ { pick(H, ~0u), pick(H, ~0u) }, 2 };
		Q[Q_HM][p] = (query){ { pick(H, ~0u), pick(M, H) }, 2 };
		Q[Q_MM][p] = (query){ { pick(M, H), pick(M, H) }, 2 };
		Q[Q_HR][p] = (query){ { pick(H, ~0u), pick(R, M) }, 2 };
		Q[Q_H3][p] = (query){ { pick(H, ~0u), pick(H, ~0u), pick(M, H) }, 3 };
	}
	static const sift_enc_opts eo[] = { { 0, 0, 0, 0 }, { SIFT_GROUPED, 0, 0, 0 }, { SIFT_GROUPED, SIFT_CT_BITMAP, 0, 0 }, { SIFT_LIST, 0, 0, 0 } };
	for (int q = 0; q < NQ; q++)
		for (int p = 0; p < NPAIR; p++)
			for (int i = 0; i < Q[q][p].k; i++) {
				uint32_t t = Q[q][p].t[i], n = C.df[t];
				arrays[q][p][i] = malloc(n * 8);
				corpus_tids(&C, t, arrays[q][p][i]);
				for (int r = 0; r < P_ARRAY64; r++) {
					size_t len = sift_encode(arrays[q][p][i], NULL, NULL, n, &eo[r], NULL);
					blobs[r][q][p][i] = aligned_alloc(64, (len + 63) & ~(size_t)63);
					sift_encode(arrays[q][p][i], NULL, NULL, n, &eo[r], blobs[r][q][p][i]);
				}
			}
}

/* baseline: uncompressed sorted u64 arrays, galloping intersection / merge */
static size_t arr_and(const sift_tid *a, size_t na, const sift_tid *b, size_t nb, sift_tid *o)
{
	if (na > nb) return arr_and(b, nb, a, na, o);
	size_t n = 0, j = 0;
	for (size_t i = 0; i < na && j < nb; i++) {
		size_t step = 1, lo = j, hi = j;
		while (hi < nb && b[hi] < a[i]) lo = hi + 1, hi += step, step <<= 1;
		if (hi > nb) hi = nb;
		while (lo < hi) {
			size_t mid = (lo + hi) / 2;
			if (b[mid] < a[i]) lo = mid + 1; else hi = mid;
		}
		j = lo;
		if (j < nb && b[j] == a[i]) o[n++] = a[i];
	}
	return n;
}

static size_t arr_or_count(const sift_tid *a, size_t na, const sift_tid *b, size_t nb)
{
	size_t i = 0, j = 0, n = 0;
	while (i < na && j < nb) {
		uint64_t x = a[i], y = b[j];
		i += x <= y, j += y <= x, n++;
	}
	return n + (na - i) + (nb - j);
}

enum { O_DECODE, O_AND, O_ANDCNT, O_OR, O_ORCNT, O_ANDNOTCNT, NO };
static const char *o_name[NO] = { "decode", "and", "and-count", "or", "or-count", "andnot-cnt" };

static volatile uint64_t sink;

static uint64_t run_query(int r, int q, int p, int op)
{
	const query *qq = &Q[q][p];
	if (r == P_ARRAY64) {
		sift_tid **a = arrays[q][p];
		const uint32_t *t = qq->t;
		switch (op) {
		case O_DECODE: { uint64_t s = 0; for (uint32_t i = 0; i < C.df[t[0]]; i++) s += a[0][i]; return s; }
		case O_AND: case O_ANDCNT: {
			size_t n = arr_and(a[0], C.df[t[0]], a[1], C.df[t[1]], OUT);
			if (qq->k == 3) n = arr_and(OUT, n, a[2], C.df[t[2]], OUT);
			return n;
		}
		case O_OR: case O_ORCNT: return arr_or_count(a[0], C.df[t[0]], a[1], C.df[t[1]]);
		default: return C.df[t[0]] - arr_and(a[0], C.df[t[0]], a[1], C.df[t[1]], OUT);
		}
	}
	sift_cur c[3], op_c, *kids[3] = { &c[0], &c[1], &c[2] };
	int k = op == O_DECODE ? 1 : op == O_AND || op == O_ANDCNT ? qq->k : 2;
	for (int i = 0; i < k; i++) sift_cur_blob(&c[i], blobs[r][q][p][i], cb[i]);
	if (op == O_DECODE) return sift_tids(&c[0], OUT);
	if (op == O_AND || op == O_ANDCNT) sift_cur_and(&op_c, kids, k, cb[4]);
	else if (op == O_OR || op == O_ORCNT) sift_cur_or(&op_c, kids, 2, cb[4]);
	else sift_cur_andnot(&op_c, kids, cb[4]);
	return op == O_AND || op == O_OR ? sift_tids(&op_c, OUT) : sift_count(&op_c);
}

static uint64_t in_postings(int q, int op)
{
	uint64_t s = 0;
	for (int p = 0; p < NPAIR; p++) {
		int k = op == O_DECODE ? 1 : op == O_AND || op == O_ANDCNT ? Q[q][p].k : 2;
		for (int i = 0; i < k; i++) s += C.df[Q[q][p].t[i]];
	}
	return s;
}

static void ops(double secs)
{
	OUT = malloc(C.nd * 2 * 8);
	make_queries();
	printf("[%s] input Mpostings/s (sum of operand df / time); %d queries per class\n", C.scen, NPAIR);
	printf("%-10s %-11s %-9s", "op", "class", "isa");
	for (int r = 0; r < NPR; r++) printf(" %10s", p_name[r]);
	printf("\n");
	for (int op = 0; op < NO; op++)
		for (int q = 0; q < NQ; q++) {
			if (op == O_DECODE && q != Q_HH) continue;
			if ((op == O_OR || op == O_ORCNT || op == O_ANDNOTCNT) && q == Q_H3) continue;
			uint64_t ref = 0;
			for (int isa = 0; isa < SIFT_NISA; isa++) {
				if (!sift_set_isa(isa)) continue;
				printf("%-10s %-11s %-9s", o_name[op], q_name[q], sift_isa_name(isa));
				for (int r = 0; r < NPR; r++) {
					if (r == P_ARRAY64 && isa) { printf(" %10s", "-"); continue; }
					uint64_t res = 0, iters = 0;
					double t0 = now(), t1;
					do {
						res = 0;
						for (int p = 0; p < NPAIR; p++) res += run_query(r, q, p, op);
						iters++;
					} while ((t1 = now()) - t0 < secs);
					if (op != O_DECODE && r != P_ARRAY64) {
						if (!ref) ref = res;
						else if (res != ref) printf("\nMISMATCH %s %s: %lu vs %lu\n", o_name[op], p_name[r], res, ref);
					}
					printf(" %10.1f", in_postings(q, op) * iters / (t1 - t0) / 1e6);
					fflush(stdout);
				}
				printf("\n");
			}
		}
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: bench dir tpp size|ops [secs]\n");
		return 1;
	}
	corpus_load(&C, argv[1], (unsigned)atoi(argv[2]));
	T = malloc(C.nd * 8);
	sift_init();
	if (!strcmp(argv[3], "size")) sizes();
	if (!strcmp(argv[3], "lists")) list_alts();
	if (!strcmp(argv[3], "ops")) ops(argc > 4 ? atof(argv[4]) : 0.2);
	return 0;
}
