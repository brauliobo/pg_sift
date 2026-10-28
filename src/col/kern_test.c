/*
 * col/kern_test.c - test-only SQL functions sift._test_col_*, compiled only with SIFT_TESTING and registered by the pg_sift_test
 * extension. Random inputs of every slot of the column kernel table (all widths, boundary thresholds, unaligned starts and lengths,
 * n in 0..5000, every array ending at an inaccessible page where the contract says it ends) go through one SIMD level; the digest of
 * all outputs must equal the scalar one. unpack is also checked against the values it was packed from.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <math.h>

#include "common/pg_prng.h"
#include "fmgr.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "col/col.h"
#include "col/kern_int.h"

PG_FUNCTION_INFO_V1(sift_test_col_kernels);
PG_FUNCTION_INFO_V1(sift_test_col_kernels_active);
PG_FUNCTION_INFO_V1(sift_test_col_level);
PG_FUNCTION_INFO_V1(sift_test_simd_detected);

#define MAXN		5000
#define CASES		40
#define GUARD		8			/* garbage words / elements after an output, digested too */

typedef struct Map
{
	void	   *base;
	size_t		len;
} Map;

typedef struct Test
{
	pg_prng_state rs;
	uint64		h;
	const SiftColKernels *k;
	Map			maps[256];
	int			nmaps;
} Test;

static uint64
rnd(Test *t, uint64 n)
{
	return pg_prng_uint64_range(&t->rs, 0, n - 1);
}

static void
mix(Test *t, const void *p, size_t len)
{
	const uint8 *b = p;

	for (size_t i = 0; i < len; i++)
		t->h = (t->h ^ b[i]) * UINT64CONST(0x100000001B3);
}

static void
mix64(Test *t, uint64 v)
{
	mix(t, &v, sizeof(v));
}

/*
 * Arrays end at an inaccessible page where the contract says they end: an input has SIFT_COL_PAD readable bytes after its n elements
 * (garbage), an output n elements and then GUARD garbage ones that the digest covers (so a kernel that reads or writes more fails).
 */
