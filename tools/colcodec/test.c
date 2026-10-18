/*
 * test.c - standalone tests of the chunk codec (no backend linked; ASan/UBSan with SANITIZE=1):
 *   round trips of typical and adversarial chunks under every policy, with and without NULL slots;
 *   decode, random access, selected rows, filters on the encoded form and sums against the plain values;
 *   the same at every SIMD level, and the written bytes identical at every level;
 *   every kernel of the codec's table at every level against the scalar table, on exact-size buffers (the column engine's kernels the codec
 *   calls are tools/kernels' and the col_kernels_* regress tests');
 *   string lists and bit vectors;
 *   corruption fuzz: damaged blobs must raise an error of the corruption class or decode safely, never crash.
 * Usage: test [scale [seed]]   (scale 1 is about 30 s under ASan)
 */
#include "postgres.h"

#include <math.h>

#include "../../src/sift_colcodec_int.h"
#include "harness.h"
#include "ccshim.h"

typedef Rng CcgRng;
#define ccg_next rng_next
#include "../../src/sift_colcodec_gen.h"	/* the chunk generators of the in-backend test too */

static int	nfail;
static uint64 tag_hist[16][16];	/* [root tag][inner tag] of every written chunk */
static uint64 nchecks;

#define CHECK(c, ...) \
	do { nchecks++; if (!(c)) { nfail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); if (nfail > 20) exit(1); } } while (0)

static void *
xalloc(size_t n)
{
	void	   *p;

	if (posix_memalign(&p, 64, n ? n : 1))
		abort();
	return p;
}

/* ---------- data ---------- */

static const uint32 sizes[] = {0, 1, 2, 3, 31, 32, 33, 63, 64, 65, 1023, 1024, 1025, 1056, 2047, 2048, 2049, 5000, 33000, 70001};

static const char *const policies[] = {
	"auto", "none", "lz4", "lz4:9", "zstd", "zstd:19", "for", "delta", "rle", "dict", "alp", "for+lz4", "rle+dict", "dict+zstd",
	"delta+zstd", "scale", "scale+rle+dict+zstd", "for+delta+rle+dict+alp+scale+zstd:3"
};

#define NPOL ((int) lengthof(policies))

/* ---------- one chunk ---------- */

typedef struct Chunk
{
	SiftCcKind	kind;
	uint32		n;
	int64	   *v;
	uint64	   *nulls;			/* may be NULL */
	int64		min,
				max;
	bool		any;
} Chunk;

static bool
is_null(const Chunk *c, uint32 i)
{
	return c->nulls && ((c->nulls[i / 64] >> (i % 64)) & 1);
}

static void
chunk_range(Chunk *c)
{
	c->any = false;
	c->min = c->max = 0;
	for (uint32 i = 0; i < c->n; i++)
		if (!is_null(c, i))
		{
			c->min = c->any && c->min < c->v[i] ? c->min : c->v[i];
			c->max = c->any && c->max > c->v[i] ? c->max : c->v[i];
			c->any = true;
		}
}

static void
bits_naive(const int64 *dec, uint32 n, int64 lo, int64 hi, uint64 *bits)
{
	memset(bits, 0, sizeof(uint64) * ((n + 63) / 64));
	for (uint32 i = 0; i < n; i++)
		if (dec[i] >= lo && dec[i] <= hi)
			bits[i / 64] |= UINT64CONST(1) << (i % 64);
}

static SiftSimdLevel levels[3];
static int	nlevels;

