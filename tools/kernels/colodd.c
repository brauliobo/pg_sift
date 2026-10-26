/*
 * colodd - the column kernels (src/col/kern*.c) at the lengths where a vector loop meets its tail, on exact-size malloc'd buffers so
 * that ASan sees a read or write past what the contract grants: an input has SIFT_COL_PAD readable bytes after its n elements, an
 * output the room col/kern.h gives it and nothing more. Every level's table must give the scalar table's result on the bits that
 * are defined (below n).
 *   colodd [rounds]     rounds of random data per length and slot (default 20)
 */
#include "harness.h"

#include "col/col.h"
#include "col/kern.h"

static const int lens[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257,
						   511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097};

static uint64 rs = UINT64CONST(0x9E3779B97F4A7C15);
static long nfail;
static long ncase;

static uint64
rnd64(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return rs;
}

static uint64
rnd(uint64 n)
{
	return rnd64() % n;
}

/* exact size: an allocation of len bytes with random contents */
static void *
xalloc(size_t len)
{
	uint8	   *p = malloc(len ? len : 1);

	for (size_t i = 0; i < len; i++)
		p[i] = (uint8) rnd64();
	return p;
}

static void *
in_array(size_t es, size_t n)
{
	return xalloc(n * es + SIFT_COL_PAD);
}

static void
fail(const char *level, const char *slot, int n)
{
	nfail++;
	printf("FAIL %s %s n=%d\n", level, slot, n);
}

/* the bits below n of two bit vectors are equal */
static bool
bits_equal(const uint64 *a, const uint64 *b, int n)
{
	for (int w = 0; w < n / 64; w++)
		if (a[w] != b[w])
			return false;
	if (n % 64)
	{
		uint64		m = (UINT64CONST(1) << (n % 64)) - 1;

		return (a[n / 64] & m) == (b[n / 64] & m);
	}
	return true;
}

static uint64
sparse_word(void)
{
	uint64		x = rnd64();
	int			d = rnd(4);

	while (d-- > 0)
		x &= rnd64();
	return x;
}

static void
fill_words(uint64 *p, size_t nw)
{
	for (size_t i = 0; i < nw; i++)
		p[i] = sparse_word();
}

static uint64
width_mask(int w)
{
	return w == SIFT_COLW_64 ? ~UINT64CONST(0) : (UINT64CONST(1) << (8 << w)) - 1;
}

static void
put(void *a, int w, size_t i, uint64 v)
{
	switch (w)
	{
		case SIFT_COLW_8:
			((uint8 *) a)[i] = (uint8) v;
			break;
		case SIFT_COLW_16:
			((uint16 *) a)[i] = (uint16) v;
			break;
		case SIFT_COLW_32:
			((uint32 *) a)[i] = (uint32) v;
			break;
		default:
			((uint64 *) a)[i] = v;
	}
}

static uint64
edge_value(int w)
{
	uint64		m = width_mask(w),
				e[] = {0, 1, 2, m, m - 1, m >> 1, (m >> 1) + 1, rnd64() & m};

	return rnd(3) ? rnd64() & m : e[rnd(8)];
}

static void
test_range(const SiftColKernels *k, const SiftColKernels *s, const char *lv, int n)
{
	for (int w = 0; w < 4; w++)
	{
		size_t		nw = SIFT_COL_NWORDS(n);
		void	   *a = in_array((size_t) 1 << w, n);
		uint64	   *b0 = xalloc(nw * 8),
				   *b1 = xalloc(nw * 8);
		uint64		lo = edge_value(w),
					span = edge_value(w);

		for (int i = 0; i < n; i++)
			put(a, w, i, edge_value(w));
		s->range[w] (a, n, lo, span, b0);
		k->range[w] (a, n, lo, span, b1);
		if (!bits_equal(b0, b1, n))
			fail(lv, "range", n);
		free(a);
		free(b0);
		free(b1);
	}
}