static void *
guarded(Test *t, size_t len)
{
	uint8	   *p;

#ifdef __linux__
	size_t		ps = sysconf(_SC_PAGESIZE),
				pages = (len + ps - 1) / ps;
	uint8	   *base = mmap(NULL, (pages + 1) * ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (base == MAP_FAILED || mprotect(base + pages * ps, ps, PROT_NONE) != 0 || t->nmaps == lengthof(t->maps))
		elog(ERROR, "sift_col_kern: cannot map a guarded array: %m");
	t->maps[t->nmaps++] = (Map) {base, (pages + 1) * ps};
	p = base + pages * ps - len;
#else
	p = palloc(len);
#endif
	for (size_t i = 0; i < len; i++)
		p[i] = (uint8) pg_prng_uint32(&t->rs);
	return p;
}

static void *
in_array(Test *t, size_t es, uint32 n)
{
	return guarded(t, n * es + SIFT_COL_PAD);
}

static void *
out_array(Test *t, size_t es, uint32 n)
{
	return guarded(t, (n + GUARD) * es);
}

static void
unmap_all(Test *t)
{
#ifdef __linux__
	for (int i = 0; i < t->nmaps; i++)
		munmap(t->maps[i].base, t->maps[i].len);
	t->nmaps = 0;
#endif
}

static uint32
pick_n(Test *t)
{
	static const int edge[] = {0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024, 4095, 4096, 4097, 5000};

	return rnd(t, 3) ? rnd(t, MAXN + 1) : edge[rnd(t, lengthof(edge))];
}

/* A random word set with about 2^-density of its bits, 0 = dense */
static uint64
sparse(Test *t, int density)
{
	uint64		x = pg_prng_uint64(&t->rs);

	while (density-- > 0)
		x &= pg_prng_uint64(&t->rs);
	return x;
}

static uint64
width_max(int w)
{
	return w == SIFT_COLW_64 ? ~UINT64CONST(0) : (UINT64CONST(1) << (8 << w)) - 1;
}

static void
put(void *a, int w, uint32 i, uint64 v)
{
	switch (w)
	{
		case SIFT_COLW_8:
			((uint8 *) a)[i] = v;
			break;
		case SIFT_COLW_16:
			((uint16 *) a)[i] = v;
			break;
		case SIFT_COLW_32:
			((uint32 *) a)[i] = v;
			break;
		default:
			((uint64 *) a)[i] = v;
	}
}

/* A bit vector output: its padded words, then GUARD words that must stay what they were */
static uint64 *
out_bits(Test *t, uint32 n)
{
	return out_array(t, sizeof(uint64), SIFT_COL_NWORDS(n));
}

static void
mix_bits(Test *t, const uint64 *bits, uint32 words)
{
	mix(t, bits, (words + GUARD) * sizeof(uint64));
}

static uint64
edge64(Test *t, uint64 max)
{
	uint64		e[] = {0, 1, max, max - 1, max / 2, max / 2 + 1, pg_prng_uint64(&t->rs) & max, pg_prng_uint64(&t->rs) & max};

	return e[rnd(t, lengthof(e))];
}

static void
test_range(Test *t, int w)
{
	uint32		n = pick_n(t);
	uint64		max = width_max(w),
				lo = edge64(t, max),
				span = rnd(t, 3) ? edge64(t, max) : rnd(t, 16) & max;
	void	   *a = in_array(t, 1 << w, n);
	uint64	   *bits = out_bits(t, n);

	for (uint32 i = 0; i < n; i++)
	{
		uint64		v[] = {lo - 1, lo, lo + 1, lo + span - 1, lo + span, lo + span + 1, lo + rnd(t, span == max ? max : span + 1),
		pg_prng_uint64(&t->rs)};

		put(a, w, i, v[rnd(t, lengthof(v))] & max);
	}
	t->k->range[w] (a, n, lo, span, bits);
	mix_bits(t, bits, SIFT_COL_NWORDS(n));
}

/* The null flag test of the executors is the u8 range [1, 255]: checked against the plain loop as well as digested */
static void
test_nonzero(Test *t)
{
	uint32		n = pick_n(t);
	uint8	   *b = in_array(t, 1, n);
	uint64	   *bits = out_bits(t, n);

	for (uint32 i = 0; i < n; i++)
		b[i] = rnd(t, 3) ? 0 : 1 + rnd(t, 255);
	t->k->range[SIFT_COLW_8] (b, n, 1, 254, bits);
	for (uint32 i = 0; i < n; i++)
		if (((bits[i / 64] >> (i % 64)) & 1) != (b[i] != 0))
			elog(ERROR, "sift_col_kern: the non-zero test of %u bytes is wrong at %u", n, i);
	mix_bits(t, bits, SIFT_COL_NWORDS(n));
}

static void
test_lut(Test *t, int w)
{
	uint32		n = pick_n(t),
				ncodes = w == SIFT_COLW_8 ? 256 : w == SIFT_COLW_16 ? 65536 : 1 + rnd(t, 100000),
				words = (ncodes + 63) / 64;
	int			density = rnd(t, 4);
	uint64	   *lutbits = in_array(t, sizeof(uint64), words);
	void	   *a = in_array(t, 1 << w, n);
	uint64	   *bits = out_bits(t, n);

	for (uint32 i = 0; i < words; i++)
		lutbits[i] = sparse(t, density);
	for (uint32 i = 0; i < n; i++)
		put(a, w, i, rnd(t, ncodes));
	t->k->lut[w] (a, n, lutbits, bits);
	mix_bits(t, bits, SIFT_COL_NWORDS(n));
}

static void
test_bitops(Test *t)
{
	uint32		nw = rnd(t, 3) ? rnd(t, 70) : rnd(t, 9);
	uint64	   *a = in_array(t, sizeof(uint64), nw),
			   *b = in_array(t, sizeof(uint64), nw);
	uint32		cnt = 0,
				base = pg_prng_uint32(&t->rs),
				ni;
	uint32	   *idx;
	void		(*const ops[3]) (uint64 *, const uint64 *, uint32) = {t->k->bits_and, t->k->bits_or, t->k->bits_andnot};
	int			density = rnd(t, 5);

	for (uint32 i = 0; i < nw; i++)
	{
		a[i] = sparse(t, density);
		b[i] = sparse(t, density);
	}
	for (int o = 0; o < 3; o++)
	{
		uint64	   *dst = out_array(t, sizeof(uint64), nw);

		memcpy(dst, a, nw * sizeof(uint64));
		ops[o] (dst, b, nw);
		mix_bits(t, dst, nw);
	}
	mix64(t, t->k->bits_count(a, nw));
	mix64(t, t->k->bits_and_count(a, b, nw));
	for (uint32 i = 0; i < nw; i++)
		cnt += pg_popcount64(a[i]);
	idx = out_array(t, sizeof(uint32), cnt + SIFT_COL_IDXPAD);
	ni = t->k->bits_to_idx(a, nw, base, idx);
	mix64(t, ni);
	mix(t, idx, ni * sizeof(uint32));
}

static void
test_zone_range(Test *t)
{
	uint32		n = pick_n(t);
	int64	   *mn = in_array(t, sizeof(int64), n),
			   *mx = in_array(t, sizeof(int64), n);
	uint64	   *keep = out_bits(t, n);
	int64		ev[] = {PG_INT64_MIN, PG_INT64_MAX, 0, -1, 1, (int64) pg_prng_uint64(&t->rs), (int64) (pg_prng_uint64(&t->rs) >> 40)},
				lo = ev[rnd(t, lengthof(ev))],
				hi = rnd(t, 2) ? ev[rnd(t, lengthof(ev))] : lo + (int64) rnd(t, 100);

	for (uint32 i = 0; i < n; i++)
	{
		int64		c = rnd(t, 3) ? lo + (int64) rnd(t, 5) - 2 : (int64) pg_prng_uint64(&t->rs);

		mn[i] = rnd(t, 4) ? c : ev[rnd(t, lengthof(ev))];
		mx[i] = rnd(t, 4) ? (int64) ((uint64) c + rnd(t, 4)) : hi + (int64) rnd(t, 3) - 1;
	}
	t->k->zone_range(mn, mx, n, lo, hi, keep);
	mix_bits(t, keep, SIFT_COL_NWORDS(n));
}

static void
test_zone_set(Test *t)
{
	uint32		n = pick_n(t);
	uint64	  (*set)[4] = in_array(t, 4 * sizeof(uint64), n);
	uint64		probe[4];
	uint64	   *keep = out_bits(t, n);
	int			density = 2 + rnd(t, 5);

	for (int j = 0; j < 4; j++)
		probe[j] = sparse(t, density);
	for (uint32 i = 0; i < n; i++)
		for (int j = 0; j < 4; j++)
			set[i][j] = sparse(t, density);
	t->k->zone_set((const uint64 (*)[4]) set, n, probe, keep);
	mix_bits(t, keep, SIFT_COL_NWORDS(n));
}

static void
test_zone_notnull(Test *t)
{
	uint32		n = pick_n(t);
	uint32	   *nnull = in_array(t, sizeof(uint32), n),
			   *nvals = in_array(t, sizeof(uint32), n);
	uint64	   *keep = out_bits(t, n);

	for (uint32 i = 0; i < n; i++)
	{
		nvals[i] = rnd(t, 4) ? rnd(t, 3) : pg_prng_uint32(&t->rs);
		nnull[i] = rnd(t, 3) ? nvals[i] : rnd(t, 3);
	}
	t->k->zone_notnull(nnull, nvals, n, keep);
	mix_bits(t, keep, SIFT_COL_NWORDS(n));
}

/* n values of `bits` bits packed LSB first, the last block of 64 padded; the check is against the values */
static void
test_unpack(Test *t, uint32 bits)
{
	uint32		n = pick_n(t);
	uint32		blocks = (n + 63) / 64;
	uint32	   *vals = in_array(t, sizeof(uint32), n),
			   *dst = out_array(t, sizeof(uint32), n);
	uint8	   *src = in_array(t, 1, blocks * 8 * bits);

	memset(src, 0, blocks * 8 * bits);
	for (uint32 i = 0; i < n; i++)
	{
		uint64		pos = (uint64) i * bits,
					x;

		vals[i] = pg_prng_uint32(&t->rs) & ((1U << bits) - 1);
		x = sift_kern_ld64(src + (pos >> 3)) | (uint64) vals[i] << (pos & 7);
		memcpy(src + (pos >> 3), &x, sizeof(x));
	}
	t->k->unpack(src, bits, n, dst);
	if (memcmp(dst, vals, n * sizeof(uint32)) != 0)
		elog(ERROR, "sift_col_kern: unpack of %u values of %u bits is wrong", n, bits);
	mix(t, dst, (n + GUARD) * sizeof(uint32));
}

static int64
edge_base(Test *t)
{
	int64		e[] = {0, 1, -1, PG_INT64_MIN, PG_INT64_MAX, (int64) pg_prng_uint64(&t->rs)};

	return e[rnd(t, lengthof(e))];
}

static void
test_for(Test *t, int w)
{
	uint32		n = pick_n(t);
	void	   *a = in_array(t, 1 << w, n);
	int64	   *dst = out_array(t, sizeof(int64), n);
	uint32		na = 1 + rnd(t, 2000),
				k = rnd(t, 3) ? rnd(t, 600) : 0;
	void	   *src = in_array(t, 1 << w, na);
	uint32	   *idx = in_array(t, sizeof(uint32), k);
	int64	   *dsel = out_array(t, sizeof(int64), k);
	int64		base = edge_base(t);

	for (uint32 i = 0; i < n; i++)
		put(a, w, i, edge64(t, width_max(w)));
	t->k->for_i64[w] (a, n, base, dst);
	mix(t, dst, (n + GUARD) * sizeof(int64));
	for (uint32 i = 0; i < na; i++)
		put(src, w, i, edge64(t, width_max(w)));
	for (uint32 i = 0; i < k; i++)
		idx[i] = rnd(t, na);
	t->k->for_i64_sel[w] (src, idx, k, base, dsel);
	mix(t, dsel, (k + GUARD) * sizeof(int64));
}

static void
test_gather(Test *t)
{
	uint32		na = 1 + rnd(t, 2000),
				k = rnd(t, 3) ? rnd(t, 800) : rnd(t, 20);
	uint32	   *a = in_array(t, sizeof(uint32), na),
			   *idx = in_array(t, sizeof(uint32), k),
			   *dst = out_array(t, sizeof(uint32), k);

	for (uint32 i = 0; i < k; i++)
		idx[i] = rnd(t, na);
	t->k->gather_u32(a, idx, k, dst);
	mix(t, dst, (k + GUARD) * sizeof(uint32));
}

/* Float keys of every kind of double back to doubles, in place; a bit vector as byte flags (the bits past n are zero, as in a chunk's bitmap) */
static void
test_decode_lanes(Test *t)
{
	uint32		n = pick_n(t),
				words = (n + 63) / 64;
	int64	   *keys = out_array(t, sizeof(int64), n);
	uint64	   *bits = in_array(t, sizeof(uint64), words);
	uint8	   *flags = out_array(t, 1, (n + 63) / 64 * 64);
	uint64	   *want;

	for (uint32 i = 0; i < n; i++)
	{
		double		d[] = {0.0, -0.0, 1.5, -1.5, HUGE_VAL, -HUGE_VAL, (double) pg_prng_int64(&t->rs), pg_prng_double(&t->rs) - 0.5};
		uint64		b;

		memcpy(&b, &d[rnd(t, lengthof(d))], sizeof(b));
		keys[i] = rnd(t, 4) ? (int64) pg_prng_uint64(&t->rs) : (int64) (b ^ ((uint64) ((int64) b >> 63) >> 1));
	}
	want = palloc((n + 1) * sizeof(uint64));
	for (uint32 i = 0; i < n; i++)
	{
		double		f = sift_col_f8_from_key(keys[i]);

		memcpy(&want[i], &f, sizeof(f));
	}
	t->k->f8_from_keys(keys, n);
	if (n > 0 && memcmp(keys, want, n * sizeof(uint64)) != 0)
		elog(ERROR, "sift_col_kern: f8_from_keys differs from sift_col_f8_from_key");
	mix(t, keys, (n + GUARD) * sizeof(int64));
	for (uint32 w = 0; w < words; w++)
		bits[w] = rnd(t, 3) ? sparse(t, rnd(t, 4)) : pg_prng_uint64(&t->rs);
	if (n % 64)
		bits[words - 1] &= (UINT64CONST(1) << (n % 64)) - 1;
	t->k->bits_to_flags(bits, n, flags);
	for (uint32 i = 0; i < n; i++)
		if (flags[i] != sift_col_bit_test(bits, i))
			elog(ERROR, "sift_col_kern: flag %u is %d, not %d", i, flags[i], sift_col_bit_test(bits, i));
	mix(t, flags, n);
}

/* The running ends of np lists (empty ones, long runs, the odd length), the parent of every element they cover */
static void
test_expand_parents(Test *t)
{
	uint32		np = rnd(t, 3) ? rnd(t, 70) : rnd(t, 5),
				e = 0;
	uint32	   *end = in_array(t, sizeof(uint32), np),
			   *parent;
	int			shape = rnd(t, 3);

	for (uint32 p = 0; p < np; p++)
	{
		e += rnd(t, 3) == 0 ? 0 : rnd(t, shape == 0 ? 3 : shape == 1 ? 20 : 90);
		end[p] = e;
	}
	parent = out_array(t, sizeof(uint32), e + SIFT_COL_PAD / sizeof(uint32));
	t->k->expand_parents(end, np, parent);
	for (uint32 p = 0, i = 0; p < np; p++)
		for (; i < end[p]; i++)
			if (parent[i] != p)
				elog(ERROR, "sift_col_kern: the parent of element %u is %u, not %u", i, parent[i], p);
	mix(t, parent, e * sizeof(uint32));
}

/*
 * Strings of a few letters, so that near misses are common (and some bytes >= 0x80), behind a stretch of junk the kernel must not read, with the text
 * ending at the guard page; a needle cut from a string, from across two strings (never a match), or random; half of the cases fold the case of the
 * letters (the needle in lower case, the text with upper case letters and bytes that differ from a letter by 0x20 only). Checked against two plain loops.
 */
static void
test_str_contains(Test *t)
{
	uint32		n = rnd(t, 3) ? rnd(t, 200) : pick_n(t) % 700,
				junk = rnd(t, 3) ? 0 : rnd(t, 50),
				alpha = 1 + rnd(t, 5),
				maxlen = rnd(t, 3) == 0 ? 6 : rnd(t, 2) ? 40 : 300,
				total = junk,
				m,
				at;
	bool		fold = rnd(t, 2);
	uint32	   *off = in_array(t, sizeof(uint32), n + 1);
	uint64	   *out;
	uint8	   *text,
			   *needle;

	for (uint32 s = 0; s < n; s++)
	{
		off[s] = total;
		total += rnd(t, 4) == 0 ? 0 : rnd(t, maxlen + 1);
	}
	off[n] = total;
	text = guarded(t, Max(total, 1));
	for (uint32 i = 0; i < total; i++)
		text[i] = rnd(t, 40) == 0 ? (uint8) (0x80 + rnd(t, 128)) : (uint8) ((fold && rnd(t, 3) == 0 ? 'A' : 'a') + rnd(t, alpha));
	m = 1 + (rnd(t, 3) ? rnd(t, 8) : rnd(t, 100));
	needle = guarded(t, m);
	at = n > 0 ? off[rnd(t, n)] : 0;
	if (rnd(t, 4) != 0 && at + m <= total)
		memcpy(needle, text + at, m);		/* a slice that may start in one string and end in the next: a match only when it fits in one */
	else
		for (uint32 i = 0; i < m; i++)
			needle[i] = (uint8) ('a' + rnd(t, alpha));
	for (uint32 i = 0; fold && i < m; i++)
		needle[i] = needle[i] >= 'A' && needle[i] <= 'Z' ? needle[i] + 0x20 : needle[i];
	out = out_array(t, sizeof(uint64), SIFT_COLK_WORDS(n));
	t->k->str_contains(out, (const char *) text, off, n, (const char *) needle, m, fold);
	for (uint32 s = 0; s < n; s++)
	{
		bool		want = false;

		for (uint32 p = off[s]; !want && p + m <= off[s + 1]; p++)
		{
			uint32		i = 0;

			while (i < m && (text[p + i] == needle[i] || (fold && needle[i] >= 'a' && needle[i] <= 'z' && text[p + i] == needle[i] - 0x20)))
				i++;
			want = i == m;
		}
		if (sift_col_bit_test(out, s) != want)
			elog(ERROR, "sift_col_kern: string %u of %u (%u bytes) %s the %u bytes of the needle", s, n, off[s + 1] - off[s],
				 want ? "does not hold" : "holds", m);
	}
	for (uint32 s = n; s < SIFT_COLK_WORDS(n) * 64; s++)
		if (sift_col_bit_test(out, s))
			elog(ERROR, "sift_col_kern: bit %u past the %u strings is set", s, n);
	mix(t, out, SIFT_COLK_WORDS(n) * sizeof(uint64));
}

static int64
run_cases(const SiftColKernels *k, int32 seed)
{
	Test		t = {.h = UINT64CONST(0xCBF29CE484222325), .k = k};
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "col kernel test", ALLOCSET_DEFAULT_SIZES),
				old = MemoryContextSwitchTo(cx);

	pg_prng_seed(&t.rs, seed);
	for (int c = 0; c < CASES; c++)
	{
		for (int w = 0; w < 4; w++)
			test_range(&t, w);
		test_nonzero(&t);
		for (int w = 0; w < 3; w++)
			test_lut(&t, w);
		test_bitops(&t);
		test_zone_range(&t);
		test_zone_set(&t);
		test_zone_notnull(&t);
		for (uint32 b = 1; b <= 31; b++)
			test_unpack(&t, b);
		for (int w = 0; w < 4; w++)
			test_for(&t, w);
		test_gather(&t);
		test_decode_lanes(&t);
		test_expand_parents(&t);
		test_str_contains(&t);
		MemoryContextReset(cx);
		unmap_all(&t);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return (int64) t.h;
}

/* Digest of every kernel output of a level's table (a level the CPU lacks runs the best one below it) */
Datum
sift_test_col_kernels(PG_FUNCTION_ARGS)
{
	int32		level = PG_GETARG_INT32(0);

	if (level < SIFT_SIMD_SCALAR || level > SIFT_SIMD_AVX512)
		elog(ERROR, "sift_col_kern: no SIMD level %d", level);
	PG_RETURN_INT64(run_cases(sift_col_kernels_for((SiftSimdLevel) level), PG_GETARG_INT32(1)));
}

/* The same through sift_colk, the table the sift.simd dispatch chose */
Datum
sift_test_col_kernels_active(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(run_cases(sift_colk, PG_GETARG_INT32(0)));
}

/* The level the active table is */
Datum
sift_test_col_level(PG_FUNCTION_ARGS)
{
	for (int l = SIFT_SIMD_SCALAR; l <= SIFT_SIMD_TOP; l++)
		if (sift_colk == sift_col_kernels_for(l))
			PG_RETURN_TEXT_P(cstring_to_text(sift_simd_level_name(l)));
	elog(ERROR, "sift_col_kern: the active table is none of the levels");
}

/* The best level the CPU supports, whatever sift.simd says: the tests state their expectation of a level against it, not against an architecture */
Datum
sift_test_simd_detected(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(sift_simd_level_name(sift_cpu.detected)));
}

#endif							/* SIFT_TESTING */
