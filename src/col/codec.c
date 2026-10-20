/*
 * col/codec.c - the chunk codec of the column engine: the writer (statistics, exact-size choice of the lightweight cascade,
 * general-purpose layer under the cost rule), the reader (validation, decoding, filters on the encoded form, random access), the policy
 * parser and the string and bit-vector blobs. Format and rationale:
 *
 * Every node is 8-byte aligned and a multiple of 8 bytes: a 16-byte header (tag, one parameter byte, a u32 count, 8 bytes of fields) and
 * its data; blobs start 8-byte aligned (the engine's chunks do), so all loads below are aligned. Little-endian hosts only, as every
 * on-disk format of pg_sift.
 */
#include "sift_host.h"

#include <math.h>

#include "bm25/analyze.h"
#include "sift_cmp.h"
#include "col/codec_int.h"
#include "col/compress.h"
#include "sift_popcount.h"

#define CC_CORRUPT(...) \
	ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column chunk: " __VA_ARGS__)))

#define CC_BIT(tag)			(1 << (tag))
#define CC_NHDR				16
#define CC_TRAILER			8
#define CC_RUP8(x)			(((x) + 7) & ~(size_t) 7)
#define CC_CAP32(n)			((size_t) SIFT_CC_CAP32(n))

/* ======================================================================== */
/* helpers                                                                  */
/* ======================================================================== */

static void
put32(StringInfo s, uint32 x)
{
	appendBinaryStringInfo(s, (const char *) &x, 4);
}

static void
put64(StringInfo s, int64 x)
{
	appendBinaryStringInfo(s, (const char *) &x, 8);
}

static void
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
get32(const uint8 *p)
{
	uint32		x;

	memcpy(&x, p, 4);
	return x;
}

static int64
get64(const uint8 *p)
{
	int64		x;

	memcpy(&x, p, 8);
	return x;
}

static uint32
bytes_for(uint64 range)
{
	uint32		b = (sift_cc_bits_of(range) + 7) / 8;

	return b ? b : 1;
}

static void
clear_tail(uint64 *bits, uint32 n)
{
	if (n % 64)
		bits[n / 64] &= (UINT64CONST(1) << (n % 64)) - 1;
}

static void
set_ones(uint64 *bits, uint32 n)
{
	for (uint32 w = 0; w < (n + 63) / 64; w++)
		bits[w] = ~UINT64CONST(0);
	clear_tail(bits, n);
}

static void
set_zeros(uint64 *bits, uint32 n)
{
	memset(bits, 0, sizeof(uint64) * ((n + 63) / 64));
}

/* bits [from, to) set */
static void
set_range(uint64 *bits, uint32 from, uint32 to)
{
	while (from < to)
	{
		uint32		w = from / 64,
					lo = from % 64,
					cnt = to - from < 64 - lo ? to - from : 64 - lo;
		uint64		m = cnt == 64 ? ~UINT64CONST(0) : ((UINT64CONST(1) << cnt) - 1) << lo;

		bits[w] |= m;
		from += cnt;
	}
}

/* set bits in [from, to) */
static uint64
count_range(const uint64 *bits, uint32 from, uint32 to)
{
	uint64		c = 0;

	while (from < to)
	{
		uint32		w = from / 64,
					lo = from % 64,
					cnt = to - from < 64 - lo ? to - from : 64 - lo;
		uint64		m = cnt == 64 ? ~UINT64CONST(0) : ((UINT64CONST(1) << cnt) - 1) << lo;

		c += sift_pc64(bits[w] & m);
		from += cnt;
	}
	return c;
}

static uint32
rows_for(uint32 n)
{
	return (uint32) SIFT_CC_ROWS(n);
}

/* ======================================================================== */
/* policy                                                                   */
/* ======================================================================== */

static const struct
{
	const char *name;
	int			tag;
}			cc_enc_names[] = {{"for", SIFT_CCN_FORBIT}, {"delta", SIFT_CCN_DELTA}, {"rle", SIFT_CCN_RLE}, {"dict", SIFT_CCN_DICT}, {"alp", SIFT_CCN_ALP}, {"scale", SIFT_CCN_SCALE}};

void
sift_cc_policy_default(SiftCcPolicy *p)
{
	memset(p, 0, sizeof(*p));
	p->mode = SIFT_CCM_AUTO;
	p->alg = SIFT_CC_ALG_AUTO;
}

/* NULL: not an algorithm; "": taken; else an error message */
static const char *
parse_alg(const char *tok, SiftCcPolicy *p)
{
	const char *colon = strchr(tok, ':');
	size_t		len = colon ? (size_t) (colon - tok) : strlen(tok);
	int			alg = len == 3 && !strncmp(tok, "lz4", 3) ? SIFT_CC_ALG_LZ4 : len == 4 && !strncmp(tok, "zstd", 4) ? SIFT_CC_ALG_ZSTD : 0;
	int			max = alg == SIFT_CC_ALG_LZ4 ? 12 : 22;
	long		level = 0;

	if (!alg)
		return NULL;
	if (p->mode == SIFT_CCM_LAYER)
		return "more than one general-purpose algorithm";
	if (colon)
	{
		char	   *end;

		level = strtol(colon + 1, &end, 10);
		if (end == colon + 1 || *end || level < 1 || level > max)
			return alg == SIFT_CC_ALG_LZ4 ? "lz4 level must be a number between 1 and 12" : "zstd level must be a number between 1 and 22";
	}
	p->alg = (uint8) alg;
	p->level = (uint8) level;
	p->mode = SIFT_CCM_LAYER;
	return "";
}

const char *
sift_cc_policy_parse(const char *spec, SiftCcPolicy *p)
{
	char	   *buf = pstrdup(spec),
			   *tok,
			   *save = NULL;
	const char *err = NULL;
	int			ntok = 0;

	sift_cc_policy_default(p);
	for (tok = strtok_r(buf, "+", &save); tok && !err; tok = strtok_r(NULL, "+", &save), ntok++)
	{
		const char *r;
		bool		found = false;
		char	   *end;

		while (*tok == ' ')
			tok++;
		for (end = tok + strlen(tok); end > tok && end[-1] == ' ';)
			*--end = '\0';
		for (size_t i = 0; i < lengthof(cc_enc_names); i++)
			if (!strcmp(tok, cc_enc_names[i].name))
			{
				p->explicit_encs = 1;
				p->encs |= CC_BIT(cc_enc_names[i].tag);
				found = true;
			}
		if (found || (!strcmp(tok, "auto") && ntok == 0))
			continue;
		if (!strcmp(tok, "none") && ntok == 0)
		{
			p->mode = SIFT_CCM_NONE;
			p->alg = SIFT_CC_ALG_NONE;
			continue;
		}
		r = parse_alg(tok, p);
		if (r == NULL)
			err = "unknown compression: expected auto, none, lz4[:N], zstd[:N] or for, delta, rle, dict, alp joined by '+'";
		else if (*r)
			err = r;
	}
	if (!err && ntok == 0)
		err = "empty compression";
	if (!err && p->mode == SIFT_CCM_NONE && ntok > 1)
		err = "none cannot be combined with other compression settings";
	if (!err && p->explicit_encs && p->mode == SIFT_CCM_AUTO)
		p->alg = SIFT_CC_ALG_NONE;
	pfree(buf);
	return err;
}

