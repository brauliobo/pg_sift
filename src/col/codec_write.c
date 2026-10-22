/*
 * col/codec_write.c - the writer of the chunk codec: statistics, the exact-size choice of the lightweight cascade, the general-purpose
 * layer under the cost rule.
 */
#include "core/host.h"

#include <math.h>

#include "bm25/analyze.h"
#include "core/cmp.h"
#include "col/codec_int.h"
#include "col/compress.h"

void
put32(StringInfo s, uint32 x)
{
	appendBinaryStringInfo(s, (const char *) &x, 4);
}

static void
put64(StringInfo s, int64 x)
{
	appendBinaryStringInfo(s, (const char *) &x, 8);
}

void
pad8(StringInfo s)
{
	static const char zeros[8] = {0};

	if (s->len & 7)
		appendBinaryStringInfo(s, zeros, 8 - (s->len & 7));
}

/* The 8-byte node header: tag, one parameter byte, two pad bytes, one u32 */
static void
put_head(StringInfo s, uint8 tag, uint8 aux, uint32 f1)
{
	char		h[8] = {0};

	h[0] = (char) tag;
	h[1] = (char) aux;
	memcpy(h + 4, &f1, 4);
	appendBinaryStringInfo(s, h, 8);
}

static uint32
bytes_for(uint64 range)
{
	uint32		b = (sift_cc_bits_of(range) + 7) / 8;

	return b ? b : 1;
}

typedef struct CcStats
{
	int64		min,
				max;
	uint64		range;			/* max - min */
	int64		dmin,
				dmax;			/* of v[i] - v[i - 1] */
	uint64		drange;
	uint32		nruns;
	bool		same;
} CcStats;

static void
stats_of(const int64 *v, uint32 n, CcStats *st)
{
	st->min = st->max = v[0];
	st->dmin = st->dmax = 0;
	st->nruns = 1;
	for (uint32 i = 1; i < n; i++)
	{
		int64		d = (int64) ((uint64) v[i] - (uint64) v[i - 1]);

		st->min = v[i] < st->min ? v[i] : st->min;
		st->max = v[i] > st->max ? v[i] : st->max;
		if (i == 1)
			st->dmin = st->dmax = d;
		else
		{
			st->dmin = d < st->dmin ? d : st->dmin;
			st->dmax = d > st->dmax ? d : st->dmax;
		}
		st->nruns += d != 0;
	}
	st->range = (uint64) st->max - (uint64) st->min;
	st->drange = (uint64) st->dmax - (uint64) st->dmin;
	st->same = st->nruns == 1;
}

/* An explicit lz4 or zstd policy on a build without that library is an error; auto simply uses what the build has */
void
check_policy(const SiftCcPolicy *pol)
{
	if (pol->mode == SIFT_CCM_LAYER && !sift_colz_available(pol->alg))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("compression %s is not available in this build", pol->alg == SIFT_CC_ALG_LZ4 ? "lz4" : "zstd"),
						errhint("Install the library and rebuild without SIFT_NO_%s.", pol->alg == SIFT_CC_ALG_LZ4 ? "LZ4" : "ZSTD")));
}

static bool
enc_allowed(const CcW *w, int tag)
{
	if (w->pol->mode == SIFT_CCM_NONE)
		return false;
	return !w->pol->explicit_encs || (w->pol->encs & CC_BIT(tag));
}

static void
emit_const(StringInfo out, int64 v)
{
	put_head(out, SIFT_CCN_CONST, 0, 0);
	put64(out, v);
}

/* The vertical layout of the first n values of tmp (padded with zeros to whole rows) at width W into packed (sift_cc_words(n, W) words, zeroed) */
static void
pack_vertical(const uint32 *tmp, uint32 n, uint32 W, uint32 *packed)
{
	uint32		rows = rows_for(n);

	for (uint32 b = 0; b * 32 < rows; b++)
	{
		uint32		R = rows - b * 32 > 32 ? 32 : rows - b * 32;
		const uint32 *vb = tmp + (size_t) b * 1024;
		uint32	   *o = packed + (size_t) b * 32 * W;

		for (uint32 r = 0; r < R; r++)
		{
			uint32		bp = r * W,
						s = bp & 31;

			for (uint32 l = 0; l < 32; l++)
			{
				uint32		x = vb[r * 32 + l];

				o[(size_t) (bp >> 5) * 32 + l] |= x << s;
				if (s + W > 32)
					o[(size_t) ((bp >> 5) + 1) * 32 + l] |= x >> (32 - s);
			}
		}
	}
}

