/*
 * tools/aggkern6/test_sbbf.h - the checks of the split-block Bloom filter (core/sbbf.c) and of the slots made of it, the granule bitmap and the
 * dictionary pair (agg/kern6_sbbf.c), against references written out here: the membership of a hash by its bits, no false negative over a million
 * keys at every level, the false positive rate against the expected one, the same bytes from every insert (atomic and concurrent included), and
 * the edges of the slots (an empty list, the range of keys, the bits past the last row). Included by test.c after its tables.
 */
#include <pthread.h>

#define SB_KEYS		1000000

/* the bits of a hash by the definition: word j gets the bit (low 32 bits * salt j) >> 27, in the block the high half selects */
static bool
sb_ref_member(const SiftSbbfBlock *blocks, uint32 nblocks, uint64 h)
{
	const uint32 blk = (uint32) (((uint64) (uint32) (h >> 32) * nblocks) >> 32);

	for (int j = 0; j < 8; j++)
	{
		const uint32 bit = ((uint32) h * SIFT_SBBF_SALT[j]) >> 27;

		if (!((blocks[blk].w[j] >> bit) & 1))
			return false;
	}
	return true;
}

static SiftSbbfBlock *
sb_new(uint32 nblocks)
{
	SiftSbbfBlock *b = xalloc((size_t) nblocks * SIFT_SBBF_BLOCK_BYTES);

	memset(b, 0, (size_t) nblocks * SIFT_SBBF_BLOCK_BYTES);
	return b;
}

typedef struct SbThread
{
	SiftSbbfBlock *blocks;
	uint32		nblocks;
	const uint64 *hashes;
	uint32		from,
				to;
} SbThread;

static void *
sb_thread(void *arg)
{
	const SbThread *t = arg;

	for (uint32 i = t->from; i < t->to; i++)
		sift_sbbf_insert_atomic(t->blocks, t->nblocks, t->hashes[i]);
	return NULL;
}