static void
test_lut(const SiftColKernels *k, const SiftColKernels *s, const char *lv, int n)
{
	static const int nlut[3] = {256, 65536, 1000};

	for (int w = 0; w < 3; w++)
	{
		size_t		nw = SIFT_COL_NWORDS(n);
		void	   *a = in_array((size_t) 1 << w, n);
		uint64	   *lut = xalloc((nlut[w] + 63) / 64 * 8),
				   *b0 = xalloc(nw * 8),
				   *b1 = xalloc(nw * 8);

		fill_words(lut, (nlut[w] + 63) / 64);
		for (int i = 0; i < n; i++)
			put(a, w, i, w == 2 ? rnd(nlut[w]) : rnd64() & width_mask(w));
		s->lut[w] (a, n, lut, b0);
		k->lut[w] (a, n, lut, b1);
		if (!bits_equal(b0, b1, n))
			fail(lv, "lut", n);
		free(a);
		free(lut);
		free(b0);
		free(b1);
	}
}

static void
test_bitops(const SiftColKernels *k, const SiftColKernels *s, const char *lv, int n)
{
	uint32		nw = SIFT_COL_NWORDS(n);
	uint64	   *a = xalloc(nw * 8 + SIFT_COL_PAD),
			   *b = xalloc(nw * 8 + SIFT_COL_PAD),
			   *d0 = xalloc(nw * 8),
			   *d1 = xalloc(nw * 8);
	void		(*f0[3]) (uint64 *, const uint64 *, uint32) = {s->bits_and, s->bits_or, s->bits_andnot};
	void		(*f1[3]) (uint64 *, const uint64 *, uint32) = {k->bits_and, k->bits_or, k->bits_andnot};

	fill_words(a, nw);
	fill_words(b, nw);
	for (int op = 0; op < 3; op++)
	{
		memcpy(d0, a, nw * 8);
		memcpy(d1, a, nw * 8);
		f0[op] (d0, b, nw);
		f1[op] (d1, b, nw);
		if (memcmp(d0, d1, nw * 8) != 0)
			fail(lv, "bits_op", n);
	}
	if (s->bits_count(a, nw) != k->bits_count(a, nw))
		fail(lv, "bits_count", n);
	if (s->bits_and_count(a, b, nw) != k->bits_and_count(a, b, nw))
		fail(lv, "bits_and_count", n);
	free(a);
	free(b);
	free(d0);
	free(d1);
}

static void
test_to_idx(const SiftColKernels *k, const SiftColKernels *s, const char *lv, int n)
{
	uint32		nw = SIFT_COL_NWORDS(n),
				base = (uint32) rnd64(),
				c0,
				c1;
	uint64	   *bits = xalloc(nw * 8 + SIFT_COL_PAD);
	uint32	   *i0,
			   *i1;

	fill_words(bits, nw);
	/* the set bits are below n: a bit vector of n rows */
	for (uint32 i = n; i < nw * 64; i++)
		bits[i >> 6] &= ~(UINT64CONST(1) << (i & 63));
	c0 = s->bits_count(bits, nw);
	i0 = xalloc((c0 + SIFT_COL_IDXPAD) * 4);
	i1 = xalloc((c0 + SIFT_COL_IDXPAD) * 4);
	c0 = s->bits_to_idx(bits, nw, base, i0);
	c1 = k->bits_to_idx(bits, nw, base, i1);
	if (c0 != c1 || memcmp(i0, i1, c0 * 4) != 0)
		fail(lv, "bits_to_idx", n);
	free(bits);
	free(i0);
	free(i1);
}