const char *
sift_cc_policy_for_column(const char *spec, const char *colname, SiftCcPolicy *p)
{
	char	   *buf = pstrdup(spec),
			   *ent,
			   *save = NULL;
	const char *err = NULL;
	bool		have_default = false;

	sift_cc_policy_default(p);
	for (ent = strtok_r(buf, ";", &save); ent && !err; ent = strtok_r(NULL, ";", &save))
	{
		char	   *eq;

		while (*ent == ' ')
			ent++;
		eq = strchr(ent, '=');
		if (!eq)
		{
			if (have_default)
				err = "more than one default compression";
			else
			{
				err = sift_cc_policy_parse(ent, p);
				have_default = true;
			}
		}
		else
		{
			size_t		l = (size_t) (eq - ent);

			while (l && ent[l - 1] == ' ')
				l--;
			if (l == strlen(colname) && !strncmp(ent, colname, l))
			{
				err = sift_cc_policy_parse(eq + 1, p);
				pfree(buf);
				return err;
			}
		}
	}
	pfree(buf);
	return err;
}

char *
sift_cc_policy_name(const SiftCcPolicy *p)
{
	StringInfoData s;

	if (p->mode == SIFT_CCM_NONE)
		return pstrdup("none");
	initStringInfo(&s);
	if (p->explicit_encs)
		for (size_t i = 0; i < lengthof(cc_enc_names); i++)
			if (p->encs & CC_BIT(cc_enc_names[i].tag))
				appendStringInfo(&s, "%s%s", s.len ? "+" : "", cc_enc_names[i].name);
	if (p->mode == SIFT_CCM_LAYER)
	{
		appendStringInfo(&s, "%s%s", s.len ? "+" : "", p->alg == SIFT_CC_ALG_LZ4 ? "lz4" : "zstd");
		if (p->level)
			appendStringInfo(&s, ":%d", p->level);
	}
	if (!s.len)
		appendStringInfoString(&s, "auto");
	return s.data;
}

/* ======================================================================== */
/* writer                                                                   */
/* ======================================================================== */

typedef enum CcRole
{
	CCR_HOT,					/* streams a scan filters */
	CCR_COLD					/* dictionaries, blobs, exception lists */
} CcRole;

typedef struct CcW
{
	const SiftCcPolicy *pol;
	SiftCcKind	kind;			/* of the root node; children are plain integers */
} CcW;

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
static void
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

static int enc_node(const CcW *w, StringInfo out, const int64 *v, uint32 n, int depth, CcRole role);

/* ---- general-purpose layer ---- */

static double
alg_ns(int alg)
{
	return alg == SIFT_CC_ALG_LZ4 ? SIFT_CC_NS_LZ4 : SIFT_CC_NS_ZSTD;
}

typedef struct CcZTry
{
	int			alg;
	char	   *buf;
	Size		len;
	double		cost;
} CcZTry;

/* The best wrapper the policy accepts for the stream raw[0..rawlen) (best->buf palloc'd), or false */
static bool
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

		while (lo < hi)
		{
			uint32		m = (lo + hi) / 2;

			if (dv[m] < v[i])
				lo = m + 1;
			else
				hi = m;
		}
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
static int
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

/* ======================================================================== */
/* reader                                                                   */
/* ======================================================================== */

typedef struct CcNode
{
	uint8		tag;
	uint8		alg;
	uint8		kind;
	uint8		W,
				B,
				e;
	uint32		n;
	int64		base,			/* FORBIT, FORBYTE; CONST value; DELTA first */
				mind,
				dmin,
				dmax;
	uint32		nruns,
				d,
				nexc;
	const uint8 *words;			/* packed words (FORBIT, DELTA) or bytes (FORBYTE) */
	const uint8 *chk;
	const uint8 *ekeys;
	struct CcNode *a,
			   *b;
	uint32	   *rstart;			/* RLE: nruns + 1 */
	int64	   *dict;			/* DICT: d, ascending */
	uint32	   *pos;			/* ALP: nexc, ascending */
} CcNode;

struct SiftCc
{
	MemoryContext mcxt;
	SiftCcKind	kind;
	uint32		n;
	int64		min,
				max;
	uint8		flags;
	CcNode	   *root;
};

static void *
rd_alloc(MemoryContext cx, size_t sz)
{
	return MemoryContextAllocZero(cx, sz);
}

static void nd_u32(const CcNode *nd, int64 sub, uint32 *dst);
static void nd_i64(const CcNode *nd, int64 *dst);
static CcNode *parse_node(MemoryContext cx, SiftCcKind kind, const uint8 *p, size_t avail, uint32 n, int depth, size_t *used);

static void
need(size_t have, size_t want)
{
	if (have < want)
		CC_CORRUPT("node is truncated");
}

static CcNode *
parse_child(MemoryContext cx, SiftCcKind kind, const uint8 *p, size_t len, uint32 n, int depth)
{
	size_t		used;
	CcNode	   *nd;

	if (len & 7)
		CC_CORRUPT("child length is not a multiple of 8");
	nd = parse_node(cx, kind, p, len, n, depth, &used);
	if (used != len)
		CC_CORRUPT("child node length does not match");
	return nd;
}