static void
sbbf_levels(void)
{
	const SiftSbbfOps *ops[4] = {&sift_sbbf_ops_scalar, sift_sbbf_ops_for(SIFT_SIMD_AVX2), sift_sbbf_ops_for(SIFT_SIMD_AVX512), NULL};
	const char *on[4] = {"scalar", "avx2", "avx512", NULL};
	const uint32 nblocks = sift_sbbf_blocks_for(SB_KEYS, 12);
	int64	   *keys = xalloc(SB_KEYS * 8),
			   *absent = xalloc(SB_KEYS * 8);
	uint64	   *hs = xalloc(SB_KEYS * 8);
	SiftSbbfBlock *ref = sb_new(nblocks);

	for (int i = 0; i < SB_KEYS; i++)
	{
		keys[i] = (int64) rnd();
		absent[i] = (int64) rnd();
		hs[i] = sift_aggk6_hash1((uint64) keys[i]);
		sift_sbbf_insert_scalar(ref, nblocks, hs[i]);
	}
	CHECK(sift_sbbf_blocks_for(0, 12) == 1 && sift_sbbf_blocks_for(1, 12) == 1 && sift_sbbf_blocks_for(1000, 12) == 47, "blocks_for sizes the filter\n");
	for (int l = 0; l < 3; l++)
	{
		SiftSbbfBlock *b;
		long		fp = 0;
		uint64		m[SIFT_VEC_WORDS];

		if (l && ops[l] == ops[l - 1])
			continue;
		b = sb_new(nblocks);
		for (int i = 0; i < SB_KEYS; i++)
			ops[l]->insert(b, nblocks, hs[i]);
		CHECK(!memcmp(b, ref, (size_t) nblocks * SIFT_SBBF_BLOCK_BYTES), "%s: insert sets the bits of the scalar insert\n", on[l]);
		for (int from = 0; from < SB_KEYS; from += SIFT_VEC_LEN)
		{
			const uint16 len = Min(SIFT_VEC_LEN, SB_KEYS - from);

			memset(m, 0, sizeof(m));
			for (int i = 0; i < len; i++)
				m[i >> 6] |= UINT64CONST(1) << (i & 63);
			CHECK(sift_sbbf_probe_batch_at(ops[l], b, nblocks, hs + from, m, len) == len, "%s: a hash in the filter is found (batch)\n", on[l]);
			memset(m, 0xFF, sizeof(m));
			CHECK(sift_aggkern6_for((SiftSimdLevel) l)->sbbf_probe_vec_i64(b, nblocks, INT64_MIN, INT64_MAX, keys + from, m, len) == len,
				  "%s: a key in the filter is found (vec_i64)\n", on[l]);
			memset(m, 0xFF, sizeof(m));
			fp += sift_aggkern6_for((SiftSimdLevel) l)->sbbf_probe_vec_i64(b, nblocks, INT64_MIN, INT64_MAX, absent + from, m, len);
		}
		for (int i = 0; i < 20000; i++)
		{
			const uint64 h = rnd();

			CHECK(sb_ref_member(b, nblocks, h) == sift_sbbf_probe_scalar(b, nblocks, h), "%s: probe_one is the membership by the bits\n", on[l]);
		}
		for (int i = 0; i < 20000; i++)
		{
			uint64		hh[64],
						w;

			for (int j = 0; j < 64; j++)
				hh[j] = rnd() & 1 ? hs[rnd() % SB_KEYS] : rnd();
			w = ops[l]->probe_word(b, nblocks, hh, ~UINT64CONST(0));
			for (int j = 0; j < 64; j++)
				CHECK(((w >> j) & 1) == sb_ref_member(b, nblocks, hh[j]), "%s: probe_word bit %d differs from the membership\n", on[l], j);
		}
		{
			const double expect = sift_sbbf_fpr(SB_KEYS, nblocks),
						seen = (double) fp / SB_KEYS;

			CHECK(expect > 0.003 && expect < 0.007, "%s: the expected rate at 12 bits a key is about 0.4 to 0.5 percent, got %g\n", on[l], expect);
			CHECK(seen > expect * 0.85 && seen < expect * 1.15, "%s: the false positive rate %g is not the expected %g\n", on[l], seen, expect);
		}
		free(b);
	}
	{
		const uint32 nb10 = sift_sbbf_blocks_for(SB_KEYS, 10);
		const double fpr = sift_sbbf_fpr(SB_KEYS, nb10);

		CHECK(fpr > 0.009 && fpr < 0.015, "the expected rate at 10 bits a key is about 1.2 percent, got %g\n", fpr);
		CHECK(sift_sbbf_fpr(0, 8) == 0 && sift_sbbf_fpr(1, 1) < 0.01 && sift_sbbf_fpr((uint64) 1 << 30, 4) > 0.999, "the rate of an empty, a lone and a saturated filter\n");
	}
	{
		SiftSbbfBlock *b = sb_new(nblocks);
		pthread_t	th[4];
		SbThread	arg[4];

		for (int t = 0; t < 4; t++)
		{
			arg[t] = (SbThread) {b, nblocks, hs, SB_KEYS / 4 * t, SB_KEYS / 4 * (t + 1)};
			pthread_create(&th[t], NULL, sb_thread, &arg[t]);
		}
		for (int t = 0; t < 4; t++)
			pthread_join(th[t], NULL);
		CHECK(!memcmp(b, ref, (size_t) nblocks * SIFT_SBBF_BLOCK_BYTES), "four builders of one filter by atomic OR set the bits of one\n");
		free(b);
	}
	free(keys);
	free(absent);
	free(hs);
	free(ref);
}

