/*
 * test_kernels.h - the kernel and string tests of test.c: every SIMD level against the scalar reference, and the string chunks.
 * Included once, by test.c, where the text stood.
 */
#ifndef SIFT_COLCODEC_TEST_KERNELS_H
#define SIFT_COLCODEC_TEST_KERNELS_H

static void
fill_random(SiftRng *r, void *p, size_t bytes)
{
	uint64	   *q = p;

	for (size_t i = 0; i < bytes / 8; i++)
		q[i] = sift_rng_next(r);
	for (size_t i = bytes & ~(size_t) 7; i < bytes; i++)
		((uint8 *) p)[i] = (uint8) sift_rng_next(r);
}

#define CMP(a, b, bytes, ...) \
	do { if (memcmp(a, b, bytes)) { CHECK(0, __VA_ARGS__); } else nchecks++; } while (0)

static void
test_kernels_level(SiftRng *r, SiftSimdLevel level, int iters)
{
	const SiftCcKernels *S = &sift_cc_kernels_scalar,
			   *K = sift_cc_kernels_for(level);
	const char *name = kt_level_name[level];

	for (int it = 0; it < iters; it++)
	{
		uint32		W = it % 33,
					rows = 1 + sift_rng_below(r, it % 5 ? 70 : 3),
					base = (uint32) sift_rng_next(r),
					lo = sift_rng_below(r, 4) ? (uint32) sift_rng_next(r) : sift_rng_below(r, 3),
					span = sift_rng_below(r, 3) == 0 ? 0xFFFFFFFFu : sift_rng_below(r, 2) ? (uint32) sift_rng_next(r) : sift_rng_below(r, 100);
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
		uint32		n = 1 + (sift_rng_below(r, 3) ? sift_rng_below(r, 300) : sift_rng_below(r, 4000)),
					d = 1 + sift_rng_below(r, 1000);
		int64		base = ccg_bits(r, 64);
		uint32	   *u = xalloc(4 * n + SIFT_COL_PAD),
				   *p1 = xalloc(4 * n),
				   *p2 = xalloc(4 * n),
				   *codes = xalloc(4 * n + SIFT_COL_PAD);
		int64	   *q1 = xalloc(8 * n),
				   *q2 = xalloc(8 * n);
		uint32		add = (uint32) sift_rng_next(r);

		fill_random(r, u, 4 * n + SIFT_COL_PAD);
		for (uint32 i = 0; i < n; i++)
			codes[i] = sift_rng_below(r, d);
		S->prefix32(u, n, p1, (uint32) base, add);
		K->prefix32(u, n, p2, (uint32) base, add);
		CMP(p1, p2, 4 * (size_t) n, "%s prefix32 n=%u", name, n);
		{
			int64		a64 = (int64) sift_rng_next(r) >> sift_rng_below(r, 64);

			S->prefix64(u, n, q1, base, a64);
			K->prefix64(u, n, q2, base, a64);
			CMP(q1, q2, 8 * (size_t) n, "%s prefix64 n=%u", name, n);
		}
		CHECK(S->max32(u, n) == K->max32(u, n) && S->max32(codes, n) == K->max32(codes, n), "%s max32 n=%u", name, n);
		free(u), free(p1), free(p2), free(codes), free(q1), free(q2);
	}
	for (int it = 0; it < iters; it++)
	{
		uint32		n = 1 + (sift_rng_below(r, 3) ? sift_rng_below(r, 300) : sift_rng_below(r, 4000));
		int			e8 = sift_rng_below(r, SIFT_CC_ALP_EMAX_F8 + 1),
					e4 = sift_rng_below(r, SIFT_CC_ALP_EMAX_F4 + 1);
		int64		dbase8 = (int64) (sift_rng_next(r) % ((UINT64CONST(1) << 53) - ((uint64) 1 << 32) - 1)) - ((int64) 1 << 52),
					dbase4 = (int64) sift_rng_below(r, 1 << 24) - (1 << 23);
		uint32	   *u8 = xalloc(4 * n + SIFT_COL_PAD),
				   *u4 = xalloc(4 * n + SIFT_COL_PAD);
		int64	   *q1 = xalloc(8 * n),
				   *q2 = xalloc(8 * n);

		for (uint32 i = 0; i < n; i++)
		{
			u8[i] = sift_rng_below(r, 2) ? (uint32) sift_rng_next(r) : sift_rng_below(r, 1 << 20);
			u4[i] = sift_rng_below(r, 1 << 23);
		}
		S->alp_keys_f8(q1, u8, n, dbase8, sift_cc_p10[e8]);
		K->alp_keys_f8(q2, u8, n, dbase8, sift_cc_p10[e8]);
		CMP(q1, q2, 8 * (size_t) n, "%s alp_keys_f8 e=%d n=%u", name, e8, n);
		S->alp_keys_f4(q1, u4, n, dbase4, sift_cc_p10f[e4]);
		K->alp_keys_f4(q2, u4, n, dbase4, sift_cc_p10f[e4]);
		CMP(q1, q2, 8 * (size_t) n, "%s alp_keys_f4 e=%d n=%u", name, e4, n);
		{
			double	   *f1 = xalloc(8 * n),
					   *f2 = xalloc(8 * n);
			uint32	   *s1 = xalloc(4 * n),
					   *s2 = xalloc(4 * n);
			int64	   *t1 = xalloc(8 * n),
					   *t2 = xalloc(8 * n);
			uint32		sb = (uint32) sift_rng_next(r),
						sm = sift_rng_below(r, 2) ? (uint32) sift_rng_next(r) : sift_rng_below(r, 100);
			int64		tb = (int64) sift_rng_next(r),
						tm = sift_rng_below(r, 2) ? (int64) sift_rng_next(r) : (int64) sift_rng_below(r, 100000);

			S->alp_f8(f1, u8, n, dbase8, sift_cc_p10[e8]);
			K->alp_f8(f2, u8, n, dbase8, sift_cc_p10[e8]);
			CMP(f1, f2, 8 * (size_t) n, "%s alp_f8 e=%d n=%u", name, e8, n);
			S->alp_f4(f1, u4, n, dbase4, sift_cc_p10f[e4]);
			K->alp_f4(f2, u4, n, dbase4, sift_cc_p10f[e4]);
			CMP(f1, f2, 8 * (size_t) n, "%s alp_f4 e=%d n=%u", name, e4, n);
			memcpy(s1, u8, 4 * n), memcpy(s2, u8, 4 * n);
			S->scale32(s1, n, sb, sm);
			K->scale32(s2, n, sb, sm);
			CMP(s1, s2, 4 * (size_t) n, "%s scale32 n=%u", name, n);
			for (uint32 i = 0; i < n; i++)
				t1[i] = t2[i] = (int64) ((uint64) u8[i] << (i % 33)) ^ (int64) (i * UINT64CONST(0x9E3779B97F4A7C15));
			S->scale64(t1, n, tb, tm);
			K->scale64(t2, n, tb, tm);
			CMP(t1, t2, 8 * (size_t) n, "%s scale64 n=%u", name, n);
			free(f1), free(f2), free(s1), free(s2), free(t1), free(t2);
		}
		free(u8), free(u4), free(q1), free(q2);
	}
	for (int it = 0; it < iters; it++)
	{
		uint32		nr = 1 + sift_rng_below(r, sift_rng_below(r, 4) ? 40 : 1500),
					total = 0;
		uint32	   *vals = xalloc(4 * nr + SIFT_COL_PAD),
				   *lens = xalloc(4 * nr + SIFT_COL_PAD);

		for (uint32 i = 0; i < nr; i++)
		{
			vals[i] = (uint32) sift_rng_next(r);
			total += lens[i] = 1 + sift_rng_below(r, sift_rng_below(r, 2) ? 3 : 70);
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

static void
test_strs(SiftRng *r, int scale)
{
	for (int it = 0; it < 40 * scale; it++)
	{
		uint32		n = it % 5 == 0 ? it / 5 : sift_rng_below(r, 3000);
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
			uint32		len = sift_rng_below(r, 4) ? sift_rng_below(r, 12) : sift_rng_below(r, 40);

			for (uint32 j = 0; j < len; j++)
				bytes[tot + j] = "abcdefgh"[sift_rng_below(r, it % 2 ? 3 : 8)];
			off[i + 1] = tot += len;
		}
		initStringInfo(&s);
		sift_cc_encode_strs(&s, off, bytes, n, &pol, NULL);
		blob = xalloc(s.len);
		memcpy(blob, s.data, s.len);
		sift_cc_decode_strs(CurrentMemoryContext, blob, s.len, n, &o2, &b2);
		CHECK(!memcmp(o2, off, 4 * (n + 1)) && !memcmp(b2, bytes, tot), "strs: round trip n=%u policy %s", n, policies[it % NPOL]);
		free(off), free(bytes), free(blob);
	}
}

#endif