/* Parses one node at p (8-aligned) of n values; *used is its size */
static CcNode *
parse_node(MemoryContext cx, SiftCcKind kind, const uint8 *p, size_t avail, uint32 n, int depth, size_t *used)
{
	CcNode	   *nd;
	size_t		len;
	uint32		f1;

	if (depth > SIFT_CC_MAXDEPTH)
		CC_CORRUPT("nodes nested too deep");
	need(avail, CC_NHDR);
	nd = rd_alloc(cx, sizeof(CcNode));
	nd->tag = p[0];
	nd->kind = (uint8) kind;
	nd->n = n;
	f1 = get32(p + 4);
	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			nd->base = get64(p + 8);
			len = CC_NHDR;
			break;
		case SIFT_CCN_FORBIT:
			nd->W = p[1];
			if (nd->W < 1 || nd->W > 32)
				CC_CORRUPT("bit width out of range");
			nd->base = get64(p + 8);
			len = CC_NHDR + CC_RUP8(4 * (size_t) sift_cc_words(n, nd->W));
			need(avail, len);
			nd->words = p + CC_NHDR;
			break;
		case SIFT_CCN_FORBYTE:
			nd->B = p[1];
			if (nd->B < 1 || nd->B > 8)
				CC_CORRUPT("byte width out of range");
			nd->base = get64(p + 8);
			len = CC_NHDR + CC_RUP8((size_t) n * nd->B);
			need(avail, len);
			nd->words = p + CC_NHDR;
			break;
		case SIFT_CCN_DELTA:
			if (n < 2)
				CC_CORRUPT("delta node with fewer than two values");
			nd->W = p[1];
			if (nd->W > 32)
				CC_CORRUPT("bit width above 32");
			need(avail, CC_NHDR + 8);
			nd->base = get64(p + 8);
			nd->mind = get64(p + 16);
			len = CC_NHDR + 8 + 8 * (size_t) ((n - 1) / 1024) + CC_RUP8(4 * (size_t) sift_cc_words(n, nd->W));
			need(avail, len);
			nd->chk = p + CC_NHDR + 8;
			nd->words = nd->chk + 8 * (size_t) ((n - 1) / 1024);
			break;
		case SIFT_CCN_RLE:
			{
				uint32		vl = get32(p + 8),
							ll = get32(p + 12);
				uint32	   *lens;
				uint64		sum = 0;

				nd->nruns = f1;
				if (nd->nruns < 1 || nd->nruns > n)
					CC_CORRUPT("run count out of range");
				len = CC_NHDR + (size_t) vl + ll;
				need(avail, len);
				nd->a = parse_child(cx, SIFT_CC_INT, p + CC_NHDR, vl, nd->nruns, depth + 1);
				nd->b = parse_child(cx, SIFT_CC_INT, p + CC_NHDR + vl, ll, nd->nruns, depth + 1);
				lens = rd_alloc(cx, sizeof(uint32) * CC_CAP32(nd->nruns));
				nd_u32(nd->b, 0, lens);
				nd->rstart = rd_alloc(cx, sizeof(uint32) * (nd->nruns + 1));
				for (uint32 r = 0; r < nd->nruns; r++)
				{
					if (lens[r] == 0)
						CC_CORRUPT("empty run");
					nd->rstart[r] = (uint32) sum;
					sum += lens[r];
					if (sum > n)
						CC_CORRUPT("runs longer than the chunk");
				}
				if (sum != n)
					CC_CORRUPT("runs shorter than the chunk");
				nd->rstart[nd->nruns] = n;
				pfree(lens);
				break;
			}
		case SIFT_CCN_DICT:
			{
				uint32		dl = get32(p + 8),
							cl = get32(p + 12);

				nd->d = f1;
				if (nd->d < 1 || nd->d > n)
					CC_CORRUPT("dictionary size out of range");
				len = CC_NHDR + (size_t) dl + cl;
				need(avail, len);
				nd->a = parse_child(cx, SIFT_CC_INT, p + CC_NHDR, dl, nd->d, depth + 1);
				nd->b = parse_child(cx, SIFT_CC_INT, p + CC_NHDR + dl, cl, n, depth + 1);
				nd->dict = rd_alloc(cx, sizeof(int64) * (nd->d + 8));
				nd_i64(nd->a, nd->dict);
				for (uint32 i = 1; i < nd->d; i++)
					if (nd->dict[i] <= nd->dict[i - 1])
						CC_CORRUPT("dictionary is not sorted");
				break;
			}
		case SIFT_CCN_ALP:
			{
				uint32		dl = get32(p + 8),
							pl = get32(p + 12);
				int			emax = kind == SIFT_CC_F8 ? SIFT_CC_ALP_EMAX_F8 : SIFT_CC_ALP_EMAX_F4;

				if (kind == SIFT_CC_INT)
					CC_CORRUPT("decimal node in an integer chunk");
				need(avail, 32);
				nd->e = p[1];
				nd->nexc = f1;
				nd->dmin = get64(p + 16);
				nd->dmax = get64(p + 24);
				int64		lim = kind == SIFT_CC_F8 ? ((int64) 1 << 52) : ((int64) 1 << 24);

				if (nd->e > emax || nd->nexc > n || nd->dmax < nd->dmin || nd->dmin < -lim || nd->dmax > lim ||
					(uint64) nd->dmax - (uint64) nd->dmin > 0xFFFFFFFFu)
					CC_CORRUPT("decimal node parameters out of range");
				if ((nd->nexc == 0) != (pl == 0))
					CC_CORRUPT("exception list length does not match");
				len = 32 + (size_t) dl + pl + 8 * (size_t) nd->nexc;
				need(avail, len);
				nd->a = parse_child(cx, SIFT_CC_INT, p + 32, dl, n, depth + 1);
				if (nd->nexc)
				{
					int64	   *tmp = rd_alloc(cx, sizeof(int64) * (nd->nexc + 8));

					nd->b = parse_child(cx, SIFT_CC_INT, p + 32 + dl, pl, nd->nexc, depth + 1);
					nd_i64(nd->b, tmp);
					nd->pos = rd_alloc(cx, sizeof(uint32) * nd->nexc);
					for (uint32 i = 0; i < nd->nexc; i++)
					{
						if (tmp[i] < 0 || tmp[i] >= (int64) n || (i && tmp[i] <= tmp[i - 1]))
							CC_CORRUPT("exception positions out of order");
						nd->pos[i] = (uint32) tmp[i];
					}
					pfree(tmp);
				}
				nd->ekeys = p + 32 + dl + pl;
				break;
			}
		case SIFT_CCN_SCALE:
			need(avail, 24);
			nd->base = get64(p + 8);
			nd->mind = get64(p + 16);	/* the divisor */
			if (nd->mind < 2)
				CC_CORRUPT("scale factor below 2");
			len = 24 + (size_t) f1;
			need(avail, len);
			nd->a = parse_child(cx, SIFT_CC_INT, p + 24, f1, n, depth + 1);
			break;
		case SIFT_CCN_WRAP:
			{
				uint32		cl = get32(p + 8);
				uint8	   *buf;
				size_t		u;

				nd->alg = p[1];
				len = CC_NHDR + CC_RUP8(cl);
				need(avail, len);
				if (!sift_colz_available(nd->alg))
					ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									errmsg("column chunk uses compression algorithm %d, which this build does not provide", nd->alg)));
				if (f1 < CC_NHDR || (f1 & 7) || f1 > (uint64) n * 9 + 4096 + 64)
					CC_CORRUPT("wrapped length out of range");
				buf = rd_alloc(cx, (size_t) f1 + SIFT_COL_PAD);
				if (!sift_colz_decompress(nd->alg, p + CC_NHDR, cl, buf, f1))
					CC_CORRUPT("compressed stream is damaged");
				nd->a = parse_node(cx, kind, buf, f1, n, depth + 1, &u);
				if (u != f1)
					CC_CORRUPT("wrapped node length does not match");
				break;
			}
		default:
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("column chunk uses encoding %d, which this build does not know", nd->tag)));
	}
	*used = len;
	return nd;
}

