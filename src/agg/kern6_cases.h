/*
 * agg/kern6_cases.h - random cases of the 2048-row operators, shared by the in-backend test (agg/kern6_test.c) and the standalone harness
 * (tools/aggkern6). The includer defines AK6C_ALLOC(n) and AK6C_FREE(p) (inputs are allocated with exactly the bytes a kernel may read, so a
 * sanitizer sees an access past them). ak6c_equiv runs every slot of two tables on one random case and counts the slots whose outputs differ
 * bit for bit.
 */
#ifndef SIFT_AGGKERN6_CASES_H
#define SIFT_AGGKERN6_CASES_H

#include "agg/kern6_int.h"

typedef struct Ak6cRng
{
	uint64		s;
} Ak6cRng;

static inline uint64
ak6c_rnd(Ak6cRng *r)
{
	r->s ^= r->s << 13;
	r->s ^= r->s >> 7;
	r->s ^= r->s << 17;
	return r->s * UINT64CONST(0x2545F4914F6CDD1D);
}

static const int64 ak6c_edge[] = {0, 1, -1, INT64_MAX, INT64_MIN, INT64_MAX - 1, INT64_MIN + 1, 0xFFFFFFFFLL, 0x100000000LL, -0x100000000LL};
static const uint64 ak6c_fedge[] = {0, UINT64CONST(0x8000000000000000), UINT64CONST(0x7FF0000000000000), UINT64CONST(0xFFF0000000000000),
	UINT64CONST(0x7FF8000000000000), UINT64CONST(0x7FF8000000000001), UINT64CONST(0xFFF8000000000123), UINT64CONST(0x7FEFFFFFFFFFFFFF)};

static inline int64
ak6c_i64(Ak6cRng *r)
{
	return ak6c_rnd(r) & 1 ? ak6c_edge[ak6c_rnd(r) % lengthof(ak6c_edge)] : (int64) ak6c_rnd(r);
}

static inline double
ak6c_f64(Ak6cRng *r)
{
	double		d;

	if (ak6c_rnd(r) % 6)
		return (double) ((int64) (ak6c_rnd(r) % 2000001) - 1000000) / 64.0;
	memcpy(&d, &ak6c_fedge[ak6c_rnd(r) % lengthof(ak6c_fedge)], 8);
	return d;
}

/* A bitmap of n bits, the bits >= n zero: all, none, random, sparse or dense */
static inline uint64 *
ak6c_mask(Ak6cRng *r, int n)
{
	const int	nw = AK6_NW(n);
	const int	kind = ak6c_rnd(r) % 5;
	uint64	   *m = AK6C_ALLOC(nw * 8 + 1);

	for (int w = 0; w < nw; w++)
		m[w] = kind == 0 ? ~UINT64CONST(0) : kind == 1 ? 0 : kind == 2 ? ak6c_rnd(r) :
			kind == 3 ? ak6c_rnd(r) & ak6c_rnd(r) & ak6c_rnd(r) & ak6c_rnd(r) : ak6c_rnd(r) | ak6c_rnd(r) | ak6c_rnd(r);
	if (nw)
		m[nw - 1] &= AK6_TAIL(n);
	return m;
}

#define AK6C_DIFF(ok, a, b, len) ((ok) = (ok) && !memcmp(a, b, len))