static void
emit_words(StringInfo out, uint32 *tmp, uint32 n, uint32 W)
{
	uint64		words = sift_cc_words(n, W);
	uint32	   *packed = palloc0(sizeof(uint32) * (words + 32));

	pack_vertical(tmp, n, W, packed);
	appendBinaryStringInfo(out, (const char *) packed, (int) (words * 4));
	pad8(out);
	pfree(packed);
}

static void
emit_forbit(StringInfo out, const int64 *v, uint32 n, int64 base, uint32 W)
{
	uint32	   *tmp = palloc0(sizeof(uint32) * (CC_CAP32(n) + 32));

	for (uint32 i = 0; i < n; i++)
		tmp[i] = (uint32) ((uint64) v[i] - (uint64) base);
	put_head(out, SIFT_CCN_FORBIT, (uint8) W, 0);
	put64(out, base);
	emit_words(out, tmp, n, W);
	pfree(tmp);
}

static void
emit_forbyte(StringInfo out, const int64 *v, uint32 n, int64 base, uint32 B)
{
	put_head(out, SIFT_CCN_FORBYTE, (uint8) B, 0);
	put64(out, base);
	for (uint32 i = 0; i < n; i++)
	{
		uint64		x = (uint64) v[i] - (uint64) base;

		appendBinaryStringInfo(out, (const char *) &x, (int) B);
	}
	pad8(out);
}

static void
emit_delta(StringInfo out, const int64 *v, uint32 n, const CcStats *st, uint32 W)
{
	uint32	   *tmp = palloc0(sizeof(uint32) * (CC_CAP32(n) + 32));

	for (uint32 i = 1; i < n; i++)
		tmp[i] = (uint32) ((uint64) v[i] - (uint64) v[i - 1] - (uint64) st->dmin);
	put_head(out, SIFT_CCN_DELTA, (uint8) W, 0);
	put64(out, v[0]);
	put64(out, st->dmin);
	for (uint32 i = 1024; i < n; i += 1024)
		put64(out, v[i]);
	emit_words(out, tmp, n, W);
	pfree(tmp);
}

/* Distinct values ascending (palloc'd) when there are at most limit of them, else NULL; *d is their number */
static int64 *
distinct_sorted(const int64 *v, uint32 n, uint32 limit, uint32 *d)
{
	uint32		cap = 16;
	int64	   *tab,
			   *out;
	uint8	   *used;
	uint32		cnt = 0;

	while (cap < limit * 2 + 2)
		cap <<= 1;
	tab = palloc(sizeof(int64) * cap);
	used = palloc0(cap);
	out = palloc(sizeof(int64) * (limit + 1));
	for (uint32 i = 0; i < n; i++)
	{
		uint64		h = ((uint64) v[i] * UINT64CONST(0x9E3779B97F4A7C15)) >> 32;
		uint32		k = (uint32) h & (cap - 1);

		while (used[k] && tab[k] != v[i])
			k = (k + 1) & (cap - 1);
		if (!used[k])
		{
			if (cnt == limit)
			{
				pfree(tab);
				pfree(used);
				pfree(out);
				return NULL;
			}
			used[k] = 1;
			tab[k] = v[i];
			out[cnt++] = v[i];
		}
	}
	qsort(out, cnt, sizeof(int64), sift_cmp_i64);
	pfree(tab);
	pfree(used);
	*d = cnt;
	return out;
}

/* ---- general-purpose layer ---- */

static double
alg_ns(int alg)
{
	return alg == SIFT_CC_ALG_LZ4 ? SIFT_CC_NS_LZ4 : SIFT_CC_NS_ZSTD;
}