SiftCc *
sift_cc_open(MemoryContext mcxt, const uint8 *blob, Size len, bool verify)
{
	MemoryContext cx;
	SiftCc	   *c;
	size_t		used;

	if (len < SIFT_CC_HDRLEN + CC_NHDR + CC_TRAILER || (len & 7) || ((uintptr_t) blob & 7) || blob[0] != SIFT_CC_MAGIC)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column chunk: bad header or misaligned blob")));
	if (blob[1] != SIFT_CC_VERSION)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("column chunk has format version %d, this build reads %d", blob[1], SIFT_CC_VERSION)));
	if (blob[2] > SIFT_CC_F8)
		CC_CORRUPT("unknown value kind");
	if (verify && sift_term_hash((const char *) blob, (int) (len - CC_TRAILER)) != get32(blob + len - CC_TRAILER))
		CC_CORRUPT("check word mismatch");
	cx = AllocSetContextCreate(mcxt, "sift column chunk", ALLOCSET_SMALL_SIZES);
	c = MemoryContextAllocZero(cx, sizeof(SiftCc));
	c->mcxt = cx;
	c->kind = (SiftCcKind) blob[2];
	c->flags = blob[3];
	c->n = get32(blob + 4);
	c->min = get64(blob + 8);
	c->max = get64(blob + 16);
	if (c->n > SIFT_CC_MAXROWS || c->max < c->min)
		CC_CORRUPT("row count or range out of bounds");
	if (((c->flags & SIFT_CCF_OFF32) != 0) != ((uint64) c->max - (uint64) c->min <= 0xFFFFFFFFu))
		CC_CORRUPT("offset flag contradicts the range");
	c->root = parse_node(cx, c->kind, blob + SIFT_CC_HDRLEN, len - SIFT_CC_HDRLEN - CC_TRAILER, c->n, 0, &used);
	if (used != len - SIFT_CC_HDRLEN - CC_TRAILER)
		CC_CORRUPT("trailing bytes after the root node");
	return c;
}

void
sift_cc_close(SiftCc *c)
{
	MemoryContextDelete(c->mcxt);
}

SiftCcKind
sift_cc_kind(const SiftCc *c)
{
	return c->kind;
}

uint32
sift_cc_nrows(const SiftCc *c)
{
	return c->n;
}

int64
sift_cc_min(const SiftCc *c)
{
	return c->min;
}

int64
sift_cc_max(const SiftCc *c)
{
	return c->max;
}

static const CcNode *
unwrap(const CcNode *nd)
{
	while (nd->tag == SIFT_CCN_WRAP)
		nd = nd->a;
	return nd;
}

uint8
sift_cc_tag(const SiftCc *c)
{
	return unwrap(c->root)->tag;
}

uint32
sift_cc_off32_bound(const SiftCc *c)
{
	const CcNode *nd = unwrap(c->root);
	uint64		off = (uint32) ((uint64) nd->base - (uint64) c->min),
				span;

	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			span = 0;
			break;
		case SIFT_CCN_FORBIT:
			span = nd->W < 32 ? (UINT64CONST(1) << nd->W) - 1 : UINT32_MAX;
			break;
		case SIFT_CCN_FORBYTE:
			span = nd->B < 4 ? (UINT64CONST(1) << (8 * nd->B)) - 1 : UINT32_MAX;
			break;
		default:
			return UINT32_MAX;
	}
	return Min(off + span, UINT32_MAX);
}

/* ---- decoding ---- */

/* The B-byte little-endian values of a FORBYTE node plus a base, wrapped to the domain: a loop per width, so the compiler vectorizes it */
#define CC_BYTES_LOOP(T, expr) \
	for (uint32 i = 0; i < n; i++) \
	{ \
		T			x; \
		memcpy(&x, s + (size_t) i * sizeof(T), sizeof(T)); \
		dst[i] = (expr); \
	}

static void
forbyte_u32(const CcNode *nd, uint32 off, uint32 *dst)
{
	const uint8 *s = nd->words;
	uint32		n = nd->n;

	switch (nd->B)
	{
		case 1:
			CC_BYTES_LOOP(uint8, off + x) break;
		case 2:
			CC_BYTES_LOOP(uint16, off + x) break;
		case 4:
			CC_BYTES_LOOP(uint32, off + x) break;
		default:
			for (uint32 i = 0; i < n; i++)
			{
				uint64		x = 0;

				memcpy(&x, s + (size_t) i * nd->B, nd->B);
				dst[i] = off + (uint32) x;
			}
	}
}

static void
forbyte_i64(const CcNode *nd, int64 *dst)
{
	const uint8 *s = nd->words;
	uint32		n = nd->n;
	uint64		base = (uint64) nd->base;

	switch (nd->B)
	{
		case 1:
			CC_BYTES_LOOP(uint8, (int64) (base + x)) break;
		case 2:
			CC_BYTES_LOOP(uint16, (int64) (base + x)) break;
		case 4:
			CC_BYTES_LOOP(uint32, (int64) (base + x)) break;
		case 8:
			CC_BYTES_LOOP(uint64, (int64) (base + x)) break;
		default:
			for (uint32 i = 0; i < n; i++)
			{
				uint64		x = 0;

				memcpy(&x, s + (size_t) i * nd->B, nd->B);
				dst[i] = (int64) (base + x);
			}
	}
}

/* the u32 domain: dst[i] = value i - sub, for nodes whose values all lie in [sub, sub + 2^32); dst has CC_CAP32(n) slots */
static void
nd_u32(const CcNode *nd, int64 sub, uint32 *dst)
{
	uint32		n = nd->n;
	uint32		off = (uint32) ((uint64) nd->base - (uint64) sub);

	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			for (uint32 i = 0; i < n; i++)
				dst[i] = off;
			break;
		case SIFT_CCN_FORBIT:
			sift_cck->unpack32((const uint32 *) nd->words, nd->W, rows_for(n), off, dst);
			break;
		case SIFT_CCN_FORBYTE:
			forbyte_u32(nd, off, dst);
			break;
		case SIFT_CCN_DELTA:
			{
				uint32	   *tmp = palloc(sizeof(uint32) * CC_CAP32(n));

				sift_cck->unpack32((const uint32 *) nd->words, nd->W, rows_for(n), 0, tmp);
				sift_cck->prefix32(tmp, n, dst, off, (uint32) nd->mind);
				pfree(tmp);
				break;
			}
		case SIFT_CCN_RLE:
			{
				uint32	   *rv = palloc(sizeof(uint32) * CC_CAP32(nd->nruns)),
						   *rl = palloc(sizeof(uint32) * CC_CAP32(nd->nruns));

				nd_u32(nd->a, sub, rv);
				for (uint32 r = 0; r < nd->nruns; r++)
					rl[r] = nd->rstart[r + 1] - nd->rstart[r];
				sift_cck->rle_expand32(rv, rl, nd->nruns, dst);
				pfree(rv);
				pfree(rl);
				break;
			}
		case SIFT_CCN_DICT:
			{
				uint32	   *dv = palloc(sizeof(uint32) * CC_CAP32(nd->d)),
						   *codes = palloc(sizeof(uint32) * CC_CAP32(n));

				for (uint32 i = 0; i < nd->d; i++)
					dv[i] = (uint32) ((uint64) nd->dict[i] - (uint64) sub);
				nd_u32(nd->b, 0, codes);
				if (sift_cck->max32(codes, n) >= nd->d)
					CC_CORRUPT("dictionary code out of range");
				sift_colk->gather_u32(dv, codes, n, dst);
				pfree(dv);
				pfree(codes);
				break;
			}
		case SIFT_CCN_SCALE:
			nd_u32(nd->a, 0, dst);
			sift_cck->scale32(dst, n, (uint32) ((uint64) nd->base - (uint64) sub), (uint32) nd->mind);
			break;
		case SIFT_CCN_WRAP:
			nd_u32(nd->a, sub, dst);
			break;
		default:
			{
				int64	   *tmp = palloc(sizeof(int64) * ((size_t) n + SIFT_COL_PAD / 8));

				nd_i64(nd, tmp);
				for (uint32 i = 0; i < n; i++)
					dst[i] = (uint32) ((uint64) tmp[i] - (uint64) sub);
				pfree(tmp);
				break;
			}
	}
}