/* The operations on an open chunk, compared with the plain values (non-NULL slots) and with the decode of the first level */
static void
check_open(const Chunk *c, SiftCc *cc, const char *pol, int64 *ref_dec, uint64 (*ref_bits)[8], Rng *r, bool first)
{
	uint32		n = c->n;
	uint32		nw = (n + 63) / 64;
	int64	   *dec = xalloc(sizeof(int64) * (n + SIFT_COL_PAD / 8));
	uint32	   *off = xalloc(sizeof(uint32) * (SIFT_CC_CAP32(n)));
	uint64	   *bits = xalloc(sizeof(uint64) * SIFT_COL_NWORDS(n));
	int64		lohi[8][2];
	int			nr = 0;

	sift_cc_decode(cc, dec);
	for (uint32 i = 0; i < n; i++)
		if (!is_null(c, i) && dec[i] != c->v[i])
		{
			CHECK(0, "%s: decode differs at row %u of %u: %lld vs %lld", pol, i, n, (long long) dec[i], (long long) c->v[i]);
			break;
		}
	if (first)
		memcpy(ref_dec, dec, sizeof(int64) * n);
	else
		CHECK(!n || !memcmp(ref_dec, dec, sizeof(int64) * n), "%s: decode differs between levels", pol);
	CHECK(sift_cc_nrows(cc) == n && sift_cc_kind(cc) == c->kind && sift_cc_min(cc) == c->min && sift_cc_max(cc) == c->max, "%s: header", pol);

	if (sift_cc_decode_off32(cc, off))
	{
		CHECK((uint64) c->max - (uint64) c->min <= 0xFFFFFFFFu, "%s: off32 offered for a wide range", pol);
		for (uint32 i = 0; i < n; i++)
			if (off[i] != (uint32) ((uint64) dec[i] - (uint64) c->min))
			{
				CHECK(0, "%s: off32 differs at row %u", pol, i);
				break;
			}
	}
	else
		CHECK((uint64) c->max - (uint64) c->min > 0xFFFFFFFFu, "%s: off32 refused for a narrow range", pol);

	for (uint32 k = 0; k < (n < 3000 ? n : 300); k++)
	{
		uint32		i = n < 3000 ? k : rng_below(r, n);

		CHECK(sift_cc_get(cc, i) == dec[i], "%s: get(%u) of %u", pol, i, n);
	}
	if (n)
	{
		uint32		m = 1 + rng_below(r, n < 200 ? n : 200),
				   *idx = xalloc(sizeof(uint32) * m);
		int64	   *sel = xalloc(sizeof(int64) * m);

		for (uint32 k = 0; k < m; k++)
			idx[k] = rng_below(r, n);
		sift_cc_decode_sel(cc, idx, m, sel);
		for (uint32 k = 0; k < m; k++)
			CHECK(sel[k] == dec[idx[k]], "%s: decode_sel", pol);
		free(idx);
		free(sel);
	}

	/* ranges: everything, nothing, one value, around the extremes, random pairs of present values */
	lohi[nr][0] = INT64_MIN, lohi[nr++][1] = INT64_MAX;
	lohi[nr][0] = 5, lohi[nr++][1] = 4;
	if (n)
	{
		int64		a = dec[rng_below(r, n)],
					b = dec[rng_below(r, n)];

		lohi[nr][0] = a, lohi[nr++][1] = a;
		lohi[nr][0] = a < b ? a : b, lohi[nr++][1] = a < b ? b : a;
		lohi[nr][0] = INT64_MIN, lohi[nr++][1] = a;
		lohi[nr][0] = a, lohi[nr++][1] = INT64_MAX;
		lohi[nr][0] = a == INT64_MIN ? a : a - 1, lohi[nr++][1] = a == INT64_MAX ? a : a + 1;
	}
	for (int q = 0; q < nr; q++)
	{
		uint64	   *want = xalloc(sizeof(uint64) * (nw ? nw : 1));
		int64		sum,
					wsum = 0;
		uint64		cnt,
					wcnt = 0;

		bits_naive(dec, n, lohi[q][0], lohi[q][1], want);
		memset(bits, 0xA5, sizeof(uint64) * nw);
		sift_cc_filter(cc, lohi[q][0], lohi[q][1], bits);
		CHECK(!nw || !memcmp(bits, want, sizeof(uint64) * nw), "%s: filter [%lld, %lld] of %u rows (tag %d)", pol, (long long) lohi[q][0],
			  (long long) lohi[q][1], n, sift_cc_tag(cc));
		if (q < 8 && ref_bits && first)
			memcpy(ref_bits[q], bits, sizeof(uint64) * (nw < 8 ? nw : 8));
		for (uint32 i = 0; i < n; i++)
			if ((want[i / 64] >> (i % 64)) & 1)
				wsum = (int64) ((uint64) wsum + (uint64) dec[i]), wcnt++;
		cnt = sift_cc_sum(cc, bits, &sum);
		CHECK(cnt == wcnt && sum == wsum, "%s: sum of the filter rows", pol);
		if (q == 0)
		{
			int64		all = 0;

			for (uint32 i = 0; i < n; i++)
				all = (int64) ((uint64) all + (uint64) dec[i]);
			cnt = sift_cc_sum(cc, NULL, &sum);
			CHECK(cnt == n && sum == all, "%s: sum of all rows", pol);
		}
		free(want);
	}
	free(dec);
	free(off);
	free(bits);
}

