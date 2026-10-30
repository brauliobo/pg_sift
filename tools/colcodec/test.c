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

#include "../../src/col/codec_int.h"
#include "harness.h"
#include "ccshim.h"

typedef SiftRng CcgRng;
#define ccg_next sift_rng_next
#include "../../src/col/codec_gen.h"	/* the chunk generators of the in-backend test too */

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

static const uint32 sizes[] = {0, 1, 2, 3, 31, 32, 33, 63, 64, 65, 1023, 1024, 1025, 1056, 2047, 2048, 2049, 5000, 33000, 70001};

static const char *const policies[] = {
	"auto", "none", "lz4", "lz4:9", "zstd", "zstd:19", "for", "delta", "rle", "dict", "alp", "for+lz4", "rle+dict", "dict+zstd",
	"delta+zstd", "scale", "scale+rle+dict+zstd", "for+delta+rle+dict+alp+scale+zstd:3"
};

#define NPOL ((int) lengthof(policies))

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
check_open(const Chunk *c, SiftCc *cc, const char *pol, int64 *ref_dec, uint64 (*ref_bits)[8], SiftRng *r, bool first)
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
		uint32		i = n < 3000 ? k : sift_rng_below(r, n);

		CHECK(sift_cc_get(cc, i) == dec[i], "%s: get(%u) of %u", pol, i, n);
	}
	if (n)
	{
		uint32		m = 1 + sift_rng_below(r, n < 200 ? n : 200),
				   *idx = xalloc(sizeof(uint32) * m);
		int64	   *sel = xalloc(sizeof(int64) * m);

		for (uint32 k = 0; k < m; k++)
			idx[k] = sift_rng_below(r, n);
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
		int64		a = dec[sift_rng_below(r, n)],
					b = dec[sift_rng_below(r, n)];

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
test_chunk(SiftRng *r, Chunk *c, int ngen)
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
		if (!pgshim_try(open_fn, &oa))
		{
			CHECK(0, "%s: open failed on a fresh blob: %s (gen %d n %u)", policies[p], pgshim_errmsg, ngen, c->n);
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
test_roundtrip(SiftRng *r, int scale)
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
						c.nulls[w] = sift_rng_below(r, 3) ? 0 : sift_rng_next(r) & sift_rng_next(r);
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
test_alternating(SiftRng *r, int scale)
{
	for (int rep = 0; rep < 8 * scale; rep++)
	{
		Chunk		c;

		c.n = 1000 + sift_rng_below(r, 3001);
		c.v = xalloc(sizeof(int64) * c.n);
		ccg_fill(r, CCG_ALT, c.n, c.v, &c.kind);
		c.nulls = NULL;
		test_chunk(r, &c, CCG_ALT);
		c.nulls = xalloc(sizeof(uint64) * ((c.n + 63) / 64));
		for (uint32 w = 0; w < (c.n + 63) / 64; w++)
			c.nulls[w] = sift_rng_next(r) & sift_rng_next(r);
		test_chunk(r, &c, CCG_ALT);
		free(c.nulls);
		free(c.v);
	}
}

#include "test_kernels.h"

static void
test_bits(SiftRng *r, int scale)
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
			if (dens == 2 ? sift_rng_below(r, 100) == 0 : dens == 3 ? sift_rng_below(r, 100) != 0 : dens == 4 ? sift_rng_below(r, 2) : 0)
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

#include "test_fuzz.h"

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
	SiftRng			r;

	setvbuf(stdout, NULL, _IOLBF, 0);
	sift_rng_seed(&r, argc > 2 ? (uint64) atoll(argv[2]) : 20260101);
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