static void
nd_i64(const CcNode *nd, int64 *dst)
{
	uint32		n = nd->n;

	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			for (uint32 i = 0; i < n; i++)
				dst[i] = nd->base;
			break;
		case SIFT_CCN_FORBIT:
			{
				uint32	   *tmp = palloc(sizeof(uint32) * CC_CAP32(n));

				sift_cck->unpack32((const uint32 *) nd->words, nd->W, rows_for(n), 0, tmp);
				sift_colk->for_i64[SIFT_COLW_32](tmp, n, nd->base, dst);
				pfree(tmp);
				break;
			}
		case SIFT_CCN_FORBYTE:
			forbyte_i64(nd, dst);
			break;
		case SIFT_CCN_DELTA:
			{
				uint32	   *tmp = palloc(sizeof(uint32) * CC_CAP32(n));

				sift_cck->unpack32((const uint32 *) nd->words, nd->W, rows_for(n), 0, tmp);
				sift_cck->prefix64(tmp, n, dst, nd->base, nd->mind);
				pfree(tmp);
				break;
			}
		case SIFT_CCN_RLE:
			{
				int64	   *rv = palloc(sizeof(int64) * (nd->nruns + SIFT_COL_PAD / 8));

				nd_i64(nd->a, rv);
				for (uint32 r = 0; r < nd->nruns; r++)
					for (uint32 j = nd->rstart[r]; j < nd->rstart[r + 1]; j++)
						dst[j] = rv[r];
				pfree(rv);
				break;
			}
		case SIFT_CCN_DICT:
			{
				uint32	   *codes = palloc(sizeof(uint32) * CC_CAP32(n));

				nd_u32(nd->b, 0, codes);
				if (sift_cck->max32(codes, n) >= nd->d)
					CC_CORRUPT("dictionary code out of range");
				sift_colk->for_i64_sel[SIFT_COLW_64](nd->dict, codes, n, 0, dst);
				pfree(codes);
				break;
			}
		case SIFT_CCN_ALP:
			{
				uint32	   *dg = palloc(sizeof(uint32) * CC_CAP32(n));

				nd_u32(nd->a, nd->dmin, dg);
				if (nd->kind == SIFT_CC_F8)
					sift_cck->alp_keys_f8(dst, dg, n, nd->dmin, sift_cc_p10[nd->e]);
				else
					sift_cck->alp_keys_f4(dst, dg, n, nd->dmin, sift_cc_p10f[nd->e]);
				for (uint32 k = 0; k < nd->nexc; k++)
					memcpy(dst + nd->pos[k], nd->ekeys + 8 * (size_t) k, 8);
				pfree(dg);
				break;
			}
		case SIFT_CCN_SCALE:
			nd_i64(nd->a, dst);
			sift_cck->scale64(dst, n, nd->base, nd->mind);
			break;
		case SIFT_CCN_WRAP:
			nd_i64(nd->a, dst);
			break;
	}
}

void
sift_cc_decode(const SiftCc *c, int64 *dst)
{
	if (c->n)
		nd_i64(c->root, dst);
}

/* The doubles of the keys: an ALP node is divided straight into doubles (its exceptions are keys), any other node decodes the keys and turns them in place */
static void
nd_f8(const CcNode *nd, double *dst)
{
	uint32		n = nd->n;

	if (nd->tag == SIFT_CCN_WRAP)
		nd_f8(nd->a, dst);
	else if (nd->tag == SIFT_CCN_ALP)
	{
		uint32	   *dg = palloc(sizeof(uint32) * CC_CAP32(n));

		nd_u32(nd->a, nd->dmin, dg);
		if (nd->kind == SIFT_CC_F8)
			sift_cck->alp_f8(dst, dg, n, nd->dmin, sift_cc_p10[nd->e]);
		else
			sift_cck->alp_f4(dst, dg, n, nd->dmin, sift_cc_p10f[nd->e]);
		for (uint32 k = 0; k < nd->nexc; k++)
			dst[nd->pos[k]] = sift_col_f8_from_key((int64) get64(nd->ekeys + 8 * (size_t) k));
		pfree(dg);
	}
	else
	{
		nd_i64(nd, (int64 *) dst);
		sift_colk->f8_from_keys((int64 *) dst, n);
	}
}

void
sift_cc_decode_f8(const SiftCc *c, double *dst)
{
	if (c->n)
		nd_f8(c->root, dst);
}

void
sift_cc_decode_u32(const SiftCc *c, uint32 *dst)
{
	if (c->n)
		nd_u32(c->root, 0, dst);
}

bool
sift_cc_decode_off32(const SiftCc *c, uint32 *dst)
{
	if (!(c->flags & SIFT_CCF_OFF32))
		return false;
	if (c->n)
		nd_u32(c->root, c->min, dst);
	return true;
}

/* ---- random access ---- */

static uint32
forbit_at(const uint8 *words, uint32 W, uint32 i)
{
	uint32		blk = i / 1024,
				j = i % 1024,
				r = j / 32,
				l = j % 32,
				bp = r * W,
				s = bp & 31;
	const uint32 *a = (const uint32 *) words + (size_t) blk * 32 * W + (size_t) (bp >> 5) * 32;
	uint32		x = sift_cc_ld32(a + l) >> s;

	if (s + W > 32)
		x |= sift_cc_ld32(a + 32 + l) << (32 - s);
	return W >= 32 ? x : x & ((1u << W) - 1);
}

static int64
alp_key(const CcNode *nd, int64 d)
{
	if (nd->kind == SIFT_CC_F8)
		return sift_col_f8_key((double) d / sift_cc_p10[nd->e]);
	return sift_col_f8_key((double) ((float) (int32) d / sift_cc_p10f[nd->e]));
}