typedef struct OpenArg
{
	const uint8 *blob;
	Size		len;
	bool		verify;
	SiftCc	   *cc;
} OpenArg;

static void
open_fn(void *a)
{
	OpenArg    *o = a;

	o->cc = sift_cc_open(CurrentMemoryContext, o->blob, o->len, o->verify);
}

static void
test_chunk(Rng *r, Chunk *c, int ngen)
{
	char	   *blob0 = NULL;
	Size		len0 = 0;
	Size		size_none = 0;

	chunk_range(c);
	for (int p = 0; p < NPOL; p++)
	{
		SiftCcPolicy pol;
		StringInfoData s;
		SiftCcInfo info;
		const char *err = sift_cc_policy_parse(policies[p], &pol);
		uint8	   *blob;
		int64	   *ref = xalloc(sizeof(int64) * (c->n + 1));
		uint64		refb[8][8];
		OpenArg		oa;

		CHECK(!err, "policy %s: %s", policies[p], err ? err : "");
		initStringInfo(&s);
		sift_cc_encode(&s, c->kind, c->v, c->n, c->nulls, &pol, &info);
		tag_hist[info.tag & 15][info.inner & 15]++;
		CHECK(info.bytes == (uint32) s.len && (s.len & 7) == 0, "%s: info.bytes", policies[p]);
		blob = xalloc(s.len);
		memcpy(blob, s.data, s.len);
		if (p == 0)
			size_none = 0;
		if (!strcmp(policies[p], "none"))
			size_none = s.len;
		if (!strcmp(policies[p], "auto"))
		{
			len0 = s.len;
			blob0 = xalloc(s.len);
			memcpy(blob0, s.data, s.len);
		}
		if (p == 1 && size_none)
			CHECK(len0 <= size_none, "auto (%zu) larger than none (%zu), gen %d n %u", (size_t) len0, (size_t) size_none, ngen, c->n);

		oa = (OpenArg) {blob, s.len, true, NULL};
		if (!cc_try(open_fn, &oa))
		{
			CHECK(0, "%s: open failed on a fresh blob: %s (gen %d n %u)", policies[p], cc_errmsg, ngen, c->n);
			free(blob);
			free(ref);
			continue;
		}
		for (int l = 0; l < nlevels; l++)
		{
			SiftCcInfo i2;
			StringInfoData s2;

			cc_use_level(levels[l]);
			check_open(c, oa.cc, policies[p], ref, refb, r, l == 0);
			initStringInfo(&s2);
			sift_cc_encode(&s2, c->kind, c->v, c->n, c->nulls, &pol, &i2);
			CHECK(s2.len == s.len && !memcmp(s2.data, s.data, s.len), "%s: bytes written differ at level %d", policies[p], l);
			pfree(s2.data);
		}
		cc_use_level(levels[nlevels - 1]);
		sift_cc_close(oa.cc);
		free(blob);
		free(ref);
		pfree(s.data);
	}
	free(blob0);
}

static void
test_roundtrip(Rng *r, int scale)
{
	for (int g = 0; g < CCG_NGEN; g++)
		for (size_t si = 0; si < lengthof(sizes); si++)
			for (int rep = 0; rep < scale; rep++)
			{
				Chunk		c;
				uint32		n = sizes[si];

				if (n > 5000 && (g % 3 != rep % 3))
					continue;
				c.n = n;
				c.v = xalloc(sizeof(int64) * (n + 1));
				ccg_fill(r, g, n, c.v, &c.kind);
				c.nulls = NULL;
				test_chunk(r, &c, g);
				if (n)
				{
					c.nulls = xalloc(sizeof(uint64) * ((n + 63) / 64));
					for (uint32 w = 0; w < (n + 63) / 64; w++)
						c.nulls[w] = rng_below(r, 3) ? 0 : rng_next(r) & rng_next(r);
					if (rep == 0)
						c.nulls[0] |= 1;	/* a leading NULL */
					test_chunk(r, &c, g);
					memset(c.nulls, 0xFF, sizeof(uint64) * ((n + 63) / 64));	/* all NULL (bits past n are ignored) */
					test_chunk(r, &c, g);
					free(c.nulls);
				}
				free(c.v);
			}
}