static void
sbbf_edges(void)
{
	for (int t = 0; t < ntabs; t++)
	{
		const SiftAggKernelsV6 *k = tabs[t];
		SiftSbbfBlock *b = sb_new(1);
		const uint64 h[3] = {0, ~UINT64CONST(0), 0x8000000080000000};
		const int64 key[5] = {5, -3, INT64_MIN, INT64_MAX, 9};
		uint64		m[1];

		CHECK(!k->sbbf_probe_any(b, 1, h, 0) && k->sbbf_probe_all(b, 1, h, 0), "%s: no hash is no match for any and a match for all\n", names[t]);
		CHECK(!k->sbbf_probe_any(b, 1, h, 3), "%s: an empty filter holds nothing\n", names[t]);
		sift_sbbf_insert_scalar(b, 1, h[0]);
		sift_sbbf_insert_scalar(b, 1, h[1]);
		CHECK(k->sbbf_probe_any(b, 1, h, 3) && k->sbbf_probe_all(b, 1, h, 3) == sift_sbbf_probe_scalar(b, 1, h[2]),
			  "%s: any and all of hashes 0 and ~0 in a one block filter\n", names[t]);
		CHECK(k->sbbf_probe_all(b, 1, h, 2), "%s: the two inserted hashes are all found\n", names[t]);
		m[0] = ~UINT64CONST(0);
		CHECK(k->sbbf_probe_vec_hash(b, 1, h, m, 3) <= 3 && (m[0] >> 3) == 0, "%s: the bits past the last row are clear\n", names[t]);
		m[0] = 0x1F;
		sift_sbbf_insert_scalar(b, 1, sift_aggk6_hash1((uint64) key[0]));
		sift_sbbf_insert_scalar(b, 1, sift_aggk6_hash1((uint64) key[2]));
		sift_sbbf_insert_scalar(b, 1, sift_aggk6_hash1((uint64) key[3]));
		CHECK(k->sbbf_probe_vec_i64(b, 1, INT64_MIN, INT64_MAX, key, m, 5) >= 3 && (m[0] & 0xD) == 0xD, "%s: the three keys in the filter are found\n", names[t]);
		m[0] = 0x1F;
		CHECK(k->sbbf_probe_vec_i64(b, 1, 0, 5, key, m, 5) == 1 && m[0] == 1, "%s: a range keeps only the keys in it\n", names[t]);
		m[0] = 0x1F;
		CHECK(k->sbbf_probe_vec_i64(b, 1, 6, 5, key, m, 5) == 0 && m[0] == 0, "%s: an empty range keeps nothing\n", names[t]);
		m[0] = 0;
		CHECK(k->sbbf_probe_vec_i64(b, 1, INT64_MIN, INT64_MAX, key, m, 5) == 0, "%s: no live row stays none\n", names[t]);
		CHECK(k->sbbf_probe_vec_hash(b, 1, h, m, 0) == 0, "%s: no row is no match\n", names[t]);
		free(b);
	}
}

/* mask_and_gran and dict_pair_gid against the loop over the bits and the rows */
static void
sbbf_granule(void)
{
	for (int t = 0; t < ntabs; t++)
		for (int rep = 0; rep < 200; rep++)
		{
			const SiftAggKernelsV6 *k = tabs[t];
			const int	n = rep < 4 ? SIFT_GRAN_ROWS - rep : 1 + (int) (rnd() % SIFT_GRAN_ROWS);
			const int	nw = AK6_NW(n);
			uint64	   *a = gen_mask(n, rnd() % 5),
					   *b = gen_mask(n, rnd() % 5),
					   *d = xalloc(nw * 8 + 1);
			int			pc = 0;
			bool		ok = true;

			for (int i = 0; i < n; i++)
				pc += rbit(a, i) & rbit(b, i);
			CHECK(k->mask_and_gran(d, a, b, n) == pc, "%s: mask_and_gran popcount, n = %d\n", names[t], n);
			for (int i = 0; i < n; i++)
				ok = ok && rbit(d, i) == (rbit(a, i) & rbit(b, i));
			CHECK(ok && (n % 64 == 0 || (d[nw - 1] >> (n % 64)) == 0), "%s: mask_and_gran bits, n = %d\n", names[t], n);
			free(a);
			free(b);
			free(d);
		}
	for (int t = 0; t < ntabs; t++)
		for (int rep = 0; rep < 200; rep++)
		{
			const SiftAggKernelsV6 *k = tabs[t];
			const int	n = (int) (rnd() % (SIFT_VEC_LEN + 1));
			const uint32 dom1 = 1 + (uint32) (rnd() % 5000),
						null0 = (uint32) rnd(),
						null1 = (uint32) (rnd() % (dom1 + 1));
			uint32	   *c0 = xalloc(n * 4 + 1),
					   *c1 = xalloc(n * 4 + 1),
					   *g = xalloc(n * 4 + 1);
			uint64	   *v0 = rnd() & 1 ? gen_mask(n, 2) : NULL,
					   *v1 = rnd() & 1 ? gen_mask(n, 2) : NULL,
					   *act = gen_mask(n, rnd() % 5);
			uint16	   *sel = xalloc((n + SIFT_SEL_SLACK) * 2);
			const int	nsel = sift_aggkern6_scalar.mask_to_sel16(sel, act, n);
			bool		ok = true;

			for (int i = 0; i < n; i++)
			{
				c0[i] = (uint32) rnd();
				c1[i] = (uint32) (rnd() % (dom1 + 1));
			}
			for (int pass = 0; pass < 2; pass++)
			{
				memset(g, 0x5A, n * 4);
				k->dict_pair_gid(g, c0, v0, null0, c1, v1, null1, dom1, pass ? sel : NULL, pass ? nsel : n);
				for (int r = 0; r < n; r++)
				{
					const bool	live = !pass || rbit(act, r);
					const uint32 x0 = !v0 || rbit(v0, r) ? c0[r] : null0,
								x1 = !v1 || rbit(v1, r) ? c1[r] : null1;

					ok = ok && (live ? g[r] == x0 * dom1 + x1 : g[r] == 0x5A5A5A5A);
				}
			}
			CHECK(ok, "%s: dict_pair_gid differs from the loop, n = %d\n", names[t], n);
			free(c0);
			free(c1);
			free(g);
			free(v0);
			free(v1);
			free(act);
			free(sel);
		}
}

