/*
 * tools/gquant/test.c - the quantized keys of a compact chunk (src/col/gquant.c), standalone: no backend, no SQLite library (the C library and the portable host header).
 * Round trips on key shapes of every kind (scattered, ties, dates, ids, runs, bursts that force patches of every width), the bounds of the buckets, and the decoders of every SIMD level
 * the CPU runs (the scalar one, AVX2, NEON) against each other, on the blobs the writer makes and on damaged ones: the same answer and, if the blob is sound, the same keys.
 *
 *   make test [N=1]
 */
#include "core/host.h"
#include "col/gquant.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SiftCpu		sift_cpu;

void *
palloc(Size n)
{
	return malloc(n ? n : 1);
}

void *
palloc0(Size n)
{
	return calloc(1, n ? n : 1);
}

void
pfree(void *p)
{
	free(p);
}

void
initStringInfo(StringInfo s)
{
	s->maxlen = 1024;
	s->data = malloc(s->maxlen);
	s->len = 0;
	s->cursor = 0;
}

void
appendBinaryStringInfo(StringInfo s, const void *d, int n)
{
	while (s->len + n > s->maxlen)
		s->data = realloc(s->data, s->maxlen *= 2);
	memcpy(s->data + s->len, d, n);
	s->len += n;
}

static uint64 rs = 88172645463325252ULL;

static uint64
rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return rs;
}