/* Chunks of 1000 to 4000 values of the flag generator, whose run values and lengths are themselves runs */
static void
test_alternating(Rng *r, int scale)
{
	for (int rep = 0; rep < 8 * scale; rep++)
	{
		Chunk		c;

		c.n = 1000 + rng_below(r, 3001);
		c.v = xalloc(sizeof(int64) * c.n);
		ccg_fill(r, CCG_ALT, c.n, c.v, &c.kind);
		c.nulls = NULL;
		test_chunk(r, &c, CCG_ALT);
		c.nulls = xalloc(sizeof(uint64) * ((c.n + 63) / 64));
		for (uint32 w = 0; w < (c.n + 63) / 64; w++)
			c.nulls[w] = rng_next(r) & rng_next(r);
		test_chunk(r, &c, CCG_ALT);
		free(c.nulls);
		free(c.v);
	}
}

/* ---------- kernels ---------- */

static void
fill_random(Rng *r, void *p, size_t bytes)
{
	uint64	   *q = p;

	for (size_t i = 0; i < bytes / 8; i++)
		q[i] = rng_next(r);
	for (size_t i = bytes & ~(size_t) 7; i < bytes; i++)
		((uint8 *) p)[i] = (uint8) rng_next(r);
}

#define CMP(a, b, bytes, ...) \
	do { if (memcmp(a, b, bytes)) { CHECK(0, __VA_ARGS__); } else nchecks++; } while (0)

