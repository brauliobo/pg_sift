/*
 * tools/aggkern6/cases_wide.h - the cases of the slots of the predicates, the wide hashes, the minimums and maximums and the arithmetic of
 * the 2048-row operators, each against a reference written out here (not against the scalar table). Included by test.c, which has the
 * generators, the tables under test and CHECK.
 */
#ifndef SIFT_AGGKERN6_CASES_WIDE_H
#define SIFT_AGGKERN6_CASES_WIDE_H

/* ---- the predicates, the wide hashes, the minimums and maximums, the arithmetic: against references written out here ---- */

static int
ref_op(int op, int c)
{
	return op == SCMP_EQ ? c == 0 : op == SCMP_NE ? c != 0 : op == SCMP_LT ? c < 0 : op == SCMP_LE ? c <= 0 : op == SCMP_GT ? c > 0 : c >= 0;
}

/* The float8 order of PostgreSQL: NaN = NaN > everything, -0 = +0 */
static int
ref_f8(double a, double b)
{
	const int	an = __builtin_isnan(a),
				bn = __builtin_isnan(b);

	if (an || bn)
		return an == bn ? 0 : an ? 1 : -1;
	return a < b ? -1 : a > b;
}

static double
ref_norm(double d)
{
	return __builtin_isnan(d) ? __builtin_nan("") : d == 0 ? 0.0 : d;
}

#define BUILD_REF(ref, n, EXPR) \
	do { \
		memset(ref, 0, ((n) + 63) / 64 * 8); \
		for (int i = 0; i < (n); i++) \
			if (EXPR) \
				(ref)[i >> 6] |= UINT64CONST(1) << (i & 63); \
	} while (0)

/* Every level writes the whole bitmap, the bits >= n zero, as the reference has it */
#define PRED_CHECK(label, CALL) \
	do { \
		for (int t = 0; t < ntabs; t++) \
		{ \
			const SiftAggKernelsV6 *k = tabs[t]; \
			memset(got, 0xA5, nw * 8); \
			CALL; \
			CHECK(!memcmp(got, ref, nw * 8), "%s " label " differs, n = %d op %d\n", names[t], n, op); \
		} \
	} while (0)

static void
case_pred(int n)
{
	const int	nw = (n + 63) / 64;
	uint64	   *ref = xalloc(nw * 8),
			   *got = xalloc(nw * 8);

	for (int style = 0; style < 4; style++)
	{
		int32	   *v32 = xalloc(n * 4),
				   *w32 = xalloc(n * 4);
		int64	   *v64 = xalloc(n * 8),
				   *w64 = xalloc(n * 8);
		SiftI128   *v128 = xalloc(n * 16);
		double	   *vf = xalloc(n * 8),
				   *wf = xalloc(n * 8);

		for (int i = 0; i < n; i++)
		{
			const bool	tie = rnd() % 3 == 0;

			v32[i] = (int32) gen64(style);
			w32[i] = tie ? v32[i] : (int32) gen64(style);
			v64[i] = gen64(style);
			w64[i] = tie ? v64[i] : gen64(style);
			v128[i].lo = (uint64) gen64(style);
			v128[i].hi = gen64(style == 2 ? 2 : 3);
			if (tie && i)
				v128[i] = v128[rnd() % i];
			vf[i] = genf(style % 3);
			wf[i] = tie ? vf[i] : genf(style % 3);
		}
		for (int op = SCMP_EQ; op <= SCMP_GE; op++)
		{
			const int32 c32 = n && rnd() % 4 ? v32[rnd() % n] : (int32) gen64(1);
			const int64 c64 = n && rnd() % 4 ? v64[rnd() % n] : gen64(1);
			const double cf = n && rnd() % 4 ? vf[rnd() % n] : genf(1);
			const SiftI128 c128 = n && rnd() % 4 ? v128[rnd() % n] : (SiftI128) {(uint64) gen64(1), gen64(1)};

			BUILD_REF(ref, n, ref_op(op, v32[i] < c32 ? -1 : v32[i] > c32));
			PRED_CHECK("cmp_i32", k->cmp_i32(got, v32, n, op, c32));
			BUILD_REF(ref, n, ref_op(op, v64[i] < c64 ? -1 : v64[i] > c64));
			PRED_CHECK("cmp_i64", k->cmp_i64(got, v64, n, op, c64));
			BUILD_REF(ref, n, ref_op(op, ref_f8(vf[i], cf)));
			PRED_CHECK("cmp_f64", k->cmp_f64(got, vf, n, op, cf));
			BUILD_REF(ref, n, ref_op(op, (__int128) ak6_u128_of(v128[i]) < (__int128) ak6_u128_of(c128) ? -1 : (__int128) ak6_u128_of(v128[i]) > (__int128) ak6_u128_of(c128)));
			PRED_CHECK("cmp_i128", k->cmp_i128(got, v128, n, op, c128));
			BUILD_REF(ref, n, ref_op(op, v32[i] < w32[i] ? -1 : v32[i] > w32[i]));
			PRED_CHECK("cmp2_i32", k->cmp2_i32(got, v32, w32, n, op));
			BUILD_REF(ref, n, ref_op(op, v64[i] < w64[i] ? -1 : v64[i] > w64[i]));
			PRED_CHECK("cmp2_i64", k->cmp2_i64(got, v64, w64, n, op));
			BUILD_REF(ref, n, ref_op(op, ref_f8(vf[i], wf[i])));
			PRED_CHECK("cmp2_f64", k->cmp2_f64(got, vf, wf, n, op));
		}
		free(v32);
		free(w32);
		free(v64);
		free(w64);
		free(v128);
		free(vf);
		free(wf);
	}
	free(ref);
	free(got);
}