/* The best wrapper the policy accepts for the stream raw[0..rawlen) (best->buf palloc'd), or false */
bool
wrap_choose(const CcW *w, const uint8 *raw, uint32 rawlen, CcRole role, CcZTry *best)
{
	int			algs[2],
				nalg = 0;
	double		rho = role == CCR_HOT ? SIFT_CC_RHO_HOT : SIFT_CC_RHO_COLD;
	bool		found = false;
	const SiftCcPolicy *pol = w->pol;

	best->buf = NULL;
	if (pol->mode == SIFT_CCM_NONE || rawlen < SIFT_CC_MIN_WRAP)
		return false;
	if (pol->mode == SIFT_CCM_LAYER)
	{
		algs[nalg++] = pol->alg;
		rho = 0;
	}
	else if (pol->alg == SIFT_CC_ALG_AUTO)
	{
		algs[nalg++] = SIFT_CC_ALG_LZ4;
		algs[nalg++] = SIFT_CC_ALG_ZSTD;
	}
	else if (pol->alg != SIFT_CC_ALG_NONE)
		algs[nalg++] = pol->alg;
	best->cost = (double) rawlen * (1.0 - SIFT_CC_MIN_GAIN);
	for (int k = 0; k < nalg; k++)
	{
		int			alg = algs[k];
		int			level = pol->mode == SIFT_CCM_LAYER ? pol->level : 0;
		Size		cap,
					clen;
		char	   *buf;
		double		cost;

		if (!sift_colz_available(alg))
			continue;
		if (rawlen > 32768)
		{
			/* four 4 KB slices: an estimate that decides whether the whole stream is worth compressing */
			Size		scap = sift_colz_bound(alg, 16384);
			char	   *sample = palloc(16384),
					   *sb = palloc(scap);
			Size		sl;

			for (int q = 0; q < 4; q++)
				memcpy(sample + q * 4096, raw + ((uint64) (rawlen - 4096) * q) / 3, 4096);
			sl = sift_colz_compress(alg, level, sample, 16384, sb, scap);
			pfree(sample);
			pfree(sb);
			if (sl == 0 || (double) sl / 16384 * rawlen + 16 + rho * rawlen * alg_ns(alg) >= best->cost)
				continue;
		}
		cap = sift_colz_bound(alg, rawlen);
		buf = palloc(cap);
		clen = sift_colz_compress(alg, level, raw, rawlen, buf, cap);
		/* zstd would have to save four fifths of a stream that LZ4 could not even shrink by a seventh to pass the hot rule: not tried */
		if (alg == SIFT_CC_ALG_LZ4 && pol->mode == SIFT_CCM_AUTO && role == CCR_HOT && (clen == 0 || clen > rawlen - rawlen / 7))
			nalg = 1;
		cost = (double) CC_RUP8(clen) + CC_NHDR + rho * rawlen * alg_ns(alg);
		if (clen == 0 || cost >= best->cost)
		{
			pfree(buf);
			continue;
		}
		if (best->buf)
			pfree(best->buf);
		best->buf = buf;
		best->len = clen;
		best->alg = alg;
		best->cost = cost;
		found = true;
	}
	return found;
}

static void
emit_wrap(StringInfo out, const CcZTry *z, uint32 rawlen)
{
	put_head(out, SIFT_CCN_WRAP, (uint8) z->alg, rawlen);
	put32(out, (uint32) z->len);
	put32(out, 0);
	appendBinaryStringInfo(out, z->buf, (int) z->len);
	pad8(out);
}

/* Size of the node bytes after the best wrapper (their own size when none is accepted) */
static Size
node_wrapped_size(const CcW *w, const uint8 *node, Size len, CcRole role)
{
	CcZTry		z;
	Size		r = len;

	if (wrap_choose(w, node, (uint32) len, role, &z))
	{
		r = CC_NHDR + CC_RUP8(z.len);
		pfree(z.buf);
	}
	return r;
}

/* Replaces the leaf node at out->data[start..] by a wrapper when the policy accepts one */
static void
maybe_wrap(const CcW *w, StringInfo out, int start, CcRole role)
{
	CcZTry		z;
	uint32		rawlen = (uint32) (out->len - start);

	if (wrap_choose(w, (const uint8 *) out->data + start, rawlen, role, &z))
	{
		out->len = start;
		out->data[start] = 0;
		emit_wrap(out, &z, rawlen);
		pfree(z.buf);
	}
}