static void
test_kernels_level(Rng *r, SiftSimdLevel level, int iters)
{
	const SiftCcKernels *S = &sift_cc_kernels_scalar,
			   *K = sift_cc_kernels_for(level);
	const char *name = kt_level_name[level];

	for (int it = 0; it < iters; it++)
	{
		uint32		W = it % 33,
					rows = 1 + rng_below(r, it % 5 ? 70 : 3),
					base = (uint32) rng_next(r),
					lo = rng_below(r, 4) ? (uint32) rng_next(r) : rng_below(r, 3),
					span = rng_below(r, 3) == 0 ? 0xFFFFFFFFu : rng_below(r, 2) ? (uint32) rng_next(r) : rng_below(r, 100);
		uint64		words = sift_cc_words(rows * 32, W);
		uint32	   *src = xalloc(4 * words + SIFT_COL_PAD),
				   *d1 = xalloc(4 * (size_t) rows * 32),
				   *d2 = xalloc(4 * (size_t) rows * 32);
		uint64	   *b1 = xalloc(8 * ((rows + 1) / 2)),
				   *b2 = xalloc(8 * ((rows + 1) / 2));

		fill_random(r, src, 4 * words + SIFT_COL_PAD);
		S->unpack32(src, W, rows, base, d1);
		K->unpack32(src, W, rows, base, d2);
		CMP(d1, d2, 4 * (size_t) rows * 32, "%s unpack32 W=%u rows=%u", name, W, rows);
		S->unpack_range32(src, W, rows, lo, span, b1);
		K->unpack_range32(src, W, rows, lo, span, b2);
		CMP(b1, b2, 8 * ((rows + 1) / 2), "%s unpack_range32 W=%u rows=%u", name, W, rows);
		free(src), free(d1), free(d2), free(b1), free(b2);
	}
	for (int it = 0; it < iters; it++)
	{
		uint32		n = 1 + (rng_below(r, 3) ? rng_below(r, 300) : rng_below(r, 4000)),
					d = 1 + rng_below(r, 1000);
		int64		base = ccg_bits(r, 64);
		uint32	   *u = xalloc(4 * n + SIFT_COL_PAD),
				   *p1 = xalloc(4 * n),
				   *p2 = xalloc(4 * n),
				   *codes = xalloc(4 * n + SIFT_COL_PAD);
		int64	   *q1 = xalloc(8 * n),
				   *q2 = xalloc(8 * n);
		uint32		add = (uint32) rng_next(r);

		fill_random(r, u, 4 * n + SIFT_COL_PAD);
		for (uint32 i = 0; i < n; i++)
			codes[i] = rng_below(r, d);
		S->prefix32(u, n, p1, (uint32) base, add);
		K->prefix32(u, n, p2, (uint32) base, add);
		CMP(p1, p2, 4 * (size_t) n, "%s prefix32 n=%u", name, n);
		{
			int64		a64 = (int64) rng_next(r) >> rng_below(r, 64);

			S->prefix64(u, n, q1, base, a64);
			K->prefix64(u, n, q2, base, a64);
			CMP(q1, q2, 8 * (size_t) n, "%s prefix64 n=%u", name, n);
		}
		CHECK(S->max32(u, n) == K->max32(u, n) && S->max32(codes, n) == K->max32(codes, n), "%s max32 n=%u", name, n);
		free(u), free(p1), free(p2), free(codes), free(q1), free(q2);
	}
	for (int it = 0; it < iters; it++)
	{
		uint32		n = 1 + (rng_below(r, 3) ? rng_below(r, 300) : rng_below(r, 4000));
		int			e8 = rng_below(r, SIFT_CC_ALP_EMAX_F8 + 1),
					e4 = rng_below(r, SIFT_CC_ALP_EMAX_F4 + 1);
		int64		dbase8 = (int64) (rng_next(r) % ((UINT64CONST(1) << 53) - ((uint64) 1 << 32) - 1)) - ((int64) 1 << 52),
					dbase4 = (int64) rng_below(r, 1 << 24) - (1 << 23);
		uint32	   *u8 = xalloc(4 * n + SIFT_COL_PAD),
				   *u4 = xalloc(4 * n + SIFT_COL_PAD);
		int64	   *q1 = xalloc(8 * n),
				   *q2 = xalloc(8 * n);

		for (uint32 i = 0; i < n; i++)
		{
			u8[i] = rng_below(r, 2) ? (uint32) rng_next(r) : rng_below(r, 1 << 20);
			u4[i] = rng_below(r, 1 << 23);
		}
		S->alp_keys_f8(q1, u8, n, dbase8, sift_cc_p10[e8]);
		K->alp_keys_f8(q2, u8, n, dbase8, sift_cc_p10[e8]);
		CMP(q1, q2, 8 * (size_t) n, "%s alp_keys_f8 e=%d n=%u", name, e8, n);
		S->alp_keys_f4(q1, u4, n, dbase4, sift_cc_p10f[e4]);
		K->alp_keys_f4(q2, u4, n, dbase4, sift_cc_p10f[e4]);
		CMP(q1, q2, 8 * (size_t) n, "%s alp_keys_f4 e=%d n=%u", name, e4, n);
		free(u8), free(u4), free(q1), free(q2);
	}
	for (int it = 0; it < iters; it++)
	{
		uint32		nr = 1 + rng_below(r, rng_below(r, 4) ? 40 : 1500),
					total = 0;
		uint32	   *vals = xalloc(4 * nr + SIFT_COL_PAD),
				   *lens = xalloc(4 * nr + SIFT_COL_PAD);

		for (uint32 i = 0; i < nr; i++)
		{
			vals[i] = (uint32) rng_next(r);
			total += lens[i] = 1 + rng_below(r, rng_below(r, 2) ? 3 : 70);
		}
		{
			uint32	   *d1 = xalloc(4 * (size_t) total + SIFT_COL_PAD),
					   *d2 = xalloc(4 * (size_t) total + SIFT_COL_PAD);

			S->rle_expand32(vals, lens, nr, d1);
			K->rle_expand32(vals, lens, nr, d2);
			CMP(d1, d2, 4 * (size_t) total, "%s rle_expand32 runs=%u", name, nr);
			free(d1), free(d2);
		}
		free(vals), free(lens);
	}
}