#define NSTR	9

static const uint32 str_len[NSTR] = {0, 1, 4, 5, 12, 13, 13, 31, 64};

static void
str_fill(int id, uint8 *b)
{
	for (uint32 i = 0; i < str_len[id]; i++)
		b[i] = (uint8) ('a' + (id == 6 && i == 12) + (i % 3));
}

/* The view of a string written out here, not by agg/kern6/cases.h */
static SiftStrView
make_view(const uint8 *b, uint32 len)
{
	SiftStrView v;
	uint8		pad[16] = {0};

	memset(&v, 0, sizeof(v));
	v.len = len;
	if (len <= 12)
	{
		memcpy(pad, b, len);
		memcpy(v.prefix, pad, 4);
		memcpy(v.u.tail, pad + 4, 8);
	}
	else
	{
		memcpy(v.prefix, b, 4);
		v.u.ptr = (const char *) b;
	}
	return v;
}

static uint64
ref_step(uint64 h, uint64 x, int mix)
{
	return sift_aggk6_hash1(mix ? ((h << 5) | (h >> 59)) ^ x : x);
}

static void
case_set_str(int n)
{
	const int	nw = (n + 63) / 64;
	uint64	   *ref = xalloc(nw * 8),
			   *got = xalloc(nw * 8),
			   *valid = gen_mask(n, rnd() % 5);
	uint8	   *sb[NSTR];
	int		   *ids = xalloc(n * 4);
	SiftStrView *sv = xalloc(n * 16);
	int64	   *v64 = xalloc(n * 8);
	uint32	   *vu = xalloc(n * 4);
	const int	op = -1;

	for (int i = 0; i < NSTR; i++)
	{
		sb[i] = xalloc(str_len[i] + 1);
		str_fill(i, sb[i]);
	}
	for (int i = 0; i < n; i++)
	{
		ids[i] = rnd() % NSTR;
		sv[i] = make_view(sb[ids[i]], str_len[ids[i]]);
		v64[i] = gen64(1);
	}
	for (int tid = 0; tid < NSTR; tid++)
	{
		SiftStrView target = make_view(sb[tid], str_len[tid]);

		BUILD_REF(ref, n, ids[i] == tid);
		PRED_CHECK("cmp_strview_eq", k->cmp_strview_eq(got, sv, n, target));
	}
	for (int trial = 0; trial < 6; trial++)
	{
		const uint32 nlut = rnd() % 300;
		const uint32 base = (uint32) rnd() >> (rnd() % 24);
		uint8	   *lut = xalloc(nlut + SIFT_AK6_LUT_PAD);
		int64		set[20];
		const int	nset = rnd() % 21;

		for (uint32 i = 0; i < nlut + SIFT_AK6_LUT_PAD; i++)
			lut[i] = rnd() % 3 ? (uint8) rnd() : 0;
		for (int i = 0; i < n; i++)
			vu[i] = rnd() % 4 ? base + (uint32) (rnd() % (nlut + 6)) - 2 : (uint32) rnd();
		for (int i = 0; i < nset; i++)
			set[i] = n && rnd() % 4 ? v64[rnd() % n] : gen64(3);
		BUILD_REF(ref, n, (uint32) (vu[i] - base) < nlut && lut[(uint32) (vu[i] - base)] != 0);
		PRED_CHECK("lut_u32", k->lut_u32(got, vu, n, base, nlut, lut));
		BUILD_REF(ref, n, ({ int in_ = 0; for (int q = 0; q < nset; q++) in_ |= v64[i] == set[q]; in_; }));
		PRED_CHECK("in_set_i64", k->in_set_i64(got, v64, n, set, nset));
		free(lut);
	}
	/* runs of a sorted column: v64 drawn from three values, NULLs under garbage */
	for (int i = 0; i < n; i++)
		v64[i] = i && rnd() % 2 ? v64[i - 1] : (int64) (rnd() % 3);
	BUILD_REF(ref, n, i == 0 || v64[i] != v64[i - 1]);
	PRED_CHECK("stream_run_ne_i64", k->stream_run_ne_i64(got, v64, NULL, n));
	BUILD_REF(ref, n, i == 0 || rbit(valid, i) != rbit(valid, i - 1) || (rbit(valid, i) && v64[i] != v64[i - 1]));
	PRED_CHECK("stream_run_ne_i64 valid", k->stream_run_ne_i64(got, v64, valid, n));
	for (int neg = 0; neg < 2; neg++)
	{
		uint64	   *kp = gen_garbage_mask(n, rnd() % 5),
				   *dv = gen_garbage_mask(n, rnd() % 5);

		for (int t = 0; t < ntabs; t++)
		{
			uint64	   *d = xalloc(nw * 8);
			uint16		pc = 0;

			memcpy(d, valid, nw * 8);
			BUILD_REF(ref, n, rbit(valid, i) && (neg ? !rbit(kp, i) && rbit(dv, i) : rbit(kp, i)));
			for (int i = 0; i < n; i++)
				pc += (ref[i >> 6] >> (i & 63)) & 1;
			CHECK(tabs[t]->key_present_apply(d, kp, dv, n, neg) == pc && !memcmp(d, ref, nw * 8), "%s key_present_apply differs, n = %d neg %d\n", names[t], n, neg);
			free(d);
		}
		free(kp);
		free(dv);
	}
	for (int i = 0; i < NSTR; i++)
		free(sb[i]);
	free(ids);
	free(sv);
	free(v64);
	free(vu);
	free(valid);
	free(ref);
	free(got);
}