static int64
nd_get(const CcNode *nd, uint32 i)
{
	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			return nd->base;
		case SIFT_CCN_FORBIT:
			return (int64) ((uint64) nd->base + forbit_at(nd->words, nd->W, i));
		case SIFT_CCN_FORBYTE:
			{
				uint64		x = 0;

				memcpy(&x, nd->words + (size_t) i * nd->B, nd->B);
				return (int64) ((uint64) nd->base + x);
			}
		case SIFT_CCN_DELTA:
			{
				uint32		blk = i / 1024,
							upto = i % 1024;
				uint64		run = (uint64) (blk ? get64(nd->chk + 8 * (size_t) (blk - 1)) : nd->base);

				if (upto)
				{
					uint32		tmp[1024 + SIFT_COL_PAD / 4];

					sift_cck->unpack32((const uint32 *) nd->words + (size_t) blk * 32 * nd->W, nd->W, upto / 32 + 1, 0, tmp);
					for (uint32 j = 1; j <= upto; j++)
						run += (uint64) tmp[j] + (uint64) nd->mind;
				}
				return (int64) run;
			}
		case SIFT_CCN_RLE:
			{
				uint32		lo = 0,
							hi = nd->nruns;

				while (hi - lo > 1)
				{
					uint32		m = (lo + hi) / 2;

					if (nd->rstart[m] <= i)
						lo = m;
					else
						hi = m;
				}
				return nd_get(nd->a, lo);
			}
		case SIFT_CCN_DICT:
			{
				int64		code = nd_get(nd->b, i);

				if (code < 0 || code >= (int64) nd->d)
					CC_CORRUPT("dictionary code out of range");
				return nd->dict[code];
			}
		case SIFT_CCN_ALP:
			{
				uint32		lo = 0,
							hi = nd->nexc;

				while (lo < hi)
				{
					uint32		m = (lo + hi) / 2;

					if (nd->pos[m] < i)
						lo = m + 1;
					else
						hi = m;
				}
				if (lo < nd->nexc && nd->pos[lo] == i)
					return get64(nd->ekeys + 8 * (size_t) lo);
				return alp_key(nd, nd_get(nd->a, i));
			}
		case SIFT_CCN_SCALE:
			return (int64) ((uint64) nd->base + (uint64) nd->mind * (uint64) nd_get(nd->a, i));
		case SIFT_CCN_WRAP:
			return nd_get(nd->a, i);
	}
	return 0;
}

int64
sift_cc_get(const SiftCc *c, uint32 row)
{
	if (row >= c->n)
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("row %u is past the end of a column chunk of %u rows", row, c->n)));
	return nd_get(c->root, row);
}

/* A delta-coded value is the sum of the deltas before it in its block of 1024: a lookup costs a block's prefix, not a few operations */
static bool
nd_has_delta(const CcNode *nd)
{
	return nd != NULL && (nd->tag == SIFT_CCN_DELTA || nd_has_delta(nd->a) || nd_has_delta(nd->b));
}

void
sift_cc_decode_sel(const SiftCc *c, const uint32 *idx, uint32 m, int64 *dst)
{
	if (m == 0)
		return;
	if ((uint64) m * (nd_has_delta(c->root) ? 256 : 8) > c->n)
	{
		int64	   *all = palloc(sizeof(int64) * ((size_t) c->n + SIFT_COL_PAD / 8));

		nd_i64(c->root, all);
		for (uint32 k = 0; k < m; k++)
		{
			if (idx[k] >= c->n)
				ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("row %u is past the end of a column chunk of %u rows", idx[k], c->n)));
			dst[k] = all[idx[k]];
		}
		pfree(all);
		return;
	}
	for (uint32 k = 0; k < m; k++)
		dst[k] = sift_cc_get(c, idx[k]);
}

/* ---- filters on the encoded form ---- */

/*
 * [lo, hi] as offsets from base within width bits (maxoff = largest offset, or UINT64_MAX): false when nothing can match, else the
 * offsets in *lo_off and *span (hi - lo)
 */
static bool
off_range(int64 base, uint64 maxoff, int64 lo, int64 hi, uint64 *lo_off, uint64 *span)
{
	uint64		l,
				h;

	if (hi < base)
		return false;
	l = lo <= base ? 0 : (uint64) lo - (uint64) base;
	h = (uint64) hi - (uint64) base;
	if (l > maxoff)
		return false;
	if (h > maxoff)
		h = maxoff;
	*lo_off = l;
	*span = h - l;
	return true;
}

static void nd_filter(const CcNode *nd, int64 lo, int64 hi, int64 sub, bool off32, uint64 *bits);

/* digits d with key(decode(d)) in [lo, hi] form one interval of [dmin, dmax] (decode is monotone); *dlo > *dhi when empty */
static void
alp_digit_range(const CcNode *nd, int64 lo, int64 hi, int64 *dlo, int64 *dhi)
{
	int64		a = nd->dmin,
				b = nd->dmax + 1;

	while (a < b)
	{
		int64		m = a + (b - a) / 2;

		if (alp_key(nd, m) < lo)
			a = m + 1;
		else
			b = m;
	}
	*dlo = a;
	a = nd->dmin;
	b = nd->dmax + 1;
	while (a < b)
	{
		int64		m = a + (b - a) / 2;

		if (alp_key(nd, m) <= hi)
			a = m + 1;
		else
			b = m;
	}
	*dhi = a - 1;
}

static void
nd_filter(const CcNode *nd, int64 lo, int64 hi, int64 sub, bool off32, uint64 *bits)
{
	uint32		n = nd->n;
	uint64		l,
				span;

	if (lo > hi)
	{
		set_zeros(bits, n);
		return;
	}
	switch (nd->tag)
	{
		case SIFT_CCN_CONST:
			if (nd->base >= lo && nd->base <= hi)
				set_ones(bits, n);
			else
				set_zeros(bits, n);
			return;
		case SIFT_CCN_FORBIT:
			if (!off_range(nd->base, nd->W >= 32 ? 0xFFFFFFFFu : (UINT64CONST(1) << nd->W) - 1, lo, hi, &l, &span))
			{
				set_zeros(bits, n);
				return;
			}
			sift_cck->unpack_range32((const uint32 *) nd->words, nd->W, rows_for(n), (uint32) l, (uint32) span, bits);
			clear_tail(bits, n);
			return;
		case SIFT_CCN_FORBYTE:
			if (nd->B == 1 || nd->B == 2 || nd->B == 4 || nd->B == 8)
			{
				if (!off_range(nd->base, nd->B == 8 ? ~UINT64CONST(0) : (UINT64CONST(1) << (8 * nd->B)) - 1, lo, hi, &l, &span))
				{
					set_zeros(bits, n);
					return;
				}
				sift_colk->range[nd->B == 1 ? 0 : nd->B == 2 ? 1 : nd->B == 4 ? 2 : 3] (nd->words, n, l, span, bits);
				return;
			}
			break;
		case SIFT_CCN_RLE:
			{
				uint64	   *rb = palloc0(sizeof(uint64) * SIFT_COL_NWORDS(nd->nruns));
				uint32		start = 0;
				bool		open = false;

				nd_filter(nd->a, lo, hi, sub, off32, rb);
				set_zeros(bits, n);
				for (uint32 r = 0; r < nd->nruns; r++)
				{
					bool		hit = (rb[r / 64] >> (r % 64)) & 1;

					if (hit && !open)
					{
						start = nd->rstart[r];
						open = true;
					}
					else if (!hit && open)
					{
						set_range(bits, start, nd->rstart[r]);
						open = false;
					}
				}
				if (open)
					set_range(bits, start, n);
				pfree(rb);
				return;
			}
		case SIFT_CCN_DICT:
			{
				uint32		a = 0,
							b = nd->d,
							cl,
							ch;

				while (a < b)
				{
					uint32		m = (a + b) / 2;

					if (nd->dict[m] < lo)
						a = m + 1;
					else
						b = m;
				}
				cl = a;
				b = nd->d;
				while (a < b)
				{
					uint32		m = (a + b) / 2;

					if (nd->dict[m] <= hi)
						a = m + 1;
					else
						b = m;
				}
				ch = a;
				if (cl >= ch)
					set_zeros(bits, n);
				else
					nd_filter(nd->b, cl, (int64) ch - 1, 0, true, bits);
				return;
			}
		case SIFT_CCN_ALP:
			{
				int64		dlo,
							dhi;

				alp_digit_range(nd, lo, hi, &dlo, &dhi);
				if (dlo > dhi)
					set_zeros(bits, n);
				else
					nd_filter(nd->a, dlo, dhi, nd->dmin, true, bits);
				for (uint32 k = 0; k < nd->nexc; k++)
				{
					uint32		p = nd->pos[k];
					int64		key = get64(nd->ekeys + 8 * (size_t) k);

					bits[p / 64] &= ~(UINT64CONST(1) << (p % 64));
					if (key >= lo && key <= hi)
						bits[p / 64] |= UINT64CONST(1) << (p % 64);
				}
				return;
			}
		case SIFT_CCN_SCALE:
			{
				/* base + g * w in [lo, hi]  <=>  w in [ceil((lo - base) / g), floor((hi - base) / g)] */
				uint64		g = (uint64) nd->mind,
							d = lo <= nd->base ? 0 : (uint64) lo - (uint64) nd->base,
							wlo = d / g + (d % g != 0),
							whi;

				if (hi < nd->base)
					whi = 0, wlo = 1;
				else
					whi = ((uint64) hi - (uint64) nd->base) / g;
				if (wlo > whi || wlo > (uint64) INT64_MAX)
					set_zeros(bits, n);
				else
					nd_filter(nd->a, (int64) wlo, (int64) whi, 0, off32, bits);
				return;
			}
		case SIFT_CCN_WRAP:
			nd_filter(nd->a, lo, hi, sub, off32, bits);
			return;
	}
	/* DELTA, and FORBYTE of an odd width: decode, then the range kernel on the plain array */
	if (off32)
	{
		uint32	   *tmp = palloc(sizeof(uint32) * CC_CAP32(n));

		if (!off_range(sub, 0xFFFFFFFFu, lo, hi, &l, &span))
			set_zeros(bits, n);
		else
		{
			nd_u32(nd, sub, tmp);
			sift_colk->range[2](tmp, n, l, span, bits);
		}
		pfree(tmp);
	}
	else
	{
		int64	   *tmp = palloc(sizeof(int64) * ((size_t) n + SIFT_COL_PAD / 8));

		nd_i64(nd, tmp);
		sift_colk->range[3](tmp, n, (uint64) lo, (uint64) hi - (uint64) lo, bits);
		pfree(tmp);
	}
}