/* ---------- strings and bit vectors ---------- */

static void
test_strs(Rng *r, int scale)
{
	for (int it = 0; it < 40 * scale; it++)
	{
		uint32		n = it % 5 == 0 ? it / 5 : rng_below(r, 3000);
		uint32	   *off = xalloc(4 * (n + 1));
		char	   *bytes = xalloc(n * 40 + 16);
		StringInfoData s;
		SiftCcPolicy pol;
		uint32	   *o2;
		char	   *b2;
		uint8	   *blob;
		uint32		tot = 0;

		sift_cc_policy_parse(policies[it % NPOL], &pol);
		off[0] = 0;
		for (uint32 i = 0; i < n; i++)
		{
			uint32		len = rng_below(r, 4) ? rng_below(r, 12) : rng_below(r, 40);

			for (uint32 j = 0; j < len; j++)
				bytes[tot + j] = "abcdefgh"[rng_below(r, it % 2 ? 3 : 8)];
			off[i + 1] = tot += len;
		}
		initStringInfo(&s);
		sift_cc_encode_strs(&s, off, bytes, n, &pol);
		blob = xalloc(s.len);
		memcpy(blob, s.data, s.len);
		CHECK(sift_cc_decode_strs(CurrentMemoryContext, blob, s.len, &o2, &b2) == n, "strs: count");
		CHECK(!memcmp(o2, off, 4 * (n + 1)) && !memcmp(b2, bytes, tot), "strs: round trip n=%u policy %s", n, policies[it % NPOL]);
		free(off), free(bytes), free(blob);
	}
}

static void
test_bits(Rng *r, int scale)
{
	static const uint32 nbs[] = {0, 1, 7, 8, 63, 64, 65, 1000, 4096, 70001};

	for (int it = 0; it < 60 * scale; it++)
	{
		uint32		nb = nbs[it % lengthof(nbs)],
					nw = (nb + 63) / 64;
		uint64	   *w = xalloc(8 * (nw + 1)),
				   *w2 = xalloc(8 * (nw + 1));
		int			dens = it / lengthof(nbs) % 6;
		StringInfoData s;
		SiftCcPolicy pol;
		uint8	   *blob;

		memset(w, dens == 1 ? 0xFF : 0, 8 * (nw + 1));
		for (uint32 i = 0; i < nb; i++)
			if (dens == 2 ? rng_below(r, 100) == 0 : dens == 3 ? rng_below(r, 100) != 0 : dens == 4 ? rng_below(r, 2) : 0)
				w[i / 64] ^= UINT64CONST(1) << (i % 64);
		if (dens == 5)
			for (uint32 i = 0; i < nb; i++)
				if ((i / 100) % 2)
					w[i / 64] |= UINT64CONST(1) << (i % 64);
		if (nb % 64)
			w[nw - 1] &= (UINT64CONST(1) << (nb % 64)) - 1;
		sift_cc_policy_parse(policies[it % NPOL], &pol);
		initStringInfo(&s);
		sift_cc_encode_bits(&s, w, nb, &pol);
		blob = xalloc(s.len);
		memcpy(blob, s.data, s.len);
		sift_cc_decode_bits(blob, s.len, nb, w2);
		CHECK(!memcmp(w, w2, 8 * nw), "bits: round trip nb=%u density %d", nb, dens);
		free(w), free(w2), free(blob);
	}
}

/* ---------- corruption fuzz ---------- */

typedef struct FuzzArg
{
	uint8	   *blob;
	Size		len;
	int			what;			/* 0 chunk, 1 strings, 2 bits */
	bool		verify;
	uint32		nbits;
	Rng		   *r;
} FuzzArg;