/* ---- decimal floats ---- */

static inline bool
alp_digit(SiftCcKind kind, int64 key, int e, int64 *d)
{
	if (kind == SIFT_CC_F8)
	{
		double		x = sift_col_f8_from_key(key),
					t,
					back;

		if (!isfinite(x))
			return false;
		t = x * sift_cc_p10[e];
		if (fabs(t) >= 4503599627370496.0)
			return false;
		*d = (int64) nearbyint(t);
		back = (double) *d / sift_cc_p10[e];
		return sift_col_f8_key(back) == key;
	}
	else
	{
		double		xd = sift_col_f8_from_key(key);
		float		x = (float) xd,
					t,
					back;

		if (!isfinite(xd) || (double) x != xd)
			return false;
		t = x * sift_cc_p10f[e];
		if (fabsf(t) >= 16777216.0f)
			return false;
		*d = (int64) nearbyintf(t);
		back = (float) (int32) *d / sift_cc_p10f[e];
		return sift_col_f8_key((double) back) == key;
	}
}

/* true when an ALP node was built into cand */
static bool
alp_build(const CcW *w, StringInfo cand, const int64 *keys, uint32 n, int depth, CcRole role)
{
	SiftCcKind	kind = w->kind;
	CcW			wi = {w->pol, SIFT_CC_INT};
	int			emax = kind == SIFT_CC_F8 ? SIFT_CC_ALP_EMAX_F8 : SIFT_CC_ALP_EMAX_F4,
				bestE = 0;
	uint32		bestOk = 0,
				step = n > 64 ? n / 64 : 1,
				nsample = 0,
				nexc = 0,
				firstok = n;
	int64	   *dig,
			   *pos,
			   *ekeys;
	uint8	   *ok;
	int64		dmin = 0,
				dmax = 0;
	StringInfoData cd,
				cp;

	for (uint32 i = 0; i < n; i += step)
		nsample++;
	for (int e = 0; e <= emax; e++)
	{
		uint32		cnt = 0;
		int64		d;

		for (uint32 i = 0; i < n; i += step)
			cnt += alp_digit(kind, keys[i], e, &d);
		if (cnt > bestOk)
		{
			bestOk = cnt;
			bestE = e;
		}
	}
	if (bestOk * 10 < nsample * 9)
		return false;
	dig = palloc(sizeof(int64) * n);
	pos = palloc(sizeof(int64) * n);
	ekeys = palloc(sizeof(int64) * n);
	ok = palloc(n);
	for (uint32 i = 0; i < n; i++)
	{
		ok[i] = alp_digit(kind, keys[i], bestE, &dig[i]);
		if (ok[i] && firstok == n)
		{
			firstok = i;
			dmin = dmax = dig[i];
		}
		else if (ok[i])
		{
			dmin = dig[i] < dmin ? dig[i] : dmin;
			dmax = dig[i] > dmax ? dig[i] : dmax;
		}
	}
	/* a digit stands in for each exception: the next exact one before the first, the previous exact one after */
	if (firstok < n)
		for (uint32 i = 0; i < n; i++)
		{
			if (!ok[i])
			{
				dig[i] = i < firstok ? dig[firstok] : dig[i - 1];
				pos[nexc] = i;
				ekeys[nexc++] = keys[i];
			}
		}
	if (firstok == n || (uint64) dmax - (uint64) dmin > 0xFFFFFFFFu || nexc > n / 4)
	{
		pfree(dig);
		pfree(pos);
		pfree(ekeys);
		pfree(ok);
		return false;
	}
	initStringInfo(&cd);
	initStringInfo(&cp);
	enc_node(&wi, &cd, dig, n, depth - 1, role);
	if (nexc)
		enc_node(&wi, &cp, pos, nexc, depth - 1, CCR_COLD);
	initStringInfo(cand);
	put_head(cand, SIFT_CCN_ALP, (uint8) bestE, nexc);
	put32(cand, (uint32) cd.len);
	put32(cand, (uint32) cp.len);
	put64(cand, dmin);
	put64(cand, dmax);
	appendBinaryStringInfo(cand, cd.data, cd.len);
	appendBinaryStringInfo(cand, cp.data, cp.len);
	appendBinaryStringInfo(cand, (const char *) ekeys, (int) (sizeof(int64) * nexc));
	pfree(cd.data);
	pfree(cp.data);
	pfree(dig);
	pfree(pos);
	pfree(ekeys);
	pfree(ok);
	return true;
}