/* The number of slots of table b whose outputs differ from table a on the case of this seed (a length n, or a random one when n < 0) */
static inline int
ak6c_equiv_base(const SiftAggKernelsV6 *a, const SiftAggKernelsV6 *b, uint64 seed, int n)
{
	Ak6cRng		rng = {seed * UINT64CONST(0x9E3779B97F4A7C15) | 1},
			   *r = &rng;
	int			bad = 0,
				G = 1 + ak6c_rnd(r) % 300;
	const SiftAggKernelsV6 *k[2] = {a, b};
	uint64	   *m,
			   *del,
			   *h[2];
	uint64	   *hin;
	uint16	   *sel[2],
				cnt[2];
	int32	   *v32;
	int64	   *v64,
			   *cn[2],
			   *s32[2],
			   *s64[2],
			   *mn[2],
			   *mx[2];
	SiftI128   *v128,
			   *w1[2],
			   *w2[2];
	double	   *vf,
			   *f[2];
	uint32	   *gid;
	bool		ok;
	SiftI128	init = {ak6c_rnd(r), ak6c_i64(r)};

	if (n < 0)
	{
		const int	edge = (int) (ak6c_rnd(r) % 33) * 64 + (int) (ak6c_rnd(r) % 3) - 1;

		n = ak6c_rnd(r) % 2 ? (int) (ak6c_rnd(r) % (SIFT_VEC_LEN + 1)) : Min(Max(edge, 0), SIFT_VEC_LEN);
	}
	m = ak6c_mask(r, n);
	del = ak6c_mask(r, n);
	v32 = AK6C_ALLOC(n * 4 + 1);
	v64 = AK6C_ALLOC(n * 8 + 1);
	v128 = AK6C_ALLOC(n * 16 + 1);
	vf = AK6C_ALLOC(n * 8 + 1);
	gid = AK6C_ALLOC(n * 4 + 1);
	hin = AK6C_ALLOC(n * 8 + 1);
	for (int i = 0; i < n; i++)
	{
		v32[i] = (int32) ak6c_i64(r);
		v64[i] = ak6c_i64(r);
		v128[i].lo = (uint64) ak6c_i64(r);
		v128[i].hi = ak6c_i64(r);
		vf[i] = ak6c_f64(r);
		gid[i] = ak6c_rnd(r) % G;
		hin[i] = ak6c_rnd(r);
	}
	for (int t = 0; t < 2; t++)
	{
		sel[t] = AK6C_ALLOC((n + SIFT_SEL_SLACK) * 2);
		h[t] = AK6C_ALLOC(n * 8 + 1);
		cn[t] = AK6C_ALLOC(G * 8);
		s32[t] = AK6C_ALLOC(G * 8);
		s64[t] = AK6C_ALLOC(G * 8);
		mn[t] = AK6C_ALLOC(G * 8);
		mx[t] = AK6C_ALLOC(G * 8);
		w1[t] = AK6C_ALLOC(G * 16);
		w2[t] = AK6C_ALLOC(G * 16);
		f[t] = AK6C_ALLOC(G * 8);
	}
	{
		uint64	   *d[2] = {AK6C_ALLOC(AK6_NW(n) * 8 + 1), AK6C_ALLOC(AK6_NW(n) * 8 + 1)};
		uint16		pc[2];
		uint64	   *an[2] = {AK6C_ALLOC(AK6_NW(n) * 8 + 1), AK6C_ALLOC(AK6_NW(n) * 8 + 1)};
		uint16		pa[2];

		for (int t = 0; t < 2; t++)
		{
			memcpy(d[t], m, AK6_NW(n) * 8);
			pc[t] = k[t]->mask_andn_del(d[t], del, n);
			pa[t] = k[t]->mask_and(an[t], m, del, n);
			cnt[t] = k[t]->mask_to_sel16(sel[t], del, n);
		}
		ok = pc[0] == pc[1] && pa[0] == pa[1] && cnt[0] == cnt[1];
		AK6C_DIFF(ok, d[0], d[1], AK6_NW(n) * 8);
		AK6C_DIFF(ok, an[0], an[1], AK6_NW(n) * 8);
		AK6C_DIFF(ok, sel[0], sel[1], cnt[0] * 2);
		bad += !ok;
		for (int t = 0; t < 2; t++)
		{
			AK6C_FREE(d[t]);
			AK6C_FREE(an[t]);
		}
	}
	/* the selection of the rows of m feeds the sel forms below */
	cnt[0] = sift_aggkern6_scalar.mask_to_sel16(sel[0], m, n);
	for (int pass = 0; pass < 2; pass++)
	{
		const uint16 *s = pass ? sel[0] : NULL;
		const int	nr = pass ? cnt[0] : n;
		uint64	   *hh[2] = {h[0], h[1]};

		ok = true;
		for (int t = 0; t < 2; t++)
		{
			const bool	mix = pass;

			memcpy(hh[t], hin, n * 8);
			k[t]->hash_i64(hh[t], v64, s, nr, mix);
		}
		AK6C_DIFF(ok, hh[0], hh[1], nr * 8);
		for (int t = 0; t < 2; t++)
		{
			memcpy(hh[t], hin, n * 8);
			k[t]->hash_i32(hh[t], v32, s, nr, !pass);
		}
		AK6C_DIFF(ok, hh[0], hh[1], nr * 8);
		bad += !ok;

		ok = true;
		for (int t = 0; t < 2; t++)
		{
			memset(cn[t], 0, G * 8);
			memset(s32[t], 0, G * 8);
			memset(s64[t], 0, G * 8);
			memset(w1[t], 0, G * 16);
			memset(w2[t], 0, G * 16);
			memset(f[t], 0, G * 8);
			for (int g = 0; g < G; g++)
			{
				mn[t][g] = INT64_MAX;
				mx[t][g] = INT64_MIN;
			}
			k[t]->count_gid(cn[t], gid, s, nr);
			k[t]->sum_gid_i32(s32[t], gid, v32, s, nr);
			k[t]->sum_gid_i64(s64[t], gid, v64, s, nr);
			k[t]->sum_gid_i64_to_i128(w1[t], gid, v64, s, nr);
			k[t]->sum_gid_i128(w2[t], gid, v128, s, nr);
			k[t]->sum_gid_f64_fast(f[t], gid, vf, s, nr);
			k[t]->minmax_gid_i64(mn[t], mx[t], gid, v64, s, nr);
		}
		AK6C_DIFF(ok, cn[0], cn[1], G * 8);
		AK6C_DIFF(ok, s32[0], s32[1], G * 8);
		AK6C_DIFF(ok, s64[0], s64[1], G * 8);
		AK6C_DIFF(ok, w1[0], w1[1], G * 16);
		AK6C_DIFF(ok, w2[0], w2[1], G * 16);
		AK6C_DIFF(ok, f[0], f[1], G * 8);
		AK6C_DIFF(ok, mn[0], mn[1], G * 8);
		AK6C_DIFF(ok, mx[0], mx[1], G * 8);
		bad += !ok;
	}
	for (int pass = 0; pass < 2; pass++)
	{
		const uint64 *mk = pass ? m : NULL;
		int64		r64[2] = {-1, -1},
					c64[2];
		bool		fit[2];
		SiftI128	r1[2],
					r2[2],
					mi[2] = {{1, 1}, {1, 1}},
					ma[2] = {{1, 1}, {1, 1}};
		double		fs[2];
		bool		any[2];

		ok = true;
		for (int t = 0; t < 2; t++)
		{
			ok = ok && k[t]->sum_i32(v32, mk, n) == k[0]->sum_i32(v32, mk, n);
			r1[t] = k[t]->sum_i64_to_i128(v64, mk, n, init);
			r2[t] = k[t]->sum_i128(v128, mk, n, init);
			fs[t] = k[t]->sum_f64_fast(vf, mk, n, 1.5);
			c64[t] = 99;
			r64[t] = c64[t];
			fit[t] = k[t]->sum_i64_checked(v64, mk, n, &r64[t]);
			any[t] = k[t]->minmax_i128(&mi[t], &ma[t], v128, mk, n);
		}
		ok = ok && fit[0] == fit[1] && r64[0] == r64[1] && any[0] == any[1];
		AK6C_DIFF(ok, &r1[0], &r1[1], 16);
		AK6C_DIFF(ok, &r2[0], &r2[1], 16);
		AK6C_DIFF(ok, &fs[0], &fs[1], 8);
		AK6C_DIFF(ok, &mi[0], &mi[1], 16);
		AK6C_DIFF(ok, &ma[0], &ma[1], 16);
		bad += !ok;
	}
	for (int t = 0; t < 2; t++)
	{
		AK6C_FREE(sel[t]);
		AK6C_FREE(h[t]);
		AK6C_FREE(cn[t]);
		AK6C_FREE(s32[t]);
		AK6C_FREE(s64[t]);
		AK6C_FREE(mn[t]);
		AK6C_FREE(mx[t]);
		AK6C_FREE(w1[t]);
		AK6C_FREE(w2[t]);
		AK6C_FREE(f[t]);
	}
	AK6C_FREE(m);
	AK6C_FREE(del);
	AK6C_FREE(v32);
	AK6C_FREE(v64);
	AK6C_FREE(v128);
	AK6C_FREE(vf);
	AK6C_FREE(gid);
	AK6C_FREE(hin);
	return bad;
}


