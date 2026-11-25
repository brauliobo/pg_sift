/*
 * agg/kern6/cases_sbbf.h - random cases of the slots of the granule bitmap, the dictionary pair and the split-block Bloom filter (agg/kern6/sbbf.c),
 * included by agg/kern6/cases.h (which has the generator and the macros it uses): ak6c_equiv_sbbf counts the slots of table b whose outputs differ bit for bit from those of table a on the case of
 * a seed. The filter of a case holds a random number of keys in a random number of blocks, from a nearly empty one to a saturated one, so that hits
 * and misses both occur; what the keys must give (no miss for a key in the filter) is checked against the reference of tools/aggkern6/test.c.
 */
#ifndef SIFT_AGGKERN6_CASES_SBBF_H
#define SIFT_AGGKERN6_CASES_SBBF_H

#define AK6C_SB_POOL	256

static inline int
ak6c_equiv_sbbf(const SiftAggKernelsV6 *a, const SiftAggKernelsV6 *b, uint64 seed, int n)
{
	Ak6cRng		rng = {seed * UINT64CONST(0xD1B54A32D192ED03) | 1},
			   *r = &rng;
	const SiftAggKernelsV6 *k[2] = {a, b};
	const uint32 nblocks = 1 + (ak6c_rnd(r) % 4 ? ak6c_rnd(r) % 40 : ak6c_rnd(r) % 400);
	const uint32 ninsert = ak6c_rnd(r) % (nblocks * 24 + 1);
	SiftSbbfBlock *blocks = AK6C_ALLOC((size_t) nblocks * SIFT_SBBF_BLOCK_BYTES);
	int64	   *pool = AK6C_ALLOC(AK6C_SB_POOL * 8);
	uint64	   *ph = AK6C_ALLOC(AK6C_SB_POOL * 8);
	int			bad = 0;

	memset(blocks, 0, (size_t) nblocks * SIFT_SBBF_BLOCK_BYTES);
	for (int i = 0; i < AK6C_SB_POOL; i++)
	{
		pool[i] = ak6c_i64(r);
		ph[i] = sift_aggk6_hash1((uint64) pool[i]);
	}
	for (uint32 i = 0; i < ninsert; i++)
		sift_sbbf_insert_scalar(blocks, nblocks, ph[i % AK6C_SB_POOL] ^ (i >= AK6C_SB_POOL ? sift_aggk6_hash1(i) : 0));
	for (uint32 i = 0; i < Min(ninsert, AK6C_SB_POOL); i++)
		sift_sbbf_insert_scalar(blocks, nblocks, ph[i]);

	{
		const uint32 nh = ak6c_rnd(r) % 101;
		uint64	   *hs = AK6C_ALLOC(nh * 8 + 1);
		bool		ok = true;
		bool		res[2][2];

		for (uint32 i = 0; i < nh; i++)
			hs[i] = ak6c_rnd(r) % 3 ? ph[ak6c_rnd(r) % AK6C_SB_POOL] : ak6c_rnd(r);
		for (int t = 0; t < 2; t++)
		{
			res[t][0] = k[t]->sbbf_probe_any(blocks, nblocks, hs, nh);
			res[t][1] = k[t]->sbbf_probe_all(blocks, nblocks, hs, nh);
		}
		ok = res[0][0] == res[1][0] && res[0][1] == res[1][1];
		bad += !ok;
		AK6C_FREE(hs);
	}
	{
		const int	nw = AK6_NW(n);
		uint64	   *hv = AK6C_ALLOC(n * 8 + 1);
		int64	   *kv = AK6C_ALLOC(n * 8 + 1);
		uint64	   *in = ak6c_mask(r, n),
				   *o[2] = {AK6C_ALLOC(nw * 8 + 1), AK6C_ALLOC(nw * 8 + 1)};
		uint16		pc[2];
		bool		ok = true;
		int64		lo = ak6c_i64(r),
					hi = ak6c_i64(r);

		for (int i = 0; i < n; i++)
		{
			const int	p = ak6c_rnd(r) % AK6C_SB_POOL;
			const bool	known = ak6c_rnd(r) % 2;

			kv[i] = known ? pool[p] : ak6c_i64(r);
			hv[i] = known ? ph[p] : ak6c_rnd(r);
		}
		AK6C_BOTH(memcpy(o[t_], in, nw * 8); pc[t_] = k[t_]->sbbf_probe_vec_hash(blocks, nblocks, hv, o[t_], n));
		ok = pc[0] == pc[1];
		AK6C_DIFF(ok, o[0], o[1], nw * 8);
		bad += !ok;
		ok = true;
		for (int pass = 0; pass < 3; pass++)
		{
			if (pass == 1)
			{
				lo = INT64_MIN;
				hi = INT64_MAX;
			}
			if (pass == 2)
			{
				const int64 t = lo;

				lo = hi;
				hi = t;
			}
			AK6C_BOTH(memcpy(o[t_], in, nw * 8); pc[t_] = k[t_]->sbbf_probe_vec_i64(blocks, nblocks, lo, hi, kv, o[t_], n));
			ok = ok && pc[0] == pc[1];
			AK6C_DIFF(ok, o[0], o[1], nw * 8);
		}
		bad += !ok;
		AK6C_FREE(hv);
		AK6C_FREE(kv);
		AK6C_FREE(in);
		AK6C_FREE(o[0]);
		AK6C_FREE(o[1]);
	}
	{
		const int	ng = ak6c_rnd(r) % 3 ? 1 + (int) (ak6c_rnd(r) % SIFT_GRAN_ROWS) : SIFT_GRAN_ROWS - (int) (ak6c_rnd(r) % 3);
		const int	nw = AK6_NW(ng);
		uint64	   *x = ak6c_mask(r, ng),
				   *y = ak6c_mask(r, ng),
				   *d[2] = {AK6C_ALLOC(nw * 8 + 1), AK6C_ALLOC(nw * 8 + 1)};
		uint16		pc[2];
		bool		ok;

		AK6C_BOTH(memset(d[t_], 0xA5, nw * 8); pc[t_] = k[t_]->mask_and_gran(d[t_], x, y, ng));
		ok = pc[0] == pc[1];
		AK6C_DIFF(ok, d[0], d[1], nw * 8);
		AK6C_BOTH(memcpy(d[t_], x, nw * 8); pc[t_] = k[t_]->mask_and_gran(d[t_], d[t_], y, ng));
		ok = ok && pc[0] == pc[1];
		AK6C_DIFF(ok, d[0], d[1], nw * 8);
		bad += !ok;
		AK6C_FREE(x);
		AK6C_FREE(y);
		AK6C_FREE(d[0]);
		AK6C_FREE(d[1]);
	}
	{
		const uint32 dom1 = 1 + ak6c_rnd(r) % 100000;
		const uint32 null0 = (uint32) ak6c_rnd(r),
					null1 = ak6c_rnd(r) % (dom1 + 1);
		uint32	   *c0 = AK6C_ALLOC(n * 4 + 1),
				   *c1 = AK6C_ALLOC(n * 4 + 1),
				   *g[2] = {AK6C_ALLOC(n * 4 + 1), AK6C_ALLOC(n * 4 + 1)};
		uint64	   *v0 = ak6c_rnd(r) % 3 ? ak6c_mask(r, n) : NULL,
				   *v1 = ak6c_rnd(r) % 3 ? ak6c_mask(r, n) : NULL,
				   *act = ak6c_mask(r, n);
		uint16	   *sel = AK6C_ALLOC((n + SIFT_SEL_SLACK) * 2);
		const int	nsel = sift_aggkern6_scalar.mask_to_sel16(sel, act, n);
		bool		ok = true;

		for (int i = 0; i < n; i++)
		{
			c0[i] = (uint32) ak6c_rnd(r);
			c1[i] = ak6c_rnd(r) % (dom1 + 1);
		}
		for (int pass = 0; pass < 2; pass++)
		{
			AK6C_BOTH(memset(g[t_], 0xA5, n * 4); k[t_]->dict_pair_gid(g[t_], c0, v0, null0, c1, v1, null1, dom1, pass ? sel : NULL, pass ? nsel : n));
			AK6C_DIFF(ok, g[0], g[1], n * 4);
		}
		bad += !ok;
		AK6C_FREE(c0);
		AK6C_FREE(c1);
		AK6C_FREE(g[0]);
		AK6C_FREE(g[1]);
		if (v0)
			AK6C_FREE(v0);
		if (v1)
			AK6C_FREE(v1);
		AK6C_FREE(act);
		AK6C_FREE(sel);
	}
	AK6C_FREE(blocks);
	AK6C_FREE(pool);
	AK6C_FREE(ph);
	return bad;
}

#endif							/* SIFT_AGGKERN6_CASES_SBBF_H */