static void
test_zones(const SiftColKernels *k, const SiftColKernels *s, const char *lv, int n)
{
	size_t		nw = SIFT_COL_NWORDS(n);
	int64	   *mn = in_array(8, n),
			   *mx = in_array(8, n);
	uint64	   *set = in_array(32, n),
				probe[4] = {sparse_word(), sparse_word(), sparse_word(), sparse_word()};
	uint32	   *nn = in_array(4, n),
			   *nv = in_array(4, n);
	uint64	   *k0 = xalloc(nw * 8),
			   *k1 = xalloc(nw * 8);
	int64		lo = (int64) edge_value(SIFT_COLW_64),
				hi = (int64) edge_value(SIFT_COLW_64);

	for (int i = 0; i < n; i++)
	{
		mn[i] = (int64) edge_value(SIFT_COLW_64);
		mx[i] = (int64) ((uint64) mn[i] + rnd(1000));
		nn[i] = rnd(4);
		nv[i] = rnd(2) ? nn[i] : nn[i] + rnd(3);
	}
	for (size_t i = 0; i < (size_t) n * 4; i++)
		set[i] = sparse_word();
	s->zone_range(mn, mx, n, lo, hi, k0);
	k->zone_range(mn, mx, n, lo, hi, k1);
	if (!bits_equal(k0, k1, n))
		fail(lv, "zone_range", n);
	fill_words(k0, nw);
	memcpy(k1, k0, nw * 8);
	s->zone_set((const uint64 (*)[4]) set, n, probe, k0);
	k->zone_set((const uint64 (*)[4]) set, n, probe, k1);
	if (!bits_equal(k0, k1, n))
		fail(lv, "zone_set", n);
	fill_words(k0, nw);
	memcpy(k1, k0, nw * 8);
	s->zone_notnull(nn, nv, n, k0);
	k->zone_notnull(nn, nv, n, k1);
	if (!bits_equal(k0, k1, n))
		fail(lv, "zone_notnull", n);
	free(mn);
	free(mx);
	free(set);
	free(nn);
	free(nv);
	free(k0);
	free(k1);
}

static void
test_decode(const SiftColKernels *k, const SiftColKernels *s, const char *lv, int n)
{
	for (uint32 bits = 1; bits < 32; bits += 1 + (uint32) rnd(4))
	{
		uint32		blocks = (n + 63) / 64;
		uint8	   *src = in_array(1, (size_t) blocks * 8 * bits);
		uint32	   *d0 = xalloc((size_t) n * 4),
				   *d1 = xalloc((size_t) n * 4);

		s->unpack(src, bits, n, d0);
		k->unpack(src, bits, n, d1);
		if (memcmp(d0, d1, (size_t) n * 4) != 0)
			fail(lv, "unpack", n);
		free(src);
		free(d0);
		free(d1);
	}
	for (int w = 0; w < 4; w++)
	{
		int64		base = (int64) edge_value(SIFT_COLW_64);
		void	   *a = in_array((size_t) 1 << w, n);
		int64	   *d0 = xalloc((size_t) n * 8),
				   *d1 = xalloc((size_t) n * 8);
		int			na = n + 1;
		void	   *src = in_array((size_t) 1 << w, na);
		uint32	   *idx = in_array(4, n);

		for (int i = 0; i < n; i++)
			put(a, w, i, edge_value(w));
		for (int i = 0; i < na; i++)
			put(src, w, i, edge_value(w));
		for (int i = 0; i < n; i++)
			idx[i] = rnd(na);
		s->for_i64[w] (a, n, base, d0);
		k->for_i64[w] (a, n, base, d1);
		if (memcmp(d0, d1, (size_t) n * 8) != 0)
			fail(lv, "for_i64", n);
		s->for_i64_sel[w] (src, idx, n, base, d0);
		k->for_i64_sel[w] (src, idx, n, base, d1);
		if (memcmp(d0, d1, (size_t) n * 8) != 0)
			fail(lv, "for_i64_sel", n);
		free(a);
		free(src);
		free(idx);
		free(d0);
		free(d1);
	}
	{
		int			na = n + 1;
		uint32	   *a = in_array(4, na),
				   *idx = in_array(4, n),
				   *d0 = xalloc((size_t) n * 4),
				   *d1 = xalloc((size_t) n * 4);

		for (int i = 0; i < n; i++)
			idx[i] = rnd(na);
		s->gather_u32(a, idx, n, d0);
		k->gather_u32(a, idx, n, d1);
		if (memcmp(d0, d1, (size_t) n * 4) != 0)
			fail(lv, "gather_u32", n);
		free(a);
		free(idx);
		free(d0);
		free(d1);
	}
}