static void
fuzz_fn(void *a)
{
	FuzzArg    *f = a;

	if (f->what == 1)
	{
		uint32	   *o;
		char	   *b;

		sift_cc_decode_strs(CurrentMemoryContext, f->blob, f->len, &o, &b);
	}
	else if (f->what == 2)
	{
		uint64	   *w = palloc(8 * ((f->nbits + 63) / 64 + 1));

		sift_cc_decode_bits(f->blob, f->len, f->nbits, w);
		pfree(w);
	}
	else
	{
		SiftCc	   *cc = sift_cc_open(CurrentMemoryContext, f->blob, f->len, f->verify);
		uint32		n = sift_cc_nrows(cc);

		if (n <= (1u << 22))
		{
			int64	   *dec = palloc(sizeof(int64) * (n + SIFT_COL_PAD / 8));
			uint32	   *off = palloc(sizeof(uint32) * SIFT_CC_CAP32(n));
			uint64	   *bits = palloc(8 * SIFT_COL_NWORDS(n));
			int64		sum;

			sift_cc_decode(cc, dec);
			sift_cc_decode_off32(cc, off);
			sift_cc_filter(cc, sift_cc_min(cc), sift_cc_min(cc) + 1000, bits);
			sift_cc_sum(cc, bits, &sum);
			for (int k = 0; n && k < 20; k++)
				sift_cc_get(cc, rng_below(f->r, n));
			pfree(dec), pfree(off), pfree(bits);
		}
		sift_cc_close(cc);
	}
}

static void
mutate(Rng *r, uint8 *b, Size len)
{
	switch (rng_below(r, 6))
	{
		case 0:
			b[rng_below(r, len)] ^= (uint8) (1 << rng_below(r, 8));
			break;
		case 1:
			for (int k = 1 + rng_below(r, 4); k; k--)
				b[rng_below(r, len)] = (uint8) rng_next(r);
			break;
		case 2:
			{
				Size		at = rng_below(r, len),
							cnt = 1 + rng_below(r, 16);

				memset(b + at, rng_below(r, 2) ? 0 : 0xFF, Min(len - at, cnt));
				break;
			}
		case 3:
			{
				Size		at = rng_below(r, len > 8 ? len - 8 : 1) & ~(Size) 7;
				uint64		x = rng_below(r, 2) ? ~UINT64CONST(0) : rng_below(r, 1 << 16);

				memcpy(b + at, &x, Min((Size) 8, len - at));
				break;
			}
		case 4:
			{
				Size		at = rng_below(r, len);

				b[at] = rng_below(r, 2) ? b[at] + 1 : b[at] - 1;
				break;
			}
		default:
			{
				Size		a = rng_below(r, len),
							c = rng_below(r, len);

				memmove(b + (a & ~(Size) 7), b + (c & ~(Size) 7), Min((Size) 16, len - Max(a & ~(Size) 7, c & ~(Size) 7)));
				break;
			}
	}
}

static void
test_fuzz(Rng *r, int scale)
{
	uint64		errs = 0,
				oks = 0;

	for (int it = 0; it < 120; it++)
	{
		Chunk		c;
		SiftCcPolicy pol;
		StringInfoData s;
		int			what = it % 12 == 10 ? 1 : it % 12 == 11 ? 2 : 0;
		uint32		n = sizes[2 + rng_below(r, 17)] % 6000;
		uint32	   *off = NULL;
		char	   *bytes = NULL;
		uint64	   *w = NULL;
		uint8	   *orig;
		Size		olen;

		sift_cc_policy_parse(policies[rng_below(r, NPOL)], &pol);
		c.n = n;
		c.v = xalloc(sizeof(int64) * (n + 1));
		c.nulls = NULL;
		initStringInfo(&s);
		if (what == 0)
		{
			ccg_fill(r, rng_below(r, CCG_NGEN), n, c.v, &c.kind);
			sift_cc_encode(&s, c.kind, c.v, n, NULL, &pol, NULL);
		}
		else if (what == 1)
		{
			off = xalloc(4 * (n + 1));
			bytes = xalloc(n * 8 + 8);
			off[0] = 0;
			for (uint32 i = 0; i < n; i++)
			{
				off[i + 1] = off[i] + rng_below(r, 8);
				memset(bytes + off[i], 'a' + (i % 5), off[i + 1] - off[i]);
			}
			sift_cc_encode_strs(&s, off, bytes, n, &pol);
		}
		else
		{
			w = xalloc(8 * ((n + 63) / 64 + 1));
			memset(w, 0, 8 * ((n + 63) / 64 + 1));
			for (uint32 i = 0; i < n; i++)
				if (rng_below(r, 40) == 0)
					w[i / 64] |= UINT64CONST(1) << (i % 64);
			sift_cc_encode_bits(&s, w, n, &pol);
		}
		olen = s.len;
		orig = xalloc(olen);
		memcpy(orig, s.data, olen);
		pfree(s.data);
		for (int m = 0; m < 400 * scale; m++)
		{
			FuzzArg		fa = {NULL, olen, what, rng_below(r, 4) == 0, n, r};
			uint8	   *tmp = xalloc(olen);

			memcpy(tmp, orig, olen);
			mutate(r, tmp, olen);
			if (rng_below(r, 8) == 0)
				fa.len = rng_below(r, olen) & ~(Size) 7;
			fa.blob = xalloc(fa.len);
			memcpy(fa.blob, tmp, fa.len);
			free(tmp);
			if (cc_try(fuzz_fn, &fa))
				oks++;
			else
			{
				errs++;
				CHECK(cc_sqlstate == ERRCODE_DATA_CORRUPTED || cc_sqlstate == ERRCODE_FEATURE_NOT_SUPPORTED, "fuzz: unexpected error: %s",
					  cc_errmsg);
			}
			free(fa.blob);
			cc_arena_reset();
		}
		free(c.v), free(off), free(bytes), free(w), free(orig);
	}
	printf("fuzz: %llu errors raised, %llu damaged blobs decoded\n", (unsigned long long) errs, (unsigned long long) oks);
}