/* ---- the slots of the predicates, the wide hashes, the minimums and maximums and the arithmetic ---- */

/* A view of the bytes b[0..len): inline up to 12 bytes (prefix, tail, zero padded), else the first four bytes and a pointer to b */
static inline SiftStrView
ak6c_view(const uint8 *b, uint32 len)
{
	SiftStrView v;

	memset(&v, 0, sizeof(v));
	v.len = len;
	memcpy(v.prefix, b, Min(len, 4));
	if (len <= 12)
		memcpy(v.u.tail, b + Min(len, 4), len > 4 ? len - 4 : 0);
	else
		v.u.ptr = (const char *) b;
	return v;
}

#define AK6C_NSTR	9

/* The strings of the cases: distinct, of lengths around the 4 and 12 byte limits */
static inline uint32
ak6c_str_len(int id)
{
	static const uint32 len[AK6C_NSTR] = {0, 1, 4, 5, 12, 13, 13, 31, 64};

	return len[id];
}

static inline void
ak6c_str_bytes(int id, uint8 *b)
{
	for (uint32 i = 0; i < ak6c_str_len(id); i++)
		b[i] = (uint8) ('a' + (id == 6 && i == 12 ? 1 : 0) + (i % 3));
}

#define AK6C_BOTH(f) do { for (int t_ = 0; t_ < 2; t_++) { f; } } while (0)