/* a filter at every byte offset (a skipping index payload has no alignment): every level and the entry points give the bits and the answers of the aligned one */
static void
sbbf_unaligned(void)
{
	enum {NB = 37, NH = 2000};
	const SiftSbbfOps *ops[3] = {&sift_sbbf_ops_scalar, sift_sbbf_ops_for(SIFT_SIMD_AVX2), sift_sbbf_ops_for(SIFT_SIMD_AVX512)};
	SiftSbbfBlock *ref = sb_new(NB);
	uint64		hs[NH],
				absent[NH];
	uint8	   *raw = xalloc((size_t) NB * SIFT_SBBF_BLOCK_BYTES + 64);

	for (int i = 0; i < NH; i++)
	{
		hs[i] = sift_aggk6_hash1(rnd());
		absent[i] = rnd();
		sift_sbbf_insert_scalar(ref, NB, hs[i]);
	}
	for (int off = 1; off < 32; off++)
		for (int l = 0; l < 3; l++)
		{
			uint8	   *p = raw + off;
			uint64		m[SIFT_VEC_WORDS];

			memset(p, 0, (size_t) NB * SIFT_SBBF_BLOCK_BYTES);
			for (int i = 0; i < NH; i++)
				ops[l]->insert(p, NB, hs[i]);
			CHECK(!memcmp(p, ref, (size_t) NB * SIFT_SBBF_BLOCK_BYTES), "level %d, offset %d: an unaligned insert sets the bits of the aligned one\n", l, off);
			for (int i = 0; i < NH; i += 64)
			{
				const uint64 w = ops[l]->probe_word(p, NB, absent + i, NH - i >= 64 ? ~UINT64CONST(0) : AK6_LOW(NH - i));

				for (int j = 0; j < 64 && i + j < NH; j++)
					CHECK(((w >> j) & 1) == sift_sbbf_probe_scalar(ref, NB, absent[i + j]), "level %d, offset %d: an unaligned probe differs\n", l, off);
			}
			memset(m, 0xFF, sizeof(m));
			CHECK(sift_sbbf_probe_batch_at(ops[l], p, NB, hs, m, NH) == NH, "level %d, offset %d: the keys inserted are found\n", l, off);
		}
	{
		uint8	   *p = raw + 3;

		memset(p, 0, (size_t) NB * SIFT_SBBF_BLOCK_BYTES);
		for (int i = 0; i < NH; i++)
			sift_sbbf_insert(p, NB, hs[i]);
		CHECK(!memcmp(p, ref, (size_t) NB * SIFT_SBBF_BLOCK_BYTES), "sift_sbbf_insert on an unaligned filter sets the aligned bits\n");
		CHECK(sift_sbbf_probe_all(p, NB, hs, NH) && sift_sbbf_probe_any(p, NB, hs, NH), "the keys inserted are all found by the entry points\n");
		for (int i = 0; i < NH; i++)
			CHECK(sift_sbbf_probe(p, NB, absent[i]) == sift_sbbf_probe_scalar(ref, NB, absent[i]), "the active level probes an unaligned filter like the scalar one\n");
		CHECK(sift_sbbf_probe_any(p, NB, hs, 0) == false && sift_sbbf_probe_all(p, NB, hs, 0) == true, "no hash is no match for any and a match for all\n");
	}
	free(raw);
	free(ref);
}

static void
sbbf_checks(void)
{
	sbbf_unaligned();
	sbbf_levels();
	sbbf_edges();
	sbbf_granule();
}