/* ---- the chooser ---- */

/* The smallest cascade node built so far (node.data is NULL while none beat the leaf size) */
typedef struct CcBest
{
	StringInfoData node;
	Size		size;
} CcBest;

/* Takes the node a builder initialized: kept when smaller than the best, freed otherwise */
static void
offer(CcBest *b, StringInfo cand)
{
	if ((Size) cand->len < b->size)
	{
		if (b->node.data)
			pfree(b->node.data);
		b->node = *cand;
		b->size = cand->len;
	}
	else
		pfree(cand->data);
}

/* A cascade node: head, the byte length of its two children, the children (each a node of its own) */
static void
put_pair(StringInfo cand, uint8 tag, uint32 count, StringInfo a, StringInfo b)
{
	initStringInfo(cand);
	put_head(cand, tag, 0, count);
	put32(cand, (uint32) a->len);
	put32(cand, (uint32) b->len);
	appendBinaryStringInfo(cand, a->data, a->len);
	appendBinaryStringInfo(cand, b->data, b->len);
	pfree(a->data);
	pfree(b->data);
}

static void
rle_build(const CcW *w, StringInfo cand, const int64 *v, uint32 n, uint32 nruns, int depth, CcRole role)
{
	CcW			wi = {w->pol, SIFT_CC_INT};
	int64	   *rv = palloc(sizeof(int64) * nruns),
			   *rl = palloc(sizeof(int64) * nruns);
	uint32		nr = 0;
	StringInfoData cv,
				cl;

	for (uint32 i = 0; i < n; i++)
	{
		if (i && v[i] == v[i - 1])
			rl[nr - 1]++;
		else
		{
			rv[nr] = v[i];
			rl[nr++] = 1;
		}
	}
	initStringInfo(&cv);
	initStringInfo(&cl);
	enc_node(&wi, &cv, rv, nr, depth - 1, role);
	enc_node(&wi, &cl, rl, nr, depth - 1, role);
	put_pair(cand, SIFT_CCN_RLE, nr, &cv, &cl);
	pfree(rv);
	pfree(rl);
}

/* false when the values are not few enough for a dictionary to pay */
static bool
dict_build(const CcW *w, StringInfo cand, const int64 *v, uint32 n, const CcStats *st, int depth, CcRole role)
{
	CcW			wi = {w->pol, SIFT_CC_INT};
	uint32		limit = n / 2 > 65536 ? 65536 : n / 2,
				d = 0;
	int64	   *dv = limit >= 2 ? distinct_sorted(v, n, limit, &d) : NULL;
	int64	   *codes;
	StringInfoData cd,
				cc;

	if (!dv || sift_cc_bits_of(d - 1) >= sift_cc_bits_of(st->range))
	{
		if (dv)
			pfree(dv);
		return false;
	}
	codes = palloc(sizeof(int64) * n);
	for (uint32 i = 0; i < n; i++)
	{
		uint32		lo = 0,
					hi = d;

		SIFT_BISECT(uint32, m, lo, hi, dv[m] < v[i]);
		codes[i] = lo;
	}
	initStringInfo(&cd);
	initStringInfo(&cc);
	enc_node(&wi, &cd, dv, d, depth - 1, CCR_COLD);
	enc_node(&wi, &cc, codes, n, depth - 1, role);
	put_pair(cand, SIFT_CCN_DICT, d, &cd, &cc);
	pfree(codes);
	pfree(dv);
	return true;
}

/* The common divisor of the offsets from min (1 when there is none): most values leave it after a few steps */
static uint64
offsets_gcd(const int64 *v, uint32 n, int64 min)
{
	uint64		g = 0;

	for (uint32 i = 0; i < n && g != 1; i++)
	{
		uint64		a = (uint64) v[i] - (uint64) min,
					b = g;

		while (b)
		{
			uint64		t = a % b;

			a = b;
			b = t;
		}
		g = a;
	}
	return g;
}