static inline int
ak6c_equiv_new(const SiftAggKernelsV6 *a, const SiftAggKernelsV6 *b, uint64 seed, int n)
{
	Ak6cRng		rng = {(seed + 77) * UINT64CONST(0xD6E8FEB86659FD93) | 1},
			   *r = &rng;
	const SiftAggKernelsV6 *k[2] = {a, b};
	const int	nw = AK6_NW(n);
	int			bad = 0;
	int32	   *v32 = AK6C_ALLOC(n * 4 + 1),
			   *w32 = AK6C_ALLOC(n * 4 + 1);
	int64	   *v64 = AK6C_ALLOC(n * 8 + 1),
			   *w64 = AK6C_ALLOC(n * 8 + 1);
	SiftI128   *v128 = AK6C_ALLOC(n * 16 + 1);
	double	   *vf = AK6C_ALLOC(n * 8 + 1),
			   *wf = AK6C_ALLOC(n * 8 + 1);
	uint32	   *vu = AK6C_ALLOC(n * 4 + 1),
			   *gid = AK6C_ALLOC(n * 4 + 1);
	uint8	   *strb[AK6C_NSTR];
	SiftStrView *sv = AK6C_ALLOC(n * 16 + 1);
	uint64	   *m = ak6c_mask(r, n),
			   *vm = ak6c_mask(r, n),
			   *o[2] = {AK6C_ALLOC(nw * 8 + 1), AK6C_ALLOC(nw * 8 + 1)},
			   *h[2] = {AK6C_ALLOC(n * 8 + 1), AK6C_ALLOC(n * 8 + 1)},
			   *hin = AK6C_ALLOC(n * 8 + 1);
	uint16	   *sel = AK6C_ALLOC((n + SIFT_SEL_SLACK) * 2);
	const uint32 nlut = ak6c_rnd(r) % 300;
	const uint32 base = (uint32) ak6c_rnd(r) >> (ak6c_rnd(r) % 20);
	uint8	   *lut = AK6C_ALLOC(nlut + SIFT_AK6_LUT_PAD);
	int64		set[20];
	const int	nset = ak6c_rnd(r) % 21;
	int			nsel;

	for (int i = 0; i < AK6C_NSTR; i++)
	{
		strb[i] = AK6C_ALLOC(ak6c_str_len(i) + 1);
		ak6c_str_bytes(i, strb[i]);
	}
	for (int i = 0; i < n; i++)
	{
		const bool	same = ak6c_rnd(r) % 3 == 0;

		v32[i] = (int32) ak6c_i64(r);
		w32[i] = same ? v32[i] : (int32) ak6c_i64(r);
		v64[i] = ak6c_i64(r);
		w64[i] = same ? v64[i] : ak6c_i64(r);
		v128[i].lo = (uint64) ak6c_i64(r);
		v128[i].hi = ak6c_i64(r) >> (ak6c_rnd(r) % 3 * 30);
		if (same && ak6c_rnd(r) % 2)
			v128[i] = v128[ak6c_rnd(r) % (i + 1)];
		vf[i] = ak6c_f64(r);
		wf[i] = same ? vf[i] : ak6c_f64(r);
		vu[i] = ak6c_rnd(r) % 4 ? base + (uint32) (ak6c_rnd(r) % (nlut + 6)) - 2 : (uint32) ak6c_rnd(r);
		gid[i] = ak6c_rnd(r) % 300;
		hin[i] = ak6c_rnd(r);
		{
			const int	id = ak6c_rnd(r) % AK6C_NSTR;

			sv[i] = ak6c_view(strb[id], ak6c_str_len(id));
		}
	}
	for (uint32 i = 0; i < nlut + SIFT_AK6_LUT_PAD; i++)
		lut[i] = ak6c_rnd(r) % 3 ? (uint8) ak6c_rnd(r) : 0;
	for (int i = 0; i < nset; i++)
		set[i] = n && ak6c_rnd(r) % 4 ? v64[ak6c_rnd(r) % n] : ak6c_i64(r);

	for (int op = SCMP_EQ; op <= SCMP_GE; op++)
	{
		const int32 c32 = n && ak6c_rnd(r) % 4 ? v32[ak6c_rnd(r) % n] : (int32) ak6c_i64(r);
		const int64 c64 = n && ak6c_rnd(r) % 4 ? v64[ak6c_rnd(r) % n] : ak6c_i64(r);
		const double cf = n && ak6c_rnd(r) % 4 ? vf[ak6c_rnd(r) % n] : ak6c_f64(r);
		const SiftI128 c128 = n && ak6c_rnd(r) % 4 ? v128[ak6c_rnd(r) % n] : (SiftI128) {(uint64) ak6c_i64(r), ak6c_i64(r)};
		bool		ok = true;

#define PRED_RUN(call) \
		do { \
			AK6C_BOTH(memset(o[t_], 0xA5, nw * 8); call; if (t_) AK6C_DIFF(ok, o[0], o[1], nw * 8)); \
		} while (0)
		PRED_RUN(k[t_]->cmp_i32(o[t_], v32, n, op, c32));
		PRED_RUN(k[t_]->cmp_i64(o[t_], v64, n, op, c64));
		PRED_RUN(k[t_]->cmp_f64(o[t_], vf, n, op, cf));
		PRED_RUN(k[t_]->cmp_i128(o[t_], v128, n, op, c128));
		PRED_RUN(k[t_]->cmp2_i32(o[t_], v32, w32, n, op));
		PRED_RUN(k[t_]->cmp2_i64(o[t_], v64, w64, n, op));
		PRED_RUN(k[t_]->cmp2_f64(o[t_], vf, wf, n, op));
		bad += !ok;
	}
	{
		bool		ok = true;
		const int	tid = ak6c_rnd(r) % AK6C_NSTR;
		const SiftStrView target = ak6c_view(strb[tid], ak6c_str_len(tid));

		PRED_RUN(k[t_]->lut_u32(o[t_], vu, n, base, nlut, lut));
		PRED_RUN(k[t_]->in_set_i64(o[t_], v64, n, set, nset));
		PRED_RUN(k[t_]->cmp_strview_eq(o[t_], sv, n, target));
		PRED_RUN(k[t_]->stream_run_ne_i64(o[t_], v64, NULL, n));
		PRED_RUN(k[t_]->stream_run_ne_i64(o[t_], w64, vm, n));
		bad += !ok;
	}
	{
		bool		ok = true;
		uint16		pc[2];

		for (int neg = 0; neg < 2; neg++)
		{
			uint64	   *dv = ak6c_mask(r, n);

			AK6C_BOTH(memcpy(o[t_], vm, nw * 8); pc[t_] = k[t_]->key_present_apply(o[t_], m, dv, n, neg));
			ok = ok && pc[0] == pc[1];
			AK6C_DIFF(ok, o[0], o[1], nw * 8);
			AK6C_FREE(dv);
		}
		bad += !ok;
	}
	nsel = sift_aggkern6_scalar.mask_to_sel16(sel, m, n);
	for (int pass = 0; pass < 2; pass++)
	{
		const uint16 *s = pass ? sel : NULL;
		const int	nr = pass ? nsel : n;
		bool		ok = true;

		for (int mix = 0; mix < 2; mix++)
		{
			AK6C_BOTH(memcpy(h[t_], hin, n * 8); k[t_]->hash_i128(h[t_], v128, s, nr, mix));
			AK6C_DIFF(ok, h[0], h[1], nr * 8);
			AK6C_BOTH(memcpy(h[t_], hin, n * 8); k[t_]->hash_strview(h[t_], sv, s, nr, mix));
			AK6C_DIFF(ok, h[0], h[1], nr * 8);
		}
		bad += !ok;
	}
	for (int pass = 0; pass < 2; pass++)
	{
		const uint64 *mk = pass ? m : NULL;
		int32		mn32[2] = {1, 1},
					mx32[2] = {1, 1};
		int64		mn64[2] = {1, 1},
					mx64[2] = {1, 1};
		SiftI128	mn128[2] = {{1, 1}, {1, 1}},
					mx128[2] = {{1, 1}, {1, 1}};
		double		mnf[2] = {1, 1},
					mxf[2] = {1, 1};
		bool		any[4][2],
					ok = true;

		for (int t = 0; t < 2; t++)
		{
			any[0][t] = k[t]->minmax_i32(&mn32[t], &mx32[t], v32, mk, n);
			any[1][t] = k[t]->minmax_i64(&mn64[t], &mx64[t], v64, mk, n);
			any[2][t] = k[t]->minmax_i128(&mn128[t], &mx128[t], v128, mk, n);
			any[3][t] = k[t]->minmax_f64(&mnf[t], &mxf[t], vf, mk, n);
		}
		for (int s = 0; s < 4; s++)
			ok = ok && any[s][0] == any[s][1];
		ok = ok && mn32[0] == mn32[1] && mx32[0] == mx32[1] && mn64[0] == mn64[1] && mx64[0] == mx64[1];
		AK6C_DIFF(ok, &mn128[0], &mn128[1], 16);
		AK6C_DIFF(ok, &mx128[0], &mx128[1], 16);
		AK6C_DIFF(ok, &mnf[0], &mnf[1], 8);
		AK6C_DIFF(ok, &mxf[0], &mxf[1], 8);
		bad += !ok;
	}
	for (int pass = 0; pass < 2; pass++)
	{
		const uint16 *s = pass ? sel : NULL;
		const int	nr = pass ? nsel : n;
		double	   *mn[2] = {AK6C_ALLOC(300 * 8), AK6C_ALLOC(300 * 8)},
				   *mx[2] = {AK6C_ALLOC(300 * 8), AK6C_ALLOC(300 * 8)};
		bool		ok = true;

		for (int g = 0; g < 300; g++)
		{
			const double lo = ak6c_rnd(r) % 3 ? __builtin_inf() : ak6c_f64(r),
						hi = ak6c_rnd(r) % 3 ? -__builtin_inf() : ak6c_f64(r);

			mn[0][g] = mn[1][g] = lo;
			mx[0][g] = mx[1][g] = hi;
		}
		AK6C_BOTH(k[t_]->minmax_gid_f64(mn[t_], mx[t_], gid, vf, s, nr));
		AK6C_DIFF(ok, mn[0], mn[1], 300 * 8);
		AK6C_DIFF(ok, mx[0], mx[1], 300 * 8);
		bad += !ok;
		for (int t = 0; t < 2; t++)
		{
			AK6C_FREE(mn[t]);
			AK6C_FREE(mx[t]);
		}
	}
	{
		int64	   *d[2] = {AK6C_ALLOC(n * 8 + 1), AK6C_ALLOC(n * 8 + 1)};
		bool		ok = true;
		const int32 ka = (int32) ak6c_i64(r),
					kb = (int32) ak6c_i64(r);

		AK6C_BOTH(memset(d[t_], 0xA5, n * 8); k[t_]->lin_i64(d[t_], v64, ka, w64, kb, n));
		AK6C_DIFF(ok, d[0], d[1], n * 8);
		AK6C_BOTH(memset(d[t_], 0xA5, n * 8); k[t_]->mulw_i64(d[t_], v64, w64, n));
		AK6C_DIFF(ok, d[0], d[1], n * 8);
		AK6C_BOTH(memcpy(d[t_], v64, n * 8); k[t_]->mulw_i64(d[t_], d[t_], w64, n));
		AK6C_DIFF(ok, d[0], d[1], n * 8);
		bad += !ok;
		AK6C_FREE(d[0]);
		AK6C_FREE(d[1]);
	}
	for (int i = 0; i < AK6C_NSTR; i++)
		AK6C_FREE(strb[i]);
	AK6C_FREE(v32);
	AK6C_FREE(w32);
	AK6C_FREE(v64);
	AK6C_FREE(w64);
	AK6C_FREE(v128);
	AK6C_FREE(vf);
	AK6C_FREE(wf);
	AK6C_FREE(vu);
	AK6C_FREE(gid);
	AK6C_FREE(sv);
	AK6C_FREE(m);
	AK6C_FREE(vm);
	AK6C_FREE(o[0]);
	AK6C_FREE(o[1]);
	AK6C_FREE(h[0]);
	AK6C_FREE(h[1]);
	AK6C_FREE(hin);
	AK6C_FREE(sel);
	AK6C_FREE(lut);
	return bad;
}

#include "agg/kern6_cases_sbbf.h"

/* The number of slots of table b whose outputs differ from table a on the case of this seed (a length n, or a random one when n < 0) */
static inline int
ak6c_equiv(const SiftAggKernelsV6 *a, const SiftAggKernelsV6 *b, uint64 seed, int n)
{
	if (n < 0)
	{
		Ak6cRng		rng = {seed * UINT64CONST(0x9E3779B97F4A7C15) | 1};
		const int	edge = (int) (ak6c_rnd(&rng) % 33) * 64 + (int) (ak6c_rnd(&rng) % 3) - 1;

		n = ak6c_rnd(&rng) % 2 ? (int) (ak6c_rnd(&rng) % (SIFT_VEC_LEN + 1)) : Min(Max(edge, 0), SIFT_VEC_LEN);
	}
	return ak6c_equiv_base(a, b, seed, n) + ak6c_equiv_new(a, b, seed, n) + ak6c_equiv_sbbf(a, b, seed, n);
}
#endif							/* SIFT_AGGKERN6_CASES_H */