static void
case_wide_hash(int n)
{
	for (int usesel = 0; usesel < 2; usesel++)
		for (int mix = 0; mix < 2; mix++)
		{
			uint64	   *m = usesel ? gen_mask(n, 2) : NULL,
					   *h0 = xalloc(n * 8 + 8),
					   *href = xalloc(n * 8 + 8),
					   *hgot = xalloc(n * 8 + 8);
			uint16	   *sel = NULL;
			int			nrows = n;
			SiftI128   *v = xalloc(n * 16);
			SiftStrView *sv = xalloc(n * 16);
			uint8	   *sb[NSTR];

			if (usesel)
			{
				sel = xalloc((n + SIFT_SEL_SLACK) * 2);
				nrows = sift_aggkern6_scalar.mask_to_sel16(sel, m, n);
			}
			for (int i = 0; i < NSTR; i++)
			{
				sb[i] = xalloc(str_len[i] + 1);
				str_fill(i, sb[i]);
			}
			for (int i = 0; i < n; i++)
			{
				const int	id = rnd() % NSTR;

				v[i].lo = (uint64) gen64(3);
				v[i].hi = gen64(3);
				sv[i] = make_view(sb[id], str_len[id]);
				h0[i] = rnd();
			}
			for (int i = 0; i < nrows; i++)
			{
				const int	r = sel ? sel[i] : i;

				href[i] = ref_step(ref_step(h0[i], v[r].lo, mix), (uint64) v[r].hi, 1);
			}
			for (int t = 0; t < ntabs; t++)
			{
				memcpy(hgot, h0, n * 8);
				tabs[t]->hash_i128(hgot, v, sel, nrows, mix);
				CHECK(!memcmp(hgot, href, nrows * 8), "%s hash_i128 sel=%d mix=%d differs, n = %d\n", names[t], usesel, mix, n);
			}
			for (int i = 0; i < nrows; i++)
			{
				const int	r = sel ? sel[i] : i;
				const uint32 len = sv[r].len;
				uint8		pad[16] = {0};
				uint64		hdr,
							tail;

				if (len <= 12)
				{
					int			id = 0;

					for (; id < NSTR; id++)
						if (str_len[id] == len && !memcmp(sb[id], &sv[r].prefix, Min(len, 4)) && (len <= 4 || !memcmp(sb[id] + 4, sv[r].u.tail, len - 4)))
							break;
					memcpy(pad, sb[id], len);
					hdr = len | ((uint64) (pad[0] | pad[1] << 8 | pad[2] << 16 | (uint32) pad[3] << 24) << 32);
					memcpy(&tail, pad + 4, 8);
					href[i] = ref_step(ref_step(h0[i], hdr, mix), tail, 1);
				}
				else
					href[i] = ref_step(h0[i], sift_aggk6_hash_bytes(sv[r].u.ptr, len), mix);
			}
			for (int t = 0; t < ntabs; t++)
			{
				memcpy(hgot, h0, n * 8);
				tabs[t]->hash_strview(hgot, sv, sel, nrows, mix);
				CHECK(!memcmp(hgot, href, nrows * 8), "%s hash_strview sel=%d mix=%d differs, n = %d\n", names[t], usesel, mix, n);
			}
			for (int i = 0; i < NSTR; i++)
				free(sb[i]);
			free(h0);
			free(href);
			free(hgot);
			free(m);
			free(sel);
			free(v);
			free(sv);
		}
}