static void
test_lanes(const SiftColKernels *k, const SiftColKernels *s, const char *lv, int n)
{
	size_t		words = (n + 63) / 64;
	int64	   *k0 = xalloc((size_t) n * 8),
			   *k1;
	uint64	   *bits = in_array(8, words);
	uint8	   *f0 = xalloc((size_t) (n + SIFT_COL_PAD - 1) / SIFT_COL_PAD * SIFT_COL_PAD),
			   *f1 = xalloc((size_t) (n + SIFT_COL_PAD - 1) / SIFT_COL_PAD * SIFT_COL_PAD);

	for (int i = 0; i < n; i++)
	{
		double		d[] = {0.0, -0.0, 1.5, -1.5, HUGE_VAL, -HUGE_VAL, (double) (int64) rnd64(), (double) rnd(1000) / 7.0 - 50.0};
		uint64		b;

		memcpy(&b, &d[rnd(8)], sizeof(b));
		k0[i] = rnd(4) ? (int64) rnd64() : (int64) (b ^ ((uint64) ((int64) b >> 63) >> 1));
	}
	k1 = xalloc((size_t) n * 8);
	memcpy(k1, k0, (size_t) n * 8);
	s->f8_from_keys(k0, n);
	k->f8_from_keys(k1, n);
	if (memcmp(k0, k1, (size_t) n * 8) != 0)
		fail(lv, "f8_from_keys", n);
	fill_words(bits, words);
	if (n % 64)
		bits[words - 1] &= (UINT64CONST(1) << (n % 64)) - 1;
	s->bits_to_flags(bits, n, f0);
	k->bits_to_flags(bits, n, f1);
	if (memcmp(f0, f1, n) != 0)
		fail(lv, "bits_to_flags", n);
	free(k0);
	free(k1);
	free(bits);
	free(f0);
	free(f1);
}

static void
test_parents(const SiftColKernels *k, const SiftColKernels *s, const char *lv, int n)
{
	/* np lists whose running ends reach about n elements */
	uint32		np = n ? 1 + (uint32) rnd(n < 70 ? n : 70) : 0,
				e = 0;
	uint32	   *end = in_array(4, np),
			   *p0,
			   *p1;

	for (uint32 p = 0; p < np; p++)
	{
		e += rnd(3) == 0 ? 0 : rnd(2 * (n / np + 1));
		end[p] = e;
	}
	p0 = xalloc((size_t) e * 4 + SIFT_COL_PAD);
	p1 = xalloc((size_t) e * 4 + SIFT_COL_PAD);
	s->expand_parents(end, np, p0);
	k->expand_parents(end, np, p1);
	if (memcmp(p0, p1, (size_t) e * 4) != 0)
		fail(lv, "expand_parents", n);
	free(end);
	free(p0);
	free(p1);
}

int
main(int argc, char **argv)
{
	int			rounds = argc > 1 ? atoi(argv[1]) : 20;
	const SiftColKernels *s;

	kt_cpu_init();
	s = sift_col_kernels_for(SIFT_SIMD_SCALAR);
	for (int level = SIFT_SIMD_AVX2; level <= SIFT_SIMD_TOP; level++)
	{
		const SiftColKernels *k = sift_col_kernels_for(level);

		if (k == s || (level > SIFT_SIMD_AVX2 && k == sift_col_kernels_for(level - 1)))
			continue;
		printf("level %s\n", kt_level_name[level]);
		for (int r = 0; r < rounds; r++)
			for (size_t i = 0; i < lengthof(lens); i++)
			{
				int			n = lens[i];

				test_range(k, s, kt_level_name[level], n);
				test_lut(k, s, kt_level_name[level], n);
				test_bitops(k, s, kt_level_name[level], n);
				test_to_idx(k, s, kt_level_name[level], n);
				test_zones(k, s, kt_level_name[level], n);
				test_decode(k, s, kt_level_name[level], n);
				test_lanes(k, s, kt_level_name[level], n);
				test_parents(k, s, kt_level_name[level], n);
				ncase++;
			}
	}
	printf("%ld length cases, %ld failures\n%s\n", ncase, nfail, nfail ? "FAILED" : "OK");
	return nfail != 0;
}