/* base + g * child: timestamps of whole seconds in microseconds, money in cents, a step of 1000 */
static bool
scale_build(const CcW *w, StringInfo cand, const int64 *v, uint32 n, const CcStats *st, int depth, CcRole role)
{
	CcW			wi = {w->pol, SIFT_CC_INT};
	uint64		g = offsets_gcd(v, n, st->min);
	int64	   *q;
	StringInfoData child;

	if (g < 2 || g > (uint64) INT64_MAX)
		return false;
	q = palloc(sizeof(int64) * n);
	for (uint32 i = 0; i < n; i++)
		q[i] = (int64) (((uint64) v[i] - (uint64) st->min) / g);
	initStringInfo(&child);
	enc_node(&wi, &child, q, n, depth - 1, role);
	initStringInfo(cand);
	put_head(cand, SIFT_CCN_SCALE, 0, (uint32) child.len);
	put64(cand, st->min);
	put64(cand, (int64) g);
	appendBinaryStringInfo(cand, child.data, child.len);
	pfree(child.data);
	pfree(q);
	return true;
}

/* Appends the leaf node (frame of reference, plain bytes or delta) chosen by exact size, wrapped when the cost rule allows */
static int
emit_leaf(const CcW *w, StringInfo out, const int64 *v, uint32 n, const CcStats *st, int tag, uint32 W, CcRole role)
{
	int			start = out->len;

	if (tag == SIFT_CCN_DELTA)
		emit_delta(out, v, n, st, W);
	else if (tag == SIFT_CCN_FORBIT)
		emit_forbit(out, v, n, st->min, W);
	else
		emit_forbyte(out, v, n, st->min, W);
	/* with a layer in play the byte-aligned twin of a narrow bit-packed stream is also tried, and the smaller result kept */
	if (tag == SIFT_CCN_FORBIT && W <= 16 && (w->pol->mode == SIFT_CCM_LAYER || role == CCR_COLD))
	{
		StringInfoData twin;
		Size		a = node_wrapped_size(w, (const uint8 *) out->data + start, out->len - start, role),
					b;

		initStringInfo(&twin);
		emit_forbyte(&twin, v, n, st->min, bytes_for(st->range));
		b = node_wrapped_size(w, (const uint8 *) twin.data, twin.len, role);
		if (b < a)
		{
			out->len = start;
			out->data[start] = 0;
			appendBinaryStringInfo(out, twin.data, twin.len);
			tag = SIFT_CCN_FORBYTE;
		}
		pfree(twin.data);
	}
	maybe_wrap(w, out, start, role);
	return tag;
}