void
sift_cc_filter(const SiftCc *c, int64 lo, int64 hi, uint64 *bits)
{
	if (c->n == 0)
		return;
	if (lo > hi || hi < c->min || lo > c->max)
		set_zeros(bits, c->n);
	else if (lo <= c->min && hi >= c->max)
		set_ones(bits, c->n);
	else
		nd_filter(c->root, lo, hi, c->min, (c->flags & SIFT_CCF_OFF32) != 0, bits);
}

uint64
sift_cc_sum(const SiftCc *c, const uint64 *bits, int64 *sum)
{
	const CcNode *nd = unwrap(c->root);
	uint64		cnt = 0;
	uint64		s = 0;
	uint32		n = c->n;

	if (n == 0)
	{
		*sum = 0;
		return 0;
	}
	cnt = bits ? count_range(bits, 0, n) : n;
	if (nd->tag == SIFT_CCN_CONST)
		s = cnt * (uint64) nd->base;
	else if (nd->tag == SIFT_CCN_RLE)
	{
		int64	   *rv = palloc(sizeof(int64) * (nd->nruns + SIFT_COL_PAD / 8));

		nd_i64(nd->a, rv);
		for (uint32 r = 0; r < nd->nruns; r++)
			s += (uint64) rv[r] * (bits ? count_range(bits, nd->rstart[r], nd->rstart[r + 1]) : nd->rstart[r + 1] - nd->rstart[r]);
		pfree(rv);
	}
	else
	{
		int64	   *all = palloc(sizeof(int64) * ((size_t) n + SIFT_COL_PAD / 8));

		nd_i64(nd, all);
		for (uint32 i = 0; i < n; i++)
			if (!bits || ((bits[i / 64] >> (i % 64)) & 1))
				s += (uint64) all[i];
		pfree(all);
	}
	*sum = (int64) s;
	return cnt;
}

/* ======================================================================== */
/* string lists and bit vectors                                             */
/* ======================================================================== */

#define CC_STRS_MAGIC	0xC6
#define CC_BITS_MAGIC	0xC7

enum
{
	CCB_ZEROS,
	CCB_ONES,
	CCB_RAW,
	CCB_SET,					/* positions of the set bits */
	CCB_CLEAR					/* positions of the clear bits */
};

/*
 * String list: 8-byte header (magic, version, u32 n, u32 length of the lengths node), the lengths node (an integer chunk node, cold), then
 * the bytes: u32 form (0 raw, else the algorithm id), u32 rawlen, u32 clen, data padded to 8.
 */
void
sift_cc_encode_strs(StringInfo out, const uint32 *off, const char *bytes, uint32 n, const SiftCcPolicy *pol)
{
	CcW			w = {pol, SIFT_CC_INT};
	int64	   *lens = palloc(sizeof(int64) * (n + 1));
	StringInfoData ln;
	uint32		total = n ? off[n] - off[0] : 0;
	CcZTry		z;
	const uint8 *src = (const uint8 *) bytes + (n ? off[0] : 0);

	check_policy(pol);
	for (uint32 i = 0; i < n; i++)
		lens[i] = off[i + 1] - off[i];
	initStringInfo(&ln);
	enc_node(&w, &ln, lens, n, SIFT_CC_MAXDEPTH - 1, CCR_COLD);
	put32(out, (CC_STRS_MAGIC | (SIFT_CC_VERSION << 8)));
	put32(out, n);
	put32(out, (uint32) ln.len);
	put32(out, total);
	appendBinaryStringInfo(out, ln.data, ln.len);
	if (wrap_choose(&w, src, total, CCR_COLD, &z))
	{
		put32(out, (uint32) z.alg);
		put32(out, (uint32) z.len);
		appendBinaryStringInfo(out, z.buf, (int) z.len);
		pfree(z.buf);
	}
	else
	{
		put32(out, 0);
		put32(out, total);
		appendBinaryStringInfo(out, (const char *) src, (int) total);
	}
	pad8(out);
	pfree(lens);
	pfree(ln.data);
}