static void
case_minmax(int n)
{
	for (int style = 0; style < 4; style++)
		for (int mk = -1; mk < 5; mk++)
		{
			int32	   *v32 = xalloc(n * 4);
			int64	   *v64 = xalloc(n * 8),
					   *w64 = xalloc(n * 8),
					   *d = xalloc(n * 8);
			SiftI128   *v128 = xalloc(n * 16);
			double	   *vf = xalloc(n * 8);
			uint64	   *m = mk < 0 ? NULL : gen_mask(n, mk);
			bool		any = false;
			int32		lo32 = 0,
						hi32 = 0,
						g32l = 7,
						g32h = 7;
			int64		lo64 = 0,
						hi64 = 0,
						g64l = 7,
						g64h = 7;
			__int128	lo128 = 0,
						hi128 = 0;
			double		lof = 0,
						hif = 0,
						gfl = 5,
						gfh = 5;
			SiftI128	g128l = {7, 7},
						g128h = {7, 7},
						r128l = {7, 7},
						r128h = {7, 7};
			const int32 ka = (int32) gen64(style),
						kb = (int32) gen64(style);

			for (int i = 0; i < n; i++)
			{
				v32[i] = (int32) gen64(style);
				v64[i] = gen64(style);
				w64[i] = gen64(style);
				v128[i].lo = (uint64) gen64(style);
				v128[i].hi = gen64(style == 2 ? 2 : 3);
				vf[i] = genf(style % 3);
			}
			for (int i = 0; i < n; i++)
			{
				const __int128 x = (__int128) ak6_u128_of(v128[i]);

				if (m && !rbit(m, i))
					continue;
				lo32 = !any || v32[i] < lo32 ? v32[i] : lo32;
				hi32 = !any || v32[i] > hi32 ? v32[i] : hi32;
				lo64 = !any || v64[i] < lo64 ? v64[i] : lo64;
				hi64 = !any || v64[i] > hi64 ? v64[i] : hi64;
				lo128 = !any || x < lo128 ? x : lo128;
				hi128 = !any || x > hi128 ? x : hi128;
				lof = !any || ref_f8(vf[i], lof) < 0 ? vf[i] : lof;
				hif = !any || ref_f8(vf[i], hif) > 0 ? vf[i] : hif;
				any = true;
			}
			lof = ref_norm(lof);
			hif = ref_norm(hif);
			if (any)
			{
				r128l = ak6_i128_of((ak6_u128) lo128);
				r128h = ak6_i128_of((ak6_u128) hi128);
			}
			for (int t = 0; t < ntabs; t++)
			{
				const SiftAggKernelsV6 *k = tabs[t];

				g32l = g32h = 7;
				g64l = g64h = 7;
				gfl = gfh = 5;
				g128l.lo = g128l.hi = g128h.lo = g128h.hi = 7;
				CHECK(k->minmax_i32(&g32l, &g32h, v32, m, n) == any && (!any || (g32l == lo32 && g32h == hi32)) && (any || (g32l == 7 && g32h == 7)),
					  "%s minmax_i32 differs, n = %d\n", names[t], n);
				CHECK(k->minmax_i64(&g64l, &g64h, v64, m, n) == any && (!any || (g64l == lo64 && g64h == hi64)) && (any || (g64l == 7 && g64h == 7)),
					  "%s minmax_i64 differs, n = %d\n", names[t], n);
				CHECK(k->minmax_i128(&g128l, &g128h, v128, m, n) == any && !memcmp(&g128l, &r128l, 16) && !memcmp(&g128h, &r128h, 16),
					  "%s minmax_i128 differs, n = %d\n", names[t], n);
				CHECK(k->minmax_f64(&gfl, &gfh, vf, m, n) == any && (any ? !memcmp(&gfl, &lof, 8) && !memcmp(&gfh, &hif, 8) : gfl == 5 && gfh == 5),
					  "%s minmax_f64 differs, n = %d\n", names[t], n);
				k->lin_i64(d, v64, ka, w64, kb, n);
				for (int i = 0; i < n; i++)
					CHECK(d[i] == (int64) ((uint64) ((unsigned __int128) (__int128) v64[i] * (uint64) (int64) ka + (unsigned __int128) (__int128) w64[i] * (uint64) (int64) kb)),
						  "%s lin_i64 differs at %d, n = %d\n", names[t], i, n);
				k->mulw_i64(d, v64, w64, n);
				for (int i = 0; i < n; i++)
					CHECK(d[i] == (int64) ((uint64) ((__int128) v64[i] * w64[i])), "%s mulw_i64 differs at %d, n = %d\n", names[t], i, n);
				memcpy(d, v64, n * 8);
				k->mulw_i64(d, d, w64, n);
				for (int i = 0; i < n; i++)
					CHECK(d[i] == (int64) ((uint64) ((__int128) v64[i] * w64[i])), "%s mulw_i64 in place differs at %d, n = %d\n", names[t], i, n);
			}
			free(v32);
			free(v64);
			free(w64);
			free(d);
			free(v128);
			free(vf);
			free(m);
		}
}

