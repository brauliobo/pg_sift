/*
 * bench: codec sizes and operation throughput on the real corpus.
 *   bench dir tpp size        bits/posting by df class and representation
 *   bench dir tpp ops [secs]  op throughput per ISA and representation
 * tpp = 0 uses the real ctids, tpp > 0 remaps docs to tpp tuples per page.
 */
#include <math.h>
#include <time.h>
#include "data.h"
#include "../../src/bm25/bitmap.h"
#include "../../src/core/cmp.h"
#include "baselines.h"
#ifdef HAVE_ROARING
#include "roaring.h"
#endif

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

enum { R_AUTO, R_LIST, R_GROUPED, R_BITMAP, R_ARRAY, R_TIN512, R_EF, R_TFNIB, R_TFL1, R_P1K, R_P8K, R_P64K, R_BND, NREP };
static const char *rep_name[NREP] = { "auto", "list", "grouped", "bitmap", "array", "tin512", "ef-docid", "tf-nib", "tf-L1",
				      "pol1K", "pol8K", "pol64K", "bnd8K" };

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
		double sz[5];
		for (int r = R_AUTO; r <= R_ARRAY; r++) bytes[k][r] += sz[r] = (double)sift_encode(T, NULL, NULL, n, &eo[r], NULL);
		/* policies: GROUPED when df >= threshold, else LIST; bounds: 9 B per group (ub + 16 block nibbles) */
		bytes[k][R_P1K] += n >= 1024 ? sz[R_GROUPED] : sz[R_LIST];
		bytes[k][R_P8K] += n >= 8192 ? sz[R_GROUPED] : sz[R_LIST];
		bytes[k][R_P64K] += n >= 65536 ? sz[R_GROUPED] : sz[R_LIST];
		/* Fixed 512-bit offset masks: 32 B page mask per chunk + 64 B per page */
		uint32_t pages = 0, groups = 0, tf1 = 0, esc = 0, l1 = 0;
		for (uint32_t i = 0; i < n; i++) {
			pages += !i || SIFT_BLK(T[i]) != SIFT_BLK(T[i - 1]);
			groups += !i || SIFT_GID(T[i]) != SIFT_GID(T[i - 1]);
		}
		bytes[k][R_TIN512] += groups * 44.0 + pages * 64.0;
		bytes[k][R_BND] += n >= 8192 ? groups * 9.0 : 1;
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
	printf("\ntotal MB: auto %.1f list %.1f bitmap %.1f tin512 %.1f ef-docid %.1f pol1K %.1f pol8K %.1f pol64K %.1f; singletons inline %.1f MB (6 B each)\n",
	       tot[R_AUTO] / 1e6, tot[R_LIST] / 1e6, tot[R_BITMAP] / 1e6, tot[R_TIN512] / 1e6, tot[R_EF] / 1e6, tot[R_P1K] / 1e6,
	       tot[R_P8K] / 1e6, tot[R_P64K] / 1e6, p1 * 6 / 1e6);
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

enum { Q_HH, Q_HM, Q_MM, Q_HR, Q_H3, NQ };
static const char *q_name[NQ] = { "hi&hi", "hi&mid", "mid&mid", "hi&rare", "hi&hi&mid" };
#define NPAIR 64

enum { P_AUTO, P_GROUPED, P_BITMAP, P_LIST, NSIFT, P_U64 = NSIFT, P_FOR, P_RDOC, P_RCTID, NPR };
static const char *p_name[NPR] = { "auto", "grouped", "bitmap", "list", "u64", "for-doc", "roar-doc", "roar-ctid" };

typedef struct { uint32_t t[3]; int k; } query;
static query Q[NQ][NPAIR];
static uint8_t *blobs[NSIFT][NQ][NPAIR][3];

/* the benchmark keeps its blobs without their lengths: the encoder's bound for the postings they hold */
static void cur_blob(sift_cur *c, const uint8_t *blob, uint8_t *buf) { sift_cur_blob(c, blob, sift_encode_bound(sift_blob_n(blob)), buf); }
static sift_tid *arrays[NQ][NPAIR][3];
static forlist fors[NQ][NPAIR][3];
static sift_tid *OUT;
static uint32_t *OUT32;
static uint8_t cb[6][SIFT_CUR_BUF];
#ifdef HAVE_ROARING
static roaring_bitmap_t *rdoc[NQ][NPAIR][3], *rctid[NQ][NPAIR][3];
#endif

static uint32_t pick(uint32_t lo, uint32_t hi)
{
	for (;;) {
		uint32_t t = (uint32_t)(rnd() % C.nt);
		if (C.df[t] >= lo && C.df[t] < hi) return t;
	}
}

static inline uint32_t ctid_key(sift_tid t) { return SIFT_BLK(t) * 512 + SIFT_OFF(t) - 1; }