/* Appends the best node for v[0..n) to out; returns its tag before any wrapper */
int
enc_node(const CcW *w, StringInfo out, const int64 *v, uint32 n, int depth, CcRole role)
{
	CcStats		st;
	CcBest		best = {{0}, 0};
	StringInfoData cand;
	int			tag = SIFT_CCN_FORBYTE;
	uint32		W;

	if (n == 0)
	{
		emit_const(out, 0);
		return SIFT_CCN_CONST;
	}
	stats_of(v, n, &st);
	if (st.same)
	{
		emit_const(out, v[0]);
		return SIFT_CCN_CONST;
	}
	if (w->pol->mode == SIFT_CCM_NONE)
	{
		uint32		B = bytes_for(st.range);

		emit_forbyte(out, v, n, st.min, B > 4 ? 8 : B > 2 ? 4 : B);
		return SIFT_CCN_FORBYTE;
	}
	/* the leaves, whose size is a formula of the statistics */
	W = bytes_for(st.range);
	best.size = CC_NHDR + CC_RUP8((Size) n * W);
	if (st.range <= 0xFFFFFFFFu && enc_allowed(w, SIFT_CCN_FORBIT))
	{
		uint32		bits = sift_cc_bits_of(st.range);
		Size		sz = CC_NHDR + CC_RUP8(4 * (Size) sift_cc_words(n, bits));

		if (sz <= best.size)
		{
			best.size = sz;
			tag = SIFT_CCN_FORBIT;
			W = bits;
		}
	}
	if (n >= 2 && st.drange <= 0xFFFFFFFFu && enc_allowed(w, SIFT_CCN_DELTA))
	{
		uint32		bits = sift_cc_bits_of(st.drange);
		Size		sz = CC_NHDR + 8 + 8 * (Size) ((n - 1) / 1024) + CC_RUP8(4 * (Size) sift_cc_words(n, bits));

		if (sz < best.size)
		{
			best.size = sz;
			tag = SIFT_CCN_DELTA;
			W = bits;
		}
	}
	/* the cascades: children are nodes chosen the same way one level down */
	if (depth > 0 && n >= 8)
	{
		if (enc_allowed(w, SIFT_CCN_RLE) && (uint64) st.nruns * 2 <= n)
		{
			rle_build(w, &cand, v, n, st.nruns, depth, role);
			offer(&best, &cand);
		}
		if (enc_allowed(w, SIFT_CCN_DICT) && dict_build(w, &cand, v, n, &st, depth, role))
			offer(&best, &cand);
		if (w->kind != SIFT_CC_INT && enc_allowed(w, SIFT_CCN_ALP) && alp_build(w, &cand, v, n, depth, role))
			offer(&best, &cand);
		if (enc_allowed(w, SIFT_CCN_SCALE) && scale_build(w, &cand, v, n, &st, depth, role))
			offer(&best, &cand);
	}
	if (best.node.data)
	{
		tag = (uint8) best.node.data[0];
		appendBinaryStringInfo(out, best.node.data, best.node.len);
		pfree(best.node.data);
		return tag;
	}
	return emit_leaf(w, out, v, n, &st, tag, W, role);
}

void
sift_cc_encode(StringInfo out, SiftCcKind kind, const int64 *keys, uint32 n, const uint64 *nulls, const SiftCcPolicy *pol, SiftCcInfo *info)
{
	CcW			w = {pol, kind};
	int64	   *v;
	int			hdr = out->len,
				body;
	int64		mn = 0,
				mx = 0,
				prev = 0;
	uint32		nnull = 0;
	bool		any = false;
	uint8		flags = 0;
	int			inner;

	check_policy(pol);
	if (n > SIFT_CC_MAXROWS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("too many rows for one column chunk: %u", n)));
	v = palloc(sizeof(int64) * (n + 1));
	for (uint32 i = 0; i < n; i++)
	{
		if (nulls && ((nulls[i / 64] >> (i % 64)) & 1))
		{
			nnull++;
			v[i] = prev;
			continue;
		}
		if (!any)
		{
			mn = mx = keys[i];
			any = true;
			for (uint32 j = 0; j < i; j++)	/* leading NULL slots take the first value */
				v[j] = keys[i];
		}
		else
		{
			mn = keys[i] < mn ? keys[i] : mn;
			mx = keys[i] > mx ? keys[i] : mx;
		}
		v[i] = prev = keys[i];
	}
	if (!any)
		memset(v, 0, sizeof(int64) * n);
	if ((uint64) mx - (uint64) mn <= 0xFFFFFFFFu)
		flags |= SIFT_CCF_OFF32;
	{
		char		h[SIFT_CC_HDRLEN] = {0};

		h[0] = SIFT_CC_MAGIC;
		h[1] = SIFT_CC_VERSION;
		h[2] = (char) kind;
		h[3] = (char) flags;
		memcpy(h + 4, &n, 4);
		memcpy(h + 8, &mn, 8);
		memcpy(h + 16, &mx, 8);
		appendBinaryStringInfo(out, h, SIFT_CC_HDRLEN);
	}
	body = out->len;
	inner = enc_node(&w, out, v, n, SIFT_CC_MAXDEPTH - 1, CCR_HOT);
	{
		uint32		check = sift_term_hash(out->data + hdr, out->len - hdr);

		put32(out, check);
		put32(out, 0);
	}
	if (info)
	{
		info->n = n;
		info->min = mn;
		info->max = mx;
		info->nnull = nnull;
		info->tag = (uint8) out->data[body];
		info->inner = (uint8) inner;
		info->bytes = (uint32) (out->len - hdr);
	}
	pfree(v);
}