/* minmax_gid_f64 against the rows in order, the group cells starting at the extremes, at NaN or at -0.0 */
static void
case_gid_f64(int n)
{
	for (int usesel = 0; usesel < 2; usesel++)
		for (int ng = 1; ng <= 5; ng += 2)
		{
			const int	G = ng == 1 ? 1 : ng == 3 ? 7 : 300;
			uint32	   *gid = xalloc(n * 4);
			double	   *vf = xalloc(n * 8);
			uint16	   *sel = NULL;
			uint64	   *m = usesel ? gen_mask(n, 2) : NULL;
			int			nrows = n;
			double	   *mn0 = xalloc(G * 8),
					   *mx0 = xalloc(G * 8);

			if (usesel)
			{
				sel = xalloc((n + SIFT_SEL_SLACK) * 2);
				nrows = sift_aggkern6_scalar.mask_to_sel16(sel, m, n);
			}
			for (int i = 0; i < n; i++)
			{
				gid[i] = rnd() % G;
				vf[i] = genf(2);
			}
			for (int g = 0; g < G; g++)
			{
				const double init[4] = {__builtin_inf(), -__builtin_inf(), -0.0, __builtin_nan("")};

				mn0[g] = init[rnd() % 4];
				mx0[g] = init[rnd() % 4];
			}
			for (int t = 0; t < ntabs; t++)
			{
				double	   *mn = xalloc(G * 8),
						   *mx = xalloc(G * 8),
						   *rmn = xalloc(G * 8),
						   *rmx = xalloc(G * 8);

				memcpy(mn, mn0, G * 8);
				memcpy(mx, mx0, G * 8);
				memcpy(rmn, mn0, G * 8);
				memcpy(rmx, mx0, G * 8);
				for (int i = 0; i < nrows; i++)
				{
					const int	r = sel ? sel[i] : i;

					rmn[gid[r]] = ref_norm(ref_f8(vf[r], rmn[gid[r]]) < 0 ? vf[r] : rmn[gid[r]]);
					rmx[gid[r]] = ref_norm(ref_f8(vf[r], rmx[gid[r]]) > 0 ? vf[r] : rmx[gid[r]]);
				}
				tabs[t]->minmax_gid_f64(mn, mx, gid, vf, sel, nrows);
				CHECK(!memcmp(mn, rmn, G * 8) && !memcmp(mx, rmx, G * 8), "%s minmax_gid_f64 differs, n = %d\n", names[t], n);
				free(mn);
				free(mx);
				free(rmn);
				free(rmx);
			}
			free(gid);
			free(vf);
			free(m);
			free(sel);
			free(mn0);
			free(mx0);
		}
}

#endif							/* SIFT_AGGKERN6_CASES_WIDE_H */