static void make_queries(void)
{
	const uint32_t H = 65536, M = 1024, R = 16;
	for (int p = 0; p < NPAIR; p++) {
		Q[Q_HH][p] = (query){ { pick(H, ~0u), pick(H, ~0u), 0 }, 2 };
		Q[Q_HM][p] = (query){ { pick(H, ~0u), pick(M, H), 0 }, 2 };
		Q[Q_MM][p] = (query){ { pick(M, H), pick(M, H), 0 }, 2 };
		Q[Q_HR][p] = (query){ { pick(H, ~0u), pick(R, M), 0 }, 2 };
		Q[Q_H3][p] = (query){ { pick(H, ~0u), pick(H, ~0u), pick(M, H) }, 3 };
	}
	static const sift_enc_opts eo[] = { { 0, 0, 0, 0 }, { SIFT_GROUPED, 0, 0, 0 }, { SIFT_GROUPED, SIFT_CT_BITMAP, 0, 0 }, { SIFT_LIST, 0, 0, 0 } };
	uint32_t *docs = malloc(C.nd * 4), *keys = malloc(C.nd * 4);
	for (int q = 0; q < NQ; q++)
		for (int p = 0; p < NPAIR; p++)
			for (int i = 0; i < Q[q][p].k; i++) {
				uint32_t t = Q[q][p].t[i], n = C.df[t];
				arrays[q][p][i] = malloc(n * 8);
				corpus_tids(&C, t, arrays[q][p][i]);
				for (int r = 0; r < NSIFT; r++) {
					size_t len = sift_encode(arrays[q][p][i], NULL, NULL, n, &eo[r], NULL);
					blobs[r][q][p][i] = aligned_alloc(64, (len + 63) & ~(size_t)63);
					sift_encode(arrays[q][p][i], NULL, NULL, n, &eo[r], blobs[r][q][p][i]);
				}
				memcpy(docs, C.doc + C.start[t], n * 4);
				for_encode(&fors[q][p][i], docs, n);
#ifdef HAVE_ROARING
				for (uint32_t j = 0; j < n; j++) keys[j] = ctid_key(arrays[q][p][i][j]);
				rdoc[q][p][i] = roaring_bitmap_of_ptr(n, docs), rctid[q][p][i] = roaring_bitmap_of_ptr(n, keys);
				roaring_bitmap_run_optimize(rdoc[q][p][i]), roaring_bitmap_run_optimize(rctid[q][p][i]);
				roaring_bitmap_shrink_to_fit(rdoc[q][p][i]), roaring_bitmap_shrink_to_fit(rctid[q][p][i]);
#endif
			}
	free(docs), free(keys);
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

static size_t arr_or(const sift_tid *a, size_t na, const sift_tid *b, size_t nb, sift_tid *o)
{
	size_t i = 0, j = 0, n = 0;
	while (i < na && j < nb) {
		uint64_t x = a[i], y = b[j];
		o[n++] = x < y ? x : y;
		i += x <= y, j += y <= x;
	}
	while (i < na) o[n++] = a[i++];
	while (j < nb) o[n++] = b[j++];
	return n;
}

enum { O_DECODE, O_AND, O_ANDCNT, O_OR, O_ORCNT, O_ANDNOTCNT, NO };
static const char *o_name[NO] = { "decode", "and", "and-count", "or", "or-count", "andnot-cnt" };

#ifdef HAVE_ROARING
static uint64_t roar_out(roaring_bitmap_t *x, int ctid)
{
	uint64_t n = roaring_bitmap_get_cardinality(x);
	roaring_bitmap_to_uint32_array(x, OUT32);
	for (uint64_t i = 0; i < n; i++) OUT[i] = ctid ? SIFT_TID(OUT32[i] / 512, OUT32[i] % 512 + 1) : C.tid[OUT32[i]];
	roaring_bitmap_free(x);
	return n;
}

static uint64_t run_roaring(roaring_bitmap_t **b, int k, int op, int ctid)
{
	switch (op) {
	case O_DECODE: return roar_out(roaring_bitmap_copy(b[0]), ctid);
	case O_AND: case O_ANDCNT: {
		if (k == 2 && op == O_ANDCNT) return roaring_bitmap_and_cardinality(b[0], b[1]);
		roaring_bitmap_t *x = roaring_bitmap_and(b[0], b[1]);
		if (k == 3) {
			if (op == O_ANDCNT) { uint64_t n = roaring_bitmap_and_cardinality(x, b[2]); roaring_bitmap_free(x); return n; }
			roaring_bitmap_and_inplace(x, b[2]);
		}
		if (op == O_ANDCNT) { uint64_t n = roaring_bitmap_get_cardinality(x); roaring_bitmap_free(x); return n; }
		return roar_out(x, ctid);
	}
	case O_OR: return roar_out(roaring_bitmap_or(b[0], b[1]), ctid);
	case O_ORCNT: return roaring_bitmap_or_cardinality(b[0], b[1]);
	default: return roaring_bitmap_andnot_cardinality(b[0], b[1]);
	}
}
#endif

static uint64_t run_query(int r, int q, int p, int op)
{
	const query *qq = &Q[q][p];
	int k = op == O_DECODE ? 1 : op == O_AND || op == O_ANDCNT ? qq->k : 2;
	const uint32_t *t = qq->t;
	if (r == P_U64) {
		sift_tid **a = arrays[q][p];
		switch (op) {
		case O_DECODE: memcpy(OUT, a[0], C.df[t[0]] * 8ull); return C.df[t[0]];
		case O_AND: case O_ANDCNT: {
			size_t n = arr_and(a[0], C.df[t[0]], a[1], C.df[t[1]], OUT);
			return k == 3 ? arr_and(OUT, n, a[2], C.df[t[2]], OUT) : n;
		}
		case O_OR: case O_ORCNT: return arr_or(a[0], C.df[t[0]], a[1], C.df[t[1]], OUT);
		default: return C.df[t[0]] - arr_and(a[0], C.df[t[0]], a[1], C.df[t[1]], OUT);
		}
	}
	if (r == P_FOR) {
		const forlist *f[3] = { &fors[q][p][0], &fors[q][p][1], &fors[q][p][2] }, *s[3];
		int cnt = op == O_ANDCNT || op == O_ORCNT || op == O_ANDNOTCNT;
		switch (op) {
		case O_DECODE: return for_decode(f[0], C.tid, OUT);
		case O_AND: case O_ANDCNT:
			/* lead with the shortest list */
			memcpy(s, f, sizeof s);
			for (int i = 1; i < k; i++)
				if (s[i]->n < s[0]->n) { const forlist *x = s[0]; s[0] = s[i], s[i] = x; }
			return for_and(s, k, C.tid, cnt ? NULL : OUT);
		case O_OR: case O_ORCNT: return for_or(f[0], f[1], C.tid, cnt ? NULL : OUT);
		default: return f[0]->n - for_and((const forlist *[]){ f[0]->n < f[1]->n ? f[0] : f[1], f[0]->n < f[1]->n ? f[1] : f[0] }, 2, C.tid, NULL);
		}
	}
#ifdef HAVE_ROARING
	if (r == P_RDOC || r == P_RCTID) return run_roaring(r == P_RDOC ? rdoc[q][p] : rctid[q][p], k, op, r == P_RCTID);
#else
	if (r >= P_RDOC) return 0;
#endif
	sift_cur c[3], op_c, *kids[3] = { &c[0], &c[1], &c[2] };
	for (int i = 0; i < k; i++) cur_blob(&c[i], blobs[r][q][p][i], cb[i]);
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

/* best of 9 trials of >= secs/9 each: robust against a loaded machine */
static double time_set(int r, int q, int op, double secs, uint64_t *res)
{
	double best = 1e30;
	for (int trial = 0; trial < 9; trial++) {
		uint64_t iters = 0;
		double t0 = now(), t1;
		do {
			*res = 0;
			for (int p = 0; p < NPAIR; p++) *res += run_query(r, q, p, op);
			iters++;
		} while ((t1 = now()) - t0 < secs / 9);
		if ((t1 - t0) / iters < best) best = (t1 - t0) / iters;
	}
	return best;
}

static const char *op_filter, *class_filter, *rep_filter;

static void ops(double secs)
{
	OUT = malloc(C.nd * 2 * 8), OUT32 = malloc(C.nd * 2 * 4);
	make_queries();
	printf("[%s] input Mpostings/s (sum of operand df / best-of-9 time); %d queries per class; baselines on the avx512 row\n", C.scen, NPAIR);
	printf("%-10s %-10s %-7s", "op", "class", "isa");
	for (int r = 0; r < NPR; r++) printf(" %9s", p_name[r]);
	printf("\n");
	for (int op = 0; op < NO; op++)
		for (int q = 0; q < NQ; q++) {
			if (op == O_DECODE && q != Q_HH && q != Q_MM) continue;
			if ((op_filter && strcmp(op_filter, o_name[op])) || (class_filter && strcmp(class_filter, q_name[q]))) continue;
			if ((op == O_OR || op == O_ORCNT || op == O_ANDNOTCNT) && q == Q_H3) continue;
			uint64_t ref = UINT64_MAX;
			for (int isa = 0; isa < SIFT_NISA; isa++) {
				if (!sift_set_isa(isa)) continue;
				printf("%-10s %-10s %-7s", o_name[op], q_name[q], sift_isa_name(isa));
				for (int r = 0; r < NPR; r++) {
					if ((r >= NSIFT && isa != SIFT_ISA_AVX512) || (rep_filter && strcmp(rep_filter, p_name[r]))) { printf(" %9s", "-"); continue; }
					uint64_t res;
					double t = time_set(r, q, op, secs, &res);
					if (ref == UINT64_MAX) ref = res;
					else if (res != ref && op != O_DECODE) printf("\nMISMATCH %s %s %s: %lu vs %lu\n", o_name[op], q_name[q], p_name[r], res, ref);
					printf(" %9.0f", in_postings(q, op) / t / 1e6);
					fflush(stdout);
				}
				printf("\n");
			}
		}
}

#define K1 1.2f
#define BB 0.75f
enum { F_OR2, F_OR3, F_AND2, F_AND3, F_ORHHH, NF };
static const char *f_name[NF] = { "or(h,m)", "or(h,m,m)", "and(h,m)", "and(h,m,m)", "or(h,h,h)" };
#define NRQ 48

typedef struct { int nt, conj; uint32_t t[4]; } rquery;

static float *knorm_slot, *knorm_doc;
static uint32_t *rowstart;
static double avgdl;

static void norms_init(void)
{
	uint32_t npages = SIFT_BLK(C.tid[C.nd - 1]) + 1;
	uint32_t *dl = calloc(C.nd, 4), *maxoff = calloc(npages, 4);
	for (uint64_t i = 0; i < C.np; i++) dl[C.doc[i]] += C.tf[i];
	double sum = 0;
	for (uint64_t d = 0; d < C.nd; d++) sum += dl[d];
	avgdl = sum / C.nd;
	for (uint64_t d = 0; d < C.nd; d++) {
		uint32_t b = SIFT_BLK(C.tid[d]);
		if (SIFT_OFF(C.tid[d]) > maxoff[b]) maxoff[b] = SIFT_OFF(C.tid[d]);
	}
	rowstart = malloc(npages * 4);
	uint64_t slots = 0;
	for (uint32_t p = 0; p < npages; p++) rowstart[p] = (uint32_t)slots, slots += maxoff[p];
	knorm_slot = calloc(slots, 4), knorm_doc = malloc(C.nd * 4);
	for (uint64_t d = 0; d < C.nd; d++) {
		knorm_doc[d] = K1 * (1 - BB + BB * (float)dl[d] / (float)avgdl);
		knorm_slot[rowstart[SIFT_BLK(C.tid[d])] + SIFT_OFF(C.tid[d]) - 1] = knorm_doc[d];
	}
	free(dl), free(maxoff);
}

static float idf(uint32_t df) { return logf(1.0f + ((float)C.nd - df + 0.5f) / (df + 0.5f)); }

/* per-posting bound: ceil(255 * tf / (tf + K(dl))) with the stored (nibble) tf */
static void term_ub(uint32_t t, uint8_t *ub)
{
	for (uint64_t i = C.start[t]; i < C.start[t + 1]; i++) {
		float tf = (float)sift_tf_quant(C.tf[i]);
		ub[i - C.start[t]] = (uint8_t)ceilf(255.0f * (tf / (tf + knorm_doc[C.doc[i]])));
	}
}

static int cmp_hit(const void *a, const void *b)
{
	const sift_hit *x = a, *y = b;
	return x->score < y->score ? 1 : x->score > y->score ? -1 : (x->tid > y->tid) - (x->tid < y->tid);
}

/* candidate docs and float scores (nibble tf, the codec's exact op order) or double exact-tf scores */
static uint32_t brute(const rquery *q, float *sc, uint8_t *cnt, uint32_t *cand, int exact, double *dsc)
{
	uint32_t n = 0;
	for (int i = 0; i < q->nt; i++) {
		uint32_t t = q->t[i];
		float w = idf(C.df[t]) * (K1 + 1);
		for (uint64_t j = C.start[t]; j < C.start[t + 1]; j++) {
			uint32_t d = C.doc[j];
			if (!cnt[d]) cand[n++] = d;
			cnt[d]++;
			if (exact) {
				double tf = C.tf[j];
				dsc[d] += (double)w * tf / (tf + (double)knorm_doc[d]);
			} else {
				float tf = (float)sift_tf_quant(C.tf[j]);
				sc[d] += w * (tf / (tf + knorm_doc[d]));
			}
		}
	}
	uint32_t m = 0;
	for (uint32_t i = 0; i < n; i++)
		if (!q->conj || cnt[cand[i]] == q->nt) cand[m++] = cand[i];
	return m;
}

static float *ubm;
static int cmp_ubm(const void *a, const void *b)
{
	float x = ubm[*(const uint32_t *)a], y = ubm[*(const uint32_t *)b];
	return x < y ? 1 : x > y ? -1 : 0;
}

/* offline: units (G pages) visited by DAAT-with-bound vs best-first (BMP) order, k = 10 */
static void simulate(const rquery *q, const uint32_t *cand, uint32_t n, const float *sc, uint32_t G, double *visit, double *scored)
{
	uint32_t nu = (SIFT_BLK(C.tid[C.nd - 1]) + G) / G;
	static float *ubu;
	static uint8_t *ubt;
	static uint32_t *us, *ue;
	uint32_t mx = SIFT_BLK(C.tid[C.nd - 1]) + 2;
	if (!ubu) ubu = malloc(mx * 4), ubt = malloc(mx), us = malloc(mx * 4), ue = malloc(mx * 4), ubm = malloc(mx * 4);
	memset(ubu, 0, nu * 4);
	for (int i = 0; i < q->nt; i++) {
		uint32_t t = q->t[i];
		float w = idf(C.df[t]) * (K1 + 1);
		memset(ubt, 0, nu);
		for (uint64_t j = C.start[t]; j < C.start[t + 1]; j++) {
			float tf = (float)sift_tf_quant(C.tf[j]);
			uint8_t u = (uint8_t)ceilf(255.0f * (tf / (tf + knorm_doc[C.doc[j]])));
			uint32_t x = SIFT_BLK(C.tid[C.doc[j]]) / G;
			if (u > ubt[x]) ubt[x] = u;
		}
		for (uint32_t x = 0; x < nu; x++)
			if (ubt[x]) ubu[x] += w * ubt[x] / 255.0f;
	}
	/* units holding candidates, in heap order: [us, ue) ranges of cand */
	uint32_t m = 0;
	for (uint32_t i = 0; i < n;) {
		uint32_t x = SIFT_BLK(C.tid[cand[i]]) / G, j = i;
		while (j < n && SIFT_BLK(C.tid[cand[j]]) / G == x) j++;
		ubm[m] = ubu[x], us[m] = i, ue[m] = j, m++;
		i = j;
	}
	for (int order = 0; order < 2; order++) {
		static uint32_t *idx;
		if (!idx) idx = malloc(mx * 4);
		for (uint32_t u = 0; u < m; u++) idx[u] = u;
		if (order) qsort(idx, m, 4, cmp_ubm);  /* best first */
		float top[10];
		int nh = 0;
		uint32_t vis = 0;
		uint64_t sd = 0;
		for (uint32_t a = 0; a < m; a++) {
			uint32_t u = idx[a];
			float b = ubm[u];
			if (nh == 10 && b * 1.0001f < top[0]) {
				if (order) break;
				continue;
			}
			vis++, sd += ue[u] - us[u];
			for (uint32_t i = us[u]; i < ue[u]; i++) {
				float s = sc[cand[i]];
				if (nh < 10) {
					int y = nh++;
					for (; y && top[y - 1] > s; y--) top[y] = top[y - 1];
					top[y] = s;
				} else if (s > top[0]) {
					int y = 0;
					for (; y + 1 < 10 && top[y + 1] < s; y++) top[y] = top[y + 1];
					top[y] = s;
				}
			}
		}
		visit[order] += m ? (double)vis / m : 0, scored[order] += n ? (double)sd / n : 0;
	}
}

static void rank_bench(double secs)
{
	norms_init();
	static rquery rq[NF][NRQ];
	const uint32_t H = 65536, M = 1024;
	for (int p = 0; p < NRQ; p++) {
		rq[F_OR2][p] = (rquery){ 2, 0, { pick(H, ~0u), pick(M, H) } };
		rq[F_OR3][p] = (rquery){ 3, 0, { pick(H, ~0u), pick(M, H), pick(M, H) } };
		rq[F_AND2][p] = (rquery){ 2, 1, { pick(H, ~0u), pick(M, H) } };
		rq[F_AND3][p] = (rquery){ 3, 1, { pick(H, ~0u), pick(M, H), pick(M, H) } };
		rq[F_ORHHH][p] = (rquery){ 3, 0, { pick(H, ~0u), pick(H, ~0u), pick(H, ~0u) } };
	}
	sift_norms nm = { rowstart, knorm_slot, 0 };
	uint8_t *ub = malloc(C.nd), *buf = aligned_alloc(64, SIFT_TOPK_BUF);
	float *sc = calloc(C.nd, 4);
	double *dsc = calloc(C.nd, 8);
	uint8_t *cnt = calloc(C.nd, 1);
	uint32_t *cand = malloc(C.nd * 4);
	static const sift_enc_opts eo[2] = { { 0, 0, 0, 1 }, { SIFT_GROUPED, 0, 0, 1 } };
	printf("[%s] top-10 BM25 (k1=%.1f b=%.2f avgdl=%.1f), %d queries per family, avx512 timings best-of-5\n", C.scen, K1, BB, avgdl, NRQ);
	printf("%-11s %-7s %9s %9s %7s %8s %8s %8s %7s\n", "family", "rep", "ms/q", "exh ms/q", "speedup", "grp%vis", "post%scr", "cand", "rec@10");
	for (int f = 0; f < NF; f++) {
		static const uint32_t SG[4] = { 256, 64, 16, 4 };
		double sim_v[4][2] = { { 0 } }, sim_s[4][2] = { { 0 } }, recall = 0, ncand = 0;
		/* reference scores, recall of nibble tf against exact tf, offline simulation */
		for (int p = 0; p < NRQ && !op_filter; p++) {
			rquery *q = &rq[f][p];
			uint32_t n = brute(q, sc, cnt, cand, 0, NULL);
			ncand += n;
			static sift_hit h1[1 << 22];
			uint32_t m = n < (1 << 22) ? n : (1 << 22);
			for (uint32_t i = 0; i < m; i++) h1[i] = (sift_hit){ C.tid[cand[i]], sc[cand[i]] };
			qsort(h1, m, sizeof(sift_hit), cmp_hit);
			memset(cnt, 0, C.nd);
			brute(q, NULL, cnt, cand, 1, dsc);
			static sift_hit h2[1 << 22];
			for (uint32_t i = 0; i < m; i++) h2[i] = (sift_hit){ C.tid[cand[i]], (float)dsc[cand[i]] };
			qsort(h2, m, sizeof(sift_hit), cmp_hit);
			uint32_t kk = m < 10 ? m : 10, hit = 0;
			for (uint32_t i = 0; i < kk; i++)
				for (uint32_t j = 0; j < kk; j++) hit += h1[i].tid == h2[j].tid;
			recall += kk ? (double)hit / kk : 1;
			qsort(cand, n, 4, sift_cmp_u32);
			for (int g = 0; g < 4; g++) simulate(q, cand, n, sc, SG[g], sim_v[g], sim_s[g]);
			memset(sc, 0, C.nd * 4), memset(dsc, 0, C.nd * 8), memset(cnt, 0, C.nd);
		}
		if (!op_filter)
			for (int g = 0; g < 4; g++)
				printf("  sim G=%-3u pages: visited units daat %5.1f%% bmp %5.1f%%, scored docs daat %5.1f%% bmp %5.1f%%\n", SG[g],
				       100 * sim_v[g][0] / NRQ, 100 * sim_v[g][1] / NRQ, 100 * sim_s[g][0] / NRQ, 100 * sim_s[g][1] / NRQ);
		for (int r = 0; r < 2; r++) {
			uint8_t *blob[NRQ][4];
			for (int p = 0; p < NRQ; p++)
				for (int i = 0; i < rq[f][p].nt; i++) {
					uint32_t t = rq[f][p].t[i], n = C.df[t];
					corpus_tids(&C, t, T);
					term_ub(t, ub);
					size_t len = sift_encode(T, C.tf + C.start[t], ub, n, &eo[r], NULL);
					blob[p][i] = aligned_alloc(64, (len + 63) & ~(size_t)63);
					sift_encode(T, C.tf + C.start[t], ub, n, &eo[r], blob[p][i]);
				}
			sift_set_isa(SIFT_ISA_AVX512);
			double tt[2];
			sift_topk_stats st[2] = { { 0, 0, 0 }, { 0, 0, 0 } };
			static sift_hit hits[2][NRQ][10];
			for (int ex = 0; ex < 2; ex++) {
				double best = 1e30;
				for (int trial = 0; trial < 5; trial++) {
					double t0 = now();
					memset(&st[ex], 0, sizeof st[ex]);
					for (int p = 0; p < NRQ; p++) {
						sift_cur c[4];
						sift_qterm qt[4];
						for (int i = 0; i < rq[f][p].nt; i++) {
							cur_blob(&c[i], blob[p][i], cb[i]);
							qt[i] = (sift_qterm){ &c[i], idf(C.df[rq[f][p].t[i]]) * (K1 + 1), 0 };
						}
						sift_topk_q tq = { qt, rq[f][p].nt, rq[f][p].conj, NULL, &nm, 10, ex };
						sift_topk(&tq, hits[ex][p], buf, &st[ex]);
					}
					double d = now() - t0;
					if (d < best) best = d;
					if (best > secs) break;
				}
				tt[ex] = best / NRQ * 1e3;
			}
			int same = !memcmp(hits[0], hits[1], sizeof hits[0]);
			printf("%-11s %-7s %9.3f %9.3f %6.1fx %7.1f%% %7.1f%% %8.0f %7.3f%s\n", f_name[f], r ? "grouped" : "auto", tt[0], tt[1],
			       tt[1] / tt[0], 100.0 * (st[0].groups - st[0].skipped) / st[0].groups, 100.0 * st[0].scored / st[1].scored,
			       ncand / NRQ, recall / NRQ, same ? "" : "  MISMATCH");
			for (int p = 0; p < NRQ; p++)
				for (int i = 0; i < rq[f][p].nt; i++) free(blob[p][i]);
		}
	}
}

/* facets.tsv: blk, off, foreign_id, country_code; counts per value for term queries */
static void facet_bench(const char *dir, double secs)
{
	char path[4096], cc[8];
	norms_init();
	snprintf(path, sizeof path, "%s/facets.tsv", dir);
	FILE *f = fopen(path, "r");
	if (!f) { perror(path); return; }
	uint16_t *col[2] = { malloc(C.nd * 2), malloc(C.nd * 2) };
	char names[1024][8];
	int nv[2] = { 0, 0 };
	unsigned long blk, off, src;
	for (uint64_t d = 0; d < C.nd && fscanf(f, "%lu %lu %lu %7s", &blk, &off, &src, cc) == 4; d++) {
		int v = 0;
		while (v < nv[1] && strcmp(names[v], cc)) v++;
		if (v == nv[1]) snprintf(names[nv[1]++], 8, "%s", cc);
		col[0][d] = (uint16_t)src, col[1][d] = (uint16_t)v;
		if ((int)src + 1 > nv[0]) nv[0] = (int)src + 1;
	}
	fclose(f);
	OUT = malloc(C.nd * 8);
	uint8_t *buf = aligned_alloc(64, SIFT_CUR_BUF * 2);
	static uint8_t vbuf[1024][SIFT_CUR_BUF];
	printf("[%s] facet counts: ms per query (48 term queries, best of 5); values: source %d, country %d\n", C.scen, nv[0], nv[1]);
	printf("%-8s %-7s %10s %10s %10s %10s\n", "facet", "query", "sift-bm", "docvalues", "roar-doc", "postings");
	uint32_t qt[2][48];
	for (int p = 0; p < 48; p++) qt[0][p] = pick(65536, ~0u), qt[1][p] = pick(1024, 65536);
	for (int fc = 0; fc < 2; fc++) {
		/* keyword index: one blob (and one roaring bitmap) per value */
		uint8_t *vb[1024];
		uint32_t *vd = malloc(C.nd * 4);
		sift_tid *vt = malloc(C.nd * 8);
#ifdef HAVE_ROARING
		roaring_bitmap_t *rv[1024];
#endif
		for (int v = 0; v < nv[fc]; v++) {
			uint32_t n = 0;
			for (uint64_t d = 0; d < C.nd; d++)
				if (col[fc][d] == v) vt[n] = C.tid[d], vd[n++] = (uint32_t)d;
			static const sift_enc_opts eo = { SIFT_GROUPED, SIFT_CT_BITMAP, 0, 0 };
			vb[v] = malloc(sift_encode_bound(n));
			sift_encode(vt, NULL, NULL, n, &eo, vb[v]);
#ifdef HAVE_ROARING
			rv[v] = roaring_bitmap_of_ptr(n, vd);
			roaring_bitmap_run_optimize(rv[v]);
#endif
		}
		for (int qc = 0; qc < 2; qc++) {
			uint8_t *qb[48];
			uint64_t post = 0;
#ifdef HAVE_ROARING
			roaring_bitmap_t *rq[48];
#endif
			for (int p = 0; p < 48; p++) {
				uint32_t t = qt[qc][p], n = C.df[t];
				static const sift_enc_opts eo = { SIFT_GROUPED, 0, 0, 0 };
				corpus_tids(&C, t, T);
				qb[p] = malloc(sift_encode_bound(n));
				sift_encode(T, NULL, NULL, n, &eo, qb[p]);
				post += n;
#ifdef HAVE_ROARING
				rq[p] = roaring_bitmap_of_ptr(n, C.doc + C.start[t]);
				roaring_bitmap_run_optimize(rq[p]);
#endif
			}
			double best[3] = { 1e30, 1e30, 1e30 };
			uint64_t chk[3] = { 0 };
			for (int trial = 0; trial < 5; trial++)
				for (int m = 0; m < 3; m++) {
					uint64_t counts[1024] = { 0 };
					double t0 = now();
					for (int p = 0; p < 48; p++) {
						if (m == 0) {
							sift_cur qc_, vc[1024], *vk[1024];
							cur_blob(&qc_, qb[p], buf);
							for (int v = 0; v < nv[fc]; v++) cur_blob(&vc[v], vb[v], vbuf[v]), vk[v] = &vc[v];
							sift_facets(&qc_, vk, nv[fc], counts);
						} else if (m == 1) {
							/* doc values: decode the term, look up each hit's value */
							sift_cur qc_;
							cur_blob(&qc_, qb[p], buf);
							size_t n = sift_tids(&qc_, OUT);
							for (size_t i = 0; i < n; i++)
								counts[col[fc][rowstart[SIFT_BLK(OUT[i])] + SIFT_OFF(OUT[i]) - 1]]++;  /* slot == doc here */
						} else {
#ifdef HAVE_ROARING
							for (int v = 0; v < nv[fc]; v++) counts[v] += roaring_bitmap_and_cardinality(rq[p], rv[v]);
#endif
						}
					}
					double d = now() - t0;
					if (d < best[m]) best[m] = d;
					chk[m] = 0;
					for (int v = 0; v < nv[fc]; v++) chk[m] += counts[v] * (uint64_t)(v + 1);
				}
			printf("%-8s %-7s %10.3f %10.3f %10.3f %10lu%s\n", fc ? "country" : "source", qc ? "mid" : "hi", best[0] / 48 * 1e3,
			       best[1] / 48 * 1e3, best[2] / 48 * 1e3, post / 48,
			       chk[0] == chk[1] && (chk[2] == chk[0] || !chk[2]) ? "" : "  MISMATCH");
			for (int p = 0; p < 48; p++) free(qb[p]);
		}
		for (int v = 0; v < nv[fc]; v++) free(vb[v]);
		free(vd), free(vt);
	}
	(void)secs;
}

typedef struct { uint32_t ng, *gid; uint8_t **chunk; } chunked;

static void chunk_term(uint32_t t, chunked *c)
{
	static uint64_t om[SIFT_GROUP_PAGES * 8];
	uint32_t n = C.df[t];
	corpus_tids(&C, t, T);
	c->ng = 0, c->gid = malloc(n * 4), c->chunk = malloc(n * sizeof(uint8_t *));
	for (uint32_t i = 0; i < n;) {
		uint32_t j = i, gid = SIFT_GID(T[i]), np = 0, last = UINT32_MAX;
		uint64_t pm[4] = { 0 };
		memset(om, 0, sizeof om);
		for (; j < n && SIFT_GID(T[j]) == gid; j++) {
			uint32_t pg = SIFT_BLK(T[j]) & 255, o = SIFT_OFF(T[j]) - 1;
			if (pg != last) np++, last = pg, pm[pg >> 6] |= 1ull << (pg & 63);
			om[(np - 1) * 8 + o / 64] |= 1ull << (o & 63);
		}
		size_t len = sift_chunk_encode(pm, om, SIFT_CT_BITMAP, NULL);
		c->chunk[c->ng] = aligned_alloc(64, (len + 63) & ~(size_t)63);
		sift_chunk_encode(pm, om, SIFT_CT_BITMAP, c->chunk[c->ng]);
		c->gid[c->ng++] = gid;
		i = j;
	}
}

static void canon_bench(void)
{
	static uint64_t oa[SIFT_GROUP_PAGES * 8] __attribute__((aligned(64))), ob[SIFT_GROUP_PAGES * 8] __attribute__((aligned(64)));
	static uint8_t gb[2][SIFT_GRP_BYTES];
	printf("[%s] AND count of two df>=64K terms, chunk form, in cache: Mpostings/s (best of 7)\n%-7s %10s %10s %10s\n", C.scen, "isa", "packed", "canon512", "ratio");
	uint32_t ta = pick(65536, ~0u), tb = pick(65536, ~0u);
	chunked a, b;
	chunk_term(ta, &a), chunk_term(tb, &b);
	for (int isa = 0; isa < SIFT_NISA; isa++) {
		if (!sift_set_isa(isa)) continue;
		double best[2] = { 1e30, 1e30 };
		uint64_t res[2] = { 0, 0 };
		for (int trial = 0; trial < 7; trial++)
			for (int m = 0; m < 2; m++) {
				double t0 = now();
				for (int it = 0; it < 10; it++) {
					res[m] = 0;
					for (uint32_t i = 0, j = 0; i < a.ng && j < b.ng;) {
						if (a.gid[i] != b.gid[j]) { a.gid[i] < b.gid[j] ? i++ : j++; continue; }
						if (m == 0) {
							sift_grp ga, gb_;
							sift_chunk_grp(a.chunk[i], a.gid[i], &ga, gb[0]);
							sift_chunk_grp(b.chunk[j], b.gid[j], &gb_, gb[1]);
							res[m] += sift_grp_and_count(&ga, &gb_);
						} else {
							uint64_t pa[4], pb[4], sel[4];
							sift_chunk_pagemask(a.chunk[i], pa), sift_chunk_pagemask(b.chunk[j], pb);
							for (int w = 0; w < 4; w++) sel[w] = pa[w] & pb[w];
							uint32_t np = sift_chunk_offsets(a.chunk[i], sel, oa);
							sift_chunk_offsets(b.chunk[j], sel, ob);
							sift_offmask_and(oa, oa, ob, np);
							res[m] += sift_offmask_popcount(oa, np);
						}
						i++, j++;
					}
				}
				double d = (now() - t0) / 10;
				if (d < best[m]) best[m] = d;
			}
		double inp = (double)C.df[ta] + C.df[tb];
		printf("%-7s %10.0f %10.0f %9.2fx%s\n", sift_isa_name(isa), inp / best[0] / 1e6, inp / best[1] / 1e6, best[1] / best[0],
		       res[0] == res[1] ? "" : "  MISMATCH");
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
	op_filter = argc > 5 ? argv[5] : NULL, class_filter = argc > 6 ? argv[6] : NULL, rep_filter = argc > 7 ? argv[7] : NULL;
	if (!strcmp(argv[3], "ops")) ops(argc > 4 ? atof(argv[4]) : 0.2);
	if (!strcmp(argv[3], "facets")) facet_bench(argv[1], 0);
	if (!strcmp(argv[3], "canon")) canon_bench();
	if (!strcmp(argv[3], "rank")) rank_bench(argc > 4 ? atof(argv[4]) : 0.5);  /* any 5th arg: skip references */
	return 0;
}