static int
cmp64(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

#if defined(SIFT_HAVE_NEON)
#define GQ_LEVEL " neon"
#else
#define GQ_LEVEL " avx2"
#endif
#define SHAPES 9
#define GQ_WMAX 24
#define PAD 64							/* the bytes past a blob that the decoders may read */

/* n ascending keys of a shape: scattered, 10 ties each, ids with gaps, 1,430 dates, all equal, consecutive, runs, bursts of two scales, a few huge gaps */
static void
gen(int shape, uint32 n, int64 *k)
{
	uint64		base = rnd() >> 3;

	for (uint32 i = 0; i < n; i++)
		switch (shape)
		{
			case 0: k[i] = (int64) (rnd() % (300ULL * 86400 * 1000000)) - 100000000000LL; break;
			case 1: k[i] = (int64) (rnd() % 100) * 259200000LL; break;
			case 2: k[i] = (int64) (rnd() % ((uint64) n * 37)); break;
			case 3: k[i] = (int64) (rnd() % 1430) * 86400000000LL; break;
			case 4: k[i] = (int64) base; break;
			case 5: k[i] = (int64) base + i; break;
			case 6: k[i] = (int64) (rnd() % 8) * 1000000 + (i / 64) * 100000000LL; break;
			case 7: k[i] = (rnd() & 1) ? (int64) (rnd() % 1000) : (int64) (rnd() % 1000000000) + 100000000000LL; break;
			default: k[i] = (rnd() % 50 == 0) ? (int64) (rnd() >> 4) : (int64) (rnd() % 5000); break;
		}
	qsort(k, n, 8, cmp64);
}

static uint64 bad,
			checks;

static void
fail(const char *what, int shape, uint32 n, uint32 q)
{
	if (bad++ < 20)
		fprintf(stderr, "FAIL %s: shape %d n %u q %u\n", what, shape, n, q);
}

/* every decoder the CPU has, on the blob (padded), against the scalar one: the answer, and the keys when it is true */
static void
levels(const uint8 *blob, uint32 len, uint32 m, int64 first, int shape, uint32 q, const char *what)
{
	uint32		rows = (m + SIFT_GQ_MB - 1) / SIFT_GQ_MB * SIFT_GQ_MB;
	int64	   *want = aligned_alloc(64, (rows + 8) * 8),
			   *got = aligned_alloc(64, (rows + 8) * 8);
	uint32		wsh = 0,
				gsh = 0;
	bool		w = sift_gq_decode_for(SIFT_SIMD_SCALAR) (blob, len, m, first, want, &wsh);

	for (int l = SIFT_SIMD_AVX2; l <= SIFT_SIMD_TOP; l++)
	{
		bool		g;

		if (l > (int) sift_cpu.detected)
			break;
		memset(got, 0x5a, (rows + 8) * 8);
		g = sift_gq_decode_for((SiftSimdLevel) l) (blob, len, m, first, got, &gsh);
		checks++;
		if (g != w || (w && (gsh != wsh || memcmp(want, got, rows * 8) != 0)))
			fail(what, shape, m, q);
	}
	free(want);
	free(got);
}


/* A blob written straight from the layout of gquant.h, with every width 0..32 and patches of every size, and the keys it must decode to (computed here, bit by bit) */
static uint32
craft(uint32 m, uint8 *blob, int64 first, int64 *want, uint32 *sh)
{
	uint32		nmb = (m + SIFT_GQ_MB - 1) / SIFT_GQ_MB,
				len = 1,
				q = 0;

	memset(blob, 0, nmb * (1 + 128 + 2 + 32 + 128) + PAD);
	*sh = rnd() % 64;
	blob[0] = (uint8) *sh;
	for (uint32 j = 0; j < nmb; j++)
	{
		uint32		w = rnd() % 8 == 0 ? rnd() % 33 : rnd() % 16,
					n = rnd() % 3 == 0 ? 1 + rnd() % SIFT_GQ_MB : 0,
					e = n ? rnd() % 33 : 0,
					d[SIFT_GQ_MB],
					used;
		uint8	   *s = blob + len;

		for (uint32 i = 0; i < SIFT_GQ_MB; i++)
			d[i] = w == 0 ? 0 : (uint32) (rnd() & (((uint64) 1 << w) - 1));
		s[0] = (uint8) (w | (n ? 0x80 : 0));
		for (uint32 i = 0; i < SIFT_GQ_MB; i++)
			for (uint32 b = 0; b < w; b++)
				if ((d[i] >> b) & 1)
					s[1 + ((i * w + b) >> 3)] |= (uint8) (1 << ((i * w + b) & 7));
		used = 1 + 4 * w;
		if (n)
		{
			uint8	   *pos = s + used + 2;
			uint8	   *hi = pos + n;
			uint8		taken[SIFT_GQ_MB] = {0};

			s[used] = (uint8) n;
			s[used + 1] = (uint8) e;
			for (uint32 k = 0; k < n; k++)
			{
				uint32		at,
							v = e == 0 ? 0 : (uint32) (rnd() & (((uint64) 1 << e) - 1));

				do
					at = rnd() % SIFT_GQ_MB;
				while (taken[at]);
				taken[at] = 1;
				pos[k] = (uint8) at;
				for (uint32 b = 0; b < e; b++)
					if ((v >> b) & 1)
						hi[(k * e + b) >> 3] |= (uint8) (1 << ((k * e + b) & 7));
				d[at] |= w < 32 ? v << w : 0;
			}
			used += 2 + n + ((n * e + 7) >> 3);
		}
		for (uint32 i = 0; i < SIFT_GQ_MB; i++)
		{
			q += d[i];
			want[j * SIFT_GQ_MB + i] = (int64) ((uint64) first + ((uint64) q << *sh));
		}
		len += used;
	}
	return len;
}

int
main(int argc, char **argv)
{
	int			iters = (argc > 1 ? atoi(argv[1]) : 1) * 400;
	uint32		qs[] = {1, 2, 4, 8, 16};
	uint64		patched = 0,
				blobs = 0,
				wide[GQ_WMAX + 1] = {0};
	int64		key[512 + 64];

#if defined(__x86_64__)
	__builtin_cpu_init();
	sift_cpu.detected = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi2") && __builtin_cpu_supports("popcnt") ? SIFT_SIMD_AVX2 : SIFT_SIMD_SCALAR;
#elif defined(SIFT_HAVE_NEON)
	sift_cpu.detected = SIFT_SIMD_NEON;
#endif
	sift_cpu.active = sift_cpu.detected;
	for (int it = 0; it < iters; it++)
		for (int shape = 0; shape < SHAPES; shape++)
		{
			uint32		m = it % 5 == 0 ? 512 : 1 + rnd() % 512,
						q = qs[rnd() % 5],
						rows = (m + SIFT_GQ_MB - 1) / SIFT_GQ_MB * SIFT_GQ_MB;
			StringInfoData out;
			uint8	   *blob;
			uint32		len;
			int64	   *kmin = aligned_alloc(64, (rows + 8) * 8);
			uint32		sh;

			gen(shape, m, key);
			initStringInfo(&out);
			len = sift_gq_encode(key, m, q, &out);
			blob = calloc(1, len + PAD);
			memcpy(blob, out.data, len);
			free(out.data);
			blobs++;
			for (uint32 j = 0, p = 1; j * SIFT_GQ_MB < m; j++)
			{
				uint32		w = blob[p] & 0x7f,
							used = 1 + 4 * w;

				patched += blob[p] >> 7;
				wide[w <= GQ_WMAX ? w : GQ_WMAX]++;
				if (blob[p] & 0x80)
					used += 2 + blob[p + used] + ((blob[p + used] * blob[p + used + 1] + 7) >> 3);
				p += used;
			}
			if (!sift_gq_decode_for(SIFT_SIMD_SCALAR) (blob, len, m, key[0], kmin, &sh))
				fail("sound blob refused", shape, m, q);
			else
			{
				/* the key of every entry is in its bucket, the first is exact, the buckets do not decrease */
				if (kmin[0] != key[0])
					fail("first key", shape, m, q);
				for (uint32 i = 0; i < m; i++)
					if (key[i] < kmin[i] || key[i] > sift_gq_kmax(kmin[i], sh) || (i > 0 && kmin[i] < kmin[i - 1]))
						fail("bucket", shape, m, q);
				for (uint32 i = m; i < rows; i++)
					if (kmin[i] != kmin[m - 1])
						fail("padding", shape, m, q);
			}
			levels(blob, len, m, key[0], shape, q, "levels");
			/* damaged blobs: a few bytes changed, or the blob cut short */
			for (int d = 0; d < 6; d++)
			{
				uint8	   *dm = calloc(1, len + PAD);
				uint32		dl = len;

				memcpy(dm, blob, len);
				for (int c = 0, nc = 1 + rnd() % 3; c < nc; c++)
					dm[rnd() % len] = (uint8) rnd();
				if (d == 5)
					dl = rnd() % (len + 1);
				levels(dm, dl, m, key[0], shape, q, "damaged");
				free(dm);
			}
			free(kmin);
			free(blob);
		}
	for (int it = 0; it < iters * 4; it++)
	{
		uint32		m = 1 + rnd() % 512,
					rows = (m + SIFT_GQ_MB - 1) / SIFT_GQ_MB * SIFT_GQ_MB,
					len,
					sh,
					gsh;
		uint8	   *blob = malloc(16 * (1 + 128 + 2 + 32 + 128) + PAD);
		int64	   *want = aligned_alloc(64, (rows + 8) * 8),
				   *got = aligned_alloc(64, (rows + 8) * 8);
		int64		first = (int64) rnd();

		len = craft(m, blob, first, want, &sh);
		blobs++;
		for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_TOP; l++)
		{
			if (l > (int) sift_cpu.detected)
				break;
			memset(got, 0x5a, (rows + 8) * 8);
			checks++;
			if (!sift_gq_decode_for((SiftSimdLevel) l) (blob, len, m, first, got, &gsh) || gsh != sh || memcmp(want, got, rows * 8) != 0)
				fail("crafted blob", l, m, 0);
		}
		levels(blob, len, m, first, 99, 0, "crafted");
		free(blob);
		free(want);
		free(got);
	}
	printf("%llu blobs, %llu decoder comparisons, %llu mini-blocks patched; widths:", (unsigned long long) blobs, (unsigned long long) checks, (unsigned long long) patched);
	for (int w = 0; w <= GQ_WMAX; w++)
		printf(" %d:%llu", w, (unsigned long long) wide[w]);
	printf("\nlevels checked: scalar%s\n", sift_cpu.detected > SIFT_SIMD_SCALAR ? GQ_LEVEL : "");
	printf(bad ? "FAILED %llu\n" : "ok\n", (unsigned long long) bad);
	return bad ? 1 : 0;
}