uint32
sift_cc_decode_strs(MemoryContext mcxt, const uint8 *blob, Size len, uint32 **off, char **bytes)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift string list", ALLOCSET_SMALL_SIZES);
	uint32		n,
				ll,
				total,
				form,
				clen;
	CcNode	   *nd;
	uint32	   *lens,
			   *o;
	char	   *b;
	size_t		u,
				pos;
	uint64		sum = 0;

	if (len < 24 || (len & 7) || ((uintptr_t) blob & 7) || get32(blob) != (CC_STRS_MAGIC | (SIFT_CC_VERSION << 8)))
		CC_CORRUPT("bad string list header");
	n = get32(blob + 4);
	ll = get32(blob + 8);
	total = get32(blob + 12);
	if (n > SIFT_CC_MAXROWS || (ll & 7) || (uint64) ll + 16 + 8 > len || total > (uint64) 1 << 31)
		CC_CORRUPT("bad string list sizes");
	nd = parse_node(cx, SIFT_CC_INT, blob + 16, ll, n, 0, &u);
	if (u != ll)
		CC_CORRUPT("string lengths node does not match");
	pos = 16 + (size_t) ll;
	form = get32(blob + pos);
	clen = get32(blob + pos + 4);
	pos += 8;
	if ((uint64) pos + CC_RUP8(clen) != len)
		CC_CORRUPT("string list length does not match");
	lens = palloc(sizeof(uint32) * CC_CAP32(n));
	nd_u32(nd, 0, lens);
	o = MemoryContextAlloc(mcxt, sizeof(uint32) * (n + 1));
	b = MemoryContextAlloc(mcxt, (size_t) total + 1);
	o[0] = 0;
	for (uint32 i = 0; i < n; i++)
	{
		sum += lens[i];
		if (sum > total)
			CC_CORRUPT("string lengths exceed the bytes");
		o[i + 1] = (uint32) sum;
	}
	if (sum != total)
		CC_CORRUPT("string lengths do not add up");
	if (form == 0)
	{
		if (clen != total)
			CC_CORRUPT("raw string bytes length does not match");
		memcpy(b, blob + pos, total);
	}
	else
	{
		if (!sift_colz_available((int) form))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("string list uses compression algorithm %u, which this build does not provide", form)));
		if (!sift_colz_decompress((int) form, blob + pos, clen, b, total))
			CC_CORRUPT("compressed string bytes are damaged");
	}
	b[total] = 0;
	*off = o;
	*bytes = b;
	pfree(lens);
	MemoryContextDelete(cx);
	return n;
}

/*
 * Bit vector: 8-byte header (magic, form, u16 pad, u32 nbits); raw: the bytes padded to 8; positions: u32 count, u32 node length, the node.
 */
void
sift_cc_encode_bits(StringInfo out, const uint64 *words, uint32 nbits, const SiftCcPolicy *pol)
{
	uint64		set = 0;
	uint32		nw = (nbits + 63) / 64;
	size_t		rawsz = CC_RUP8((nbits + 7) / 8);
	int			form = CCB_RAW;
	int64	   *pos = NULL;
	uint32		npos = 0;
	StringInfoData node;
	CcW			w = {pol, SIFT_CC_INT};
	char		h[8] = {0};

	check_policy(pol);
	for (uint32 i = 0; i < nw; i++)
		set += sift_pc64(i == nw - 1 && nbits % 64 ? words[i] & ((UINT64CONST(1) << (nbits % 64)) - 1) : words[i]);
	node.data = NULL;
	if (set == 0)
		form = CCB_ZEROS;
	else if (set == nbits)
		form = CCB_ONES;
	else if (set * 16 <= nbits || (nbits - set) * 16 <= nbits)
	{
		bool		want = set * 2 <= nbits;

		pos = palloc(sizeof(int64) * (want ? set : nbits - set));
		for (uint32 i = 0; i < nbits; i++)
			if ((((words[i / 64] >> (i % 64)) & 1) != 0) == want)
				pos[npos++] = i;
		initStringInfo(&node);
		enc_node(&w, &node, pos, npos, SIFT_CC_MAXDEPTH - 1, CCR_COLD);
		if (8 + node.len < rawsz)
			form = want ? CCB_SET : CCB_CLEAR;
	}
	h[0] = (char) CC_BITS_MAGIC;
	h[1] = (char) form;
	memcpy(h + 4, &nbits, 4);
	appendBinaryStringInfo(out, h, 8);
	if (form == CCB_RAW)
	{
		uint32		nb = (nbits + 7) / 8;
		char	   *raw = palloc0(rawsz + 8);

		for (uint32 i = 0; i < nb; i++)
			raw[i] = (char) (words[i / 8] >> (8 * (i % 8)));
		if (nbits % 8)
			raw[nb - 1] &= (char) ((1 << (nbits % 8)) - 1);
		appendBinaryStringInfo(out, raw, (int) rawsz);
		pfree(raw);
	}
	else if (form == CCB_SET || form == CCB_CLEAR)
	{
		put32(out, npos);
		put32(out, (uint32) node.len);
		appendBinaryStringInfo(out, node.data, node.len);
	}
	if (pos)
		pfree(pos);
	if (node.data)
		pfree(node.data);
}

void
sift_cc_decode_bits(const uint8 *blob, Size len, uint32 nbits, uint64 *words)
{
	uint32		nw = (nbits + 63) / 64;
	int			form;

	if (len < 8 || (len & 7) || ((uintptr_t) blob & 7) || blob[0] != CC_BITS_MAGIC || get32(blob + 4) != nbits)
		CC_CORRUPT("bad bit vector header");
	form = blob[1];
	memset(words, 0, sizeof(uint64) * nw);
	switch (form)
	{
		case CCB_ZEROS:
			if (len != 8)
				CC_CORRUPT("bit vector length does not match");
			break;
		case CCB_ONES:
			if (len != 8)
				CC_CORRUPT("bit vector length does not match");
			set_ones(words, nbits);
			break;
		case CCB_RAW:
			if (len != 8 + CC_RUP8((nbits + 7) / 8))
				CC_CORRUPT("bit vector length does not match");
			memcpy(words, blob + 8, (nbits + 7) / 8);
			clear_tail(words, nbits);
			break;
		case CCB_SET:
		case CCB_CLEAR:
			{
				MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift bit vector", ALLOCSET_SMALL_SIZES);
				uint32		npos,
							nl;
				CcNode	   *nd;
				int64	   *pos;
				size_t		u;

				need(len, 16);
				npos = get32(blob + 8);
				nl = get32(blob + 12);
				if (npos > nbits || (nl & 7) || (uint64) nl + 16 != len)
					CC_CORRUPT("bit vector positions do not match");
				nd = parse_node(cx, SIFT_CC_INT, blob + 16, nl, npos, 0, &u);
				if (u != nl)
					CC_CORRUPT("bit vector node does not match");
				pos = palloc(sizeof(int64) * ((size_t) npos + SIFT_COL_PAD / 8));
				nd_i64(nd, pos);
				if (form == CCB_CLEAR)
					set_ones(words, nbits);
				for (uint32 i = 0; i < npos; i++)
				{
					if (pos[i] < 0 || pos[i] >= (int64) nbits || (i && pos[i] <= pos[i - 1]))
						CC_CORRUPT("bit vector position out of order");
					if (form == CCB_SET)
						words[pos[i] / 64] |= UINT64CONST(1) << (pos[i] % 64);
					else
						words[pos[i] / 64] &= ~(UINT64CONST(1) << (pos[i] % 64));
				}
				pfree(pos);
				MemoryContextDelete(cx);
				break;
			}
		default:
			CC_CORRUPT("unknown bit vector form");
	}
}