/* ---------- policy parsing ---------- */

static void
test_policy(void)
{
	static const char *const good[] = {"auto", "none", "lz4", "zstd:5", "for+dict", "rle+zstd:19", "alp+lz4:3"};
	static const char *const bad[] = {"", "gzip", "lz4:0", "zstd:23", "lz4:x", "none+for", "for+auto", "zstd:3+lz4"};
	SiftCcPolicy p;
	char	   *name;

	for (size_t i = 0; i < lengthof(good); i++)
	{
		CHECK(!sift_cc_policy_parse(good[i], &p), "policy %s refused", good[i]);
		name = sift_cc_policy_name(&p);
		CHECK(!sift_cc_policy_parse(name, &p), "policy name %s does not parse back", name);
	}
	for (size_t i = 0; i < lengthof(bad); i++)
		CHECK(sift_cc_policy_parse(bad[i], &p), "policy '%s' accepted", bad[i]);
	CHECK(!sift_cc_policy_for_column("zstd; price=alp+lz4 ; qty=none", "price", &p) && p.explicit_encs, "column policy: price");
	CHECK(!sift_cc_policy_for_column("zstd; price=alp+lz4 ; qty=none", "qty", &p) && p.mode == SIFT_CCM_NONE, "column policy: qty");
	CHECK(!sift_cc_policy_for_column("zstd; price=alp+lz4 ; qty=none", "other", &p) && p.alg == SIFT_CC_ALG_ZSTD, "column policy: default");
}

int
main(int argc, char **argv)
{
	int			scale = argc > 1 ? atoi(argv[1]) : 1;
	Rng			r;

	setvbuf(stdout, NULL, _IOLBF, 0);
	rng_seed(&r, argc > 2 ? (uint64) atoll(argv[2]) : 20260101);
	cc_init();
	nlevels = cc_levels(levels);
	printf("levels tested: %d (detected %s)\n", nlevels, kt_level_name[sift_cpu.detected]);

	test_policy();
	for (int l = 1; l < nlevels; l++)
		test_kernels_level(&r, levels[l], 3000 * scale);
	test_roundtrip(&r, scale);
	test_alternating(&r, scale);
	test_strs(&r, scale);
	test_bits(&r, scale);
	test_fuzz(&r, scale);
	printf("chunks written by root/inner node (const forbit forbyte delta rle dict alp wrap scale):\n");
	for (int t = 0; t < 10; t++)
		for (int i = 1; i < 10; i++)
			if (tag_hist[t][i])
				printf("  root %d inner %d: %llu\n", t, i, (unsigned long long) tag_hist[t][i]);
	printf("%llu checks, %d failures\n", (unsigned long long) nchecks, nfail);
	return nfail != 0;
}
