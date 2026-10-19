/*
 * research.c - measures column encodings and general-purpose compressors on order-shaped data (docs/perf/COMPRESSION.md).
 *
 *   research [-n orders] [-c chunk_rows] [-k] [-s]     -k clustered by (store_id, order_date), -s decode speeds
 *
 * For each column, per chunk of -c rows: the size under every candidate encoding (real encoders, vertical 32-lane bit-packing,
 * cascades up to depth 2), each layered with lz4 / lz4hc / zstd, plus the per-chunk "pick the smallest" outcome. Throwaway
 * experiments (Gorilla, byte-stream-split, front coding) live here only; what the extension ships is src/col/sift_colcodec.c.
 */
#define _GNU_SOURCE
#include <lz4.h>
#include <lz4hc.h>
#include <time.h>
#include <zstd.h>

#include "gen.h"

static u64
now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return (u64) t.tv_sec * 1000000000ull + (u64) t.tv_nsec;
}

/* run body until 40 ms of CPU time elapsed, best of 3; result in ns per value */
#define TIMEIT(res, nvals, body) do { \
	double best_ = 1e30; \
	for (int rr_ = 0; rr_ < 3; rr_++) { \
		u64 t0_ = now_ns(); long reps_ = 0; \
		do { body; reps_++; } while (now_ns() - t0_ < 40000000ull); \
		double x_ = (double) (now_ns() - t0_) / ((double) reps_ * (double) (nvals)); \
		if (x_ < best_) best_ = x_; \
	} \
	res = best_; \
} while (0)

#define PAD(n) ((((size_t) (n)) + 1023) & ~(size_t) 1023)

typedef struct Buf
{
	u8		   *p;
	size_t		n,
				cap;
} Buf;

static void
bput(Buf *b, const void *s, size_t n)
{
	if (b->n + n > b->cap)
	{
		b->cap = (b->n + n) * 2 + 64;
		b->p = realloc(b->p, b->cap);
	}
	memcpy(b->p + b->n, s, n);
	b->n += n;
}
static void bu8(Buf *b, u8 x) { bput(b, &x, 1); }
static void bu32(Buf *b, u32 x) { bput(b, &x, 4); }
static void bu64(Buf *b, u64 x) { bput(b, &x, 8); }
static void bfree(Buf *b) { free(b->p); memset(b, 0, sizeof *b); }

static inline u32
bits_of(u64 x)
{
	return x ? 64 - __builtin_clzll(x) : 0;
}

/* ---- vertical 32-lane bit-packing: value i of a 1024-block is row i/32, lane i%32; lane l is a bit stream of its rows ---- */
static size_t
bp_words(u32 n, u32 W)
{
	size_t		t = 0;
	u32			rows = (n + 31) >> 5;

	for (; rows >= 32; rows -= 32)
		t += (size_t) W * 32;
	if (rows)
		t += (size_t) ((rows * W + 31) >> 5) * 32;
	return t;
}

static void
bp_pack(const u32 *v, u32 n, u32 W, u32 *out)
{
	u32			rows = (n + 31) >> 5;

	memset(out, 0, bp_words(n, W) * 4);
	for (u32 b = 0; b * 32 < rows; b++)
	{
		u32			R = rows - b * 32 > 32 ? 32 : rows - b * 32;
		const u32  *vb = v + (size_t) b * 1024;

		for (u32 r = 0; r < R; r++)
		{
			u32			bp = r * W,
						w0 = bp >> 5,
						s = bp & 31;

			for (u32 l = 0; l < 32; l++)
			{
				u32			x = vb[r * 32 + l];

				out[w0 * 32 + l] |= x << s;
				if (s + W > 32)
					out[(w0 + 1) * 32 + l] |= x >> (32 - s);
			}
		}
		out += R == 32 ? (size_t) W * 32 : (size_t) ((R * W + 31) >> 5) * 32;
	}
}

static inline __attribute__((always_inline)) void
unpack_rows(const u32 *restrict in, u32 *restrict out, u32 W, u32 R)
{
	u32			mask = W >= 32 ? ~0u : (1u << W) - 1;

	for (u32 r = 0; r < R; r++)
	{
		u32			bp = r * W,
					s = bp & 31;
		const u32  *a = in + (size_t) (bp >> 5) * 32;

		if (s + W > 32)
			for (u32 l = 0; l < 32; l++)
				out[r * 32 + l] = ((a[l] >> s) | (a[l + 32] << (32 - s))) & mask;
		else
			for (u32 l = 0; l < 32; l++)
				out[r * 32 + l] = (a[l] >> s) & mask;
	}
}

#define UNPACK_CASES(X) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) \
	X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) X(32)

static void
bp_unpack(const u32 *in, u32 n, u32 W, u32 *out)
{
	u32			rows = (n + 31) >> 5;

	if (W == 0)
	{
		memset(out, 0, (size_t) rows * 128);
		return;
	}
	for (; rows >= 32; rows -= 32, in += (size_t) W * 32, out += 1024)
		switch (W)
		{
#define C(k) case k: unpack_rows(in, out, k, 32); break;
				UNPACK_CASES(C)
#undef C
		}
	if (rows)
		unpack_rows(in, out, W, rows);
}

/* ---- scratch arena for decoders ---- */
static u8	scratch[1 << 26];
static size_t scr_top;
static void *
scr_alloc(size_t n)
{
	void	   *p = scratch + scr_top;

	scr_top += (n + 63) & ~(size_t) 63;
	return p;
}

/* ---- integer nodes: PLAIN CONST FOR DELTA RLE DICT, children are nodes ---- */
enum { E_PLAIN, E_CONST, E_FOR, E_DELTA, E_RLE, E_DICT, E_N };
static const char *const enc_name[] = {"plain", "const", "for", "delta", "rle", "dict"};

static u32
al_w(u32 W, int al)
{
	if (!al || !W)
		return W;
	W = (W + 7) & ~7u;
	return W > 32 ? 32 : W;
}

static Buf enc_int(const u64 *v, u32 n, int width, int depth, int force, int al);

static Buf
enc_plain(const u64 *v, u32 n, int width)
{
	Buf			b = {0};

	bu8(&b, E_PLAIN);
	bu8(&b, (u8) width);
	for (u32 i = 0; i < n; i++)
		bput(&b, &v[i], width);
	return b;
}

static Buf
enc_for(const u64 *v, u32 n, int al)
{
	Buf			b = {0};
	i64			mn = (i64) v[0],
				mx = mn;
	u32		   *tmp,
			   *out;
	u32			W;
	u64			range;

	for (u32 i = 1; i < n; i++)
	{
		mn = (i64) v[i] < mn ? (i64) v[i] : mn;
		mx = (i64) v[i] > mx ? (i64) v[i] : mx;
	}
	range = (u64) mx - (u64) mn;
	if (range > 0xffffffffu)
		return b;
	W = al_w(bits_of(range), al);
	tmp = calloc(PAD(n), 4);
	for (u32 i = 0; i < n; i++)
		tmp[i] = (u32) (v[i] - (u64) mn);
	out = malloc(bp_words(n, W) * 4 + 4);
	bp_pack(tmp, n, W, out);
	bu8(&b, E_FOR);
	bu64(&b, (u64) mn);
	bu8(&b, (u8) W);
	bput(&b, out, bp_words(n, W) * 4);
	free(tmp);
	free(out);
	return b;
}

static Buf
enc_delta(const u64 *v, u32 n, int al)
{
	Buf			b = {0};
	i64			md = 0,
				mxd = 0;
	u32		   *tmp,
			   *out;
	u32			W;

	if (n < 2)
		return b;
	md = mxd = (i64) (v[1] - v[0]);
	for (u32 i = 2; i < n; i++)
	{
		i64			d = (i64) (v[i] - v[i - 1]);

		md = d < md ? d : md;
		mxd = d > mxd ? d : mxd;
	}
	if ((u64) mxd - (u64) md > 0xffffffffu)
		return b;
	W = al_w(bits_of((u64) mxd - (u64) md), al);
	tmp = calloc(PAD(n), 4);
	for (u32 i = 1; i < n; i++)
		tmp[i] = (u32) ((v[i] - v[i - 1]) - (u64) md);
	out = malloc(bp_words(n, W) * 4 + 4);
	bp_pack(tmp, n, W, out);
	bu8(&b, E_DELTA);
	bu64(&b, v[0]);
	bu64(&b, (u64) md);
	bu8(&b, (u8) W);
	for (u32 i = 1024; i < n; i += 1024)	/* checkpoints: value at every block start, for random access */
		bu64(&b, v[i]);
	bput(&b, out, bp_words(n, W) * 4);
	free(tmp);
	free(out);
	return b;
}

static Buf
enc_rle(const u64 *v, u32 n, int width, int depth, int al)
{
	Buf			b = {0},
				cv,
				cl;
	u64		   *rv = malloc(sizeof(u64) * n),
			   *rl = malloc(sizeof(u64) * n);
	u32			nr = 0;

	if (depth <= 0)
		goto out;
	for (u32 i = 0; i < n; i++)
	{
		if (i && v[i] == v[i - 1])
			rl[nr - 1]++;
		else
		{
			rv[nr] = v[i];
			rl[nr++] = 1;
		}
	}
	if (nr * 2 > n)
		goto out;
	cv = enc_int(rv, nr, width, depth - 1, -1, al);
	cl = enc_int(rl, nr, 4, depth - 1, -1, al);
	bu8(&b, E_RLE);
	bu32(&b, nr);
	bu32(&b, (u32) cv.n);
	bput(&b, cv.p, cv.n);
	bput(&b, cl.p, cl.n);
	/* random access needs run starts: a sampled cumulative index, one u32 per 64 runs */
	for (u32 i = 0; i < nr / 64; i++)
		bu32(&b, 0);
	bfree(&cv);
	bfree(&cl);
out:
	free(rv);
	free(rl);
	return b;
}

static int
cmp_i64(const void *a, const void *b)
{
	i64			x = *(const i64 *) a,
				y = *(const i64 *) b;

	return x < y ? -1 : x > y;
}

static Buf
enc_dict(const u64 *v, u32 n, int width, int depth, int al)
{
	Buf			b = {0},
				cd;
	u64		   *s = NULL;
	u32			d = 0,
			   *codes = NULL,
			   *out;
	u32			W;

	if (depth <= 0)
		return b;
	s = malloc(sizeof(u64) * n);
	memcpy(s, v, sizeof(u64) * n);
	qsort(s, n, 8, cmp_i64);
	for (u32 i = 0; i < n; i++)
		if (!i || s[i] != s[d - 1])
			s[d++] = s[i];
	if (d >= n || d > 65536)
		goto out;
	codes = calloc(PAD(n), 4);
	for (u32 i = 0; i < n; i++)
	{
		u32			lo = 0,
					hi = d;

		while (lo < hi)
		{
			u32			m = (lo + hi) / 2;

			if ((i64) s[m] < (i64) v[i])
				lo = m + 1;
			else
				hi = m;
		}
		codes[i] = lo;
	}
	W = al_w(bits_of(d - 1), al);
	cd = enc_int(s, d, width, depth - 1, -1, al);
	out = malloc(bp_words(n, W) * 4 + 4);
	bp_pack(codes, n, W, out);
	bu8(&b, E_DICT);
	bu32(&b, d);
	bu32(&b, (u32) cd.n);
	bput(&b, cd.p, cd.n);
	bu8(&b, (u8) W);
	bput(&b, out, bp_words(n, W) * 4);
	bfree(&cd);
	free(out);
out:
	free(s);
	free(codes);
	return b;
}

/* force == -1: the smallest applicable; otherwise that encoding or an empty Buf when it does not apply */
static Buf
enc_int(const u64 *v, u32 n, int width, int depth, int force, int al)
{
	Buf			c[E_N] = {{0}},
				best = {0};
	int			bi = -1;
	int			same = 1;

	for (u32 i = 1; i < n; i++)
		same &= v[i] == v[0];
	if (same && n && (force < 0 || force == E_CONST))
	{
		bu8(&best, E_CONST);
		bu64(&best, v[0]);
		return best;
	}
	if (!n)
		return enc_plain(v, 0, width);
	for (int e = 0; e < E_N; e++)
	{
		if (force >= 0 && force != e)
			continue;
		switch (e)
		{
			case E_PLAIN: c[e] = enc_plain(v, n, width); break;
			case E_FOR: c[e] = enc_for(v, n, al); break;
			case E_DELTA: c[e] = enc_delta(v, n, al); break;
			case E_RLE: c[e] = enc_rle(v, n, width, depth, al); break;
			case E_DICT: c[e] = enc_dict(v, n, width, depth, al); break;
		}
		if (c[e].p && (bi < 0 || c[e].n < c[bi].n))
			bi = e;
	}
	if (bi < 0)
		return best;
	best = c[bi];
	for (int e = 0; e < E_N; e++)
		if (e != bi)
			bfree(&c[e]);
	return best;
}

static int
node_enc(const Buf *b)
{
	return b->p[0];
}

#define DEFDEC(T, S) \
static void dec_##S(const u8 *p, size_t len, u32 n, T *out) \
{ \
	switch (*p++) \
	{ \
		case E_PLAIN: { int w = *p++; for (u32 i = 0; i < n; i++) { u64 x = 0; memcpy(&x, p + (size_t) i * w, w); out[i] = (T) x; } break; } \
		case E_CONST: { u64 x; memcpy(&x, p, 8); for (u32 i = 0; i < n; i++) out[i] = (T) x; break; } \
		case E_FOR: { u64 base; memcpy(&base, p, 8); u32 W = p[8]; size_t m = scr_top; u32 *t = scr_alloc(PAD(n) * 4); \
			bp_unpack((const u32 *) (p + 9), n, W, t); for (u32 i = 0; i < n; i++) out[i] = (T) (base + t[i]); scr_top = m; break; } \
		case E_DELTA: { u64 first, md; memcpy(&first, p, 8); memcpy(&md, p + 8, 8); u32 W = p[16]; size_t m = scr_top; \
			u32 *t = scr_alloc(PAD(n) * 4); const u8 *w = p + 17 + (n > 1024 ? 8 * ((n - 1) / 1024) : 0); \
			bp_unpack((const u32 *) w, n, W, t); T run = (T) first; out[0] = run; \
			for (u32 i = 1; i < n; i++) { run += (T) ((u64) t[i] + md); out[i] = run; } scr_top = m; break; } \
		case E_RLE: { u32 nr, vl; memcpy(&nr, p, 4); memcpy(&vl, p + 4, 4); size_t m = scr_top; \
			T *rv = scr_alloc(sizeof(T) * nr); u32 *rl = scr_alloc(4 * (size_t) nr); \
			size_t lenl = len - 9 - vl - 4 * (nr / 64); \
			dec_##S(p + 8, vl, nr, rv); dec_u32(p + 8 + vl, lenl, nr, rl); \
			u32 k = 0; for (u32 r = 0; r < nr; r++) { T x = rv[r]; for (u32 j = 0; j < rl[r]; j++) out[k++] = x; } scr_top = m; break; } \
		case E_DICT: { u32 d, dl; memcpy(&d, p, 4); memcpy(&dl, p + 4, 4); size_t m = scr_top; \
			T *dict = scr_alloc(sizeof(T) * d); u32 *t = scr_alloc(PAD(n) * 4); \
			dec_##S(p + 8, dl, d, dict); u32 W = p[8 + dl]; bp_unpack((const u32 *) (p + 9 + dl), n, W, t); \
			for (u32 i = 0; i < n; i++) { out[i] = dict[t[i]]; } scr_top = m; break; } \
	} \
}
static void dec_u32(const u8 *p, size_t len, u32 n, u32 *out);
DEFDEC(u32, u32)
DEFDEC(u64, u64)

/* ---- general-purpose compressors ---- */
typedef struct Alg
{
	const char *name;
	int			id,
				level;
} Alg;
static const Alg algs[] = {{"lz4", 0, 1}, {"lz4hc9", 1, 9}, {"zstd1", 2, 1}, {"zstd3", 2, 3}, {"zstd9", 2, 9}, {"zstd19", 2, 19}};
#define NALG 5
#define ALG_ZSTD3 3
#define Z_OVERHEAD 8			/* algorithm id and the two lengths of the wrapper node */

static size_t
z_comp(const Alg *a, const void *src, size_t n, void *dst, size_t cap)
{
	switch (a->id)
	{
		case 0: return (size_t) LZ4_compress_default(src, dst, (int) n, (int) cap);
		case 1: return (size_t) LZ4_compress_HC(src, dst, (int) n, (int) cap, a->level);
		default: return ZSTD_compress(dst, cap, src, n, a->level);
	}
}

static size_t
z_decomp(const Alg *a, const void *src, size_t clen, void *dst, size_t n)
{
	if (a->id < 2)
		return (size_t) LZ4_decompress_safe(src, dst, (int) clen, (int) n);
	return ZSTD_decompress(dst, n, src, clen);
}

static size_t
z_size(const Alg *a, const void *src, size_t n)
{
	size_t		cap = n + n / 8 + 1024,
				r;
	void	   *dst = malloc(cap);

	r = n ? z_comp(a, src, n, dst, cap) : 0;
	free(dst);
	return r + Z_OVERHEAD;
}

/* ---- floats ---- */
static const double P10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};
static const float P10f[] = {1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f};

/* decimal view: d = round(v * 10^e) and (T) d / 10^e reproduces v bit for bit; returns 0 for an exception */
static inline int
alp_one(const void *vals, u32 i, int w, int e, i64 *d)
{
	if (w == 8)
	{
		double		x = ((const double *) vals)[i],
					t;
		u64			a,
					b;
		double		back;

		if (!isfinite(x))
			return 0;
		t = x * P10[e];
		if (fabs(t) >= 9007199254740992.0)
			return 0;
		*d = (i64) nearbyint(t);
		back = (double) *d / P10[e];
		memcpy(&a, &x, 8);
		memcpy(&b, &back, 8);
		return a == b;
	}
	else
	{
		float		x = ((const float *) vals)[i],
					t,
					back;
		u32			a,
					b;

		if (!isfinite(x))
			return 0;
		t = x * P10f[e];
		if (fabsf(t) >= 16777216.0f)
			return 0;
		*d = (i64) nearbyintf(t);
		back = (float) *d / P10f[e];
		memcpy(&a, &x, 4);
		memcpy(&b, &back, 4);
		return a == b;
	}
}

static int
alp_pick_e(const void *vals, u32 n, int w)
{
	int			emax = w == 8 ? 18 : 10,
				best = 0;
	u32			bestok = 0;

	for (int e = 0; e <= emax; e++)
	{
		u32			ok = 0,
					step = n > 128 ? n / 128 : 1;
		i64			d;

		for (u32 i = 0; i < n; i += step)
			ok += alp_one(vals, i, w, e, &d);
		if (ok > bestok)
		{
			bestok = ok;
			best = e;
		}
	}
	return best;
}

static Buf
enc_alp(const void *vals, u32 n, int w, int al, u32 *nexc_out)
{
	Buf			b = {0},
				cd,
				cp,
				raw = {0};
	int			e = alp_pick_e(vals, n, w);
	i64		   *d = malloc(sizeof(i64) * n);
	u64		   *pos = malloc(sizeof(u64) * n);
	u32			nexc = 0;
	i64			prev = 0;

	for (u32 i = 0; i < n; i++)
	{
		i64			x;

		if (alp_one(vals, i, w, e, &x))
			prev = d[i] = x;
		else
		{
			d[i] = prev;
			pos[nexc++] = i;
			bput(&raw, (const u8 *) vals + (size_t) i * w, w);
		}
	}
	cd = enc_int((u64 *) d, n, 8, 2, -1, al);
	cp = enc_int(pos, nexc, 4, 2, -1, al);
	bu8(&b, 6);
	bu8(&b, (u8) e);
	bu32(&b, nexc);
	bu32(&b, (u32) cd.n);
	bput(&b, cd.p, cd.n);
	bu32(&b, (u32) cp.n);
	bput(&b, cp.p, nexc ? cp.n : 0);
	bput(&b, raw.p, raw.n);
	*nexc_out = nexc;
	bfree(&cd);
	bfree(&cp);
	bfree(&raw);
	free(d);
	free(pos);
	return b;
}

static void
dec_alp(const u8 *p, u32 n, int w, void *out)
{
	int			e = p[1];
	u32			nexc,
				dl,
				pl;
	size_t		m = scr_top;
	u64		   *d = scr_alloc(8 * (size_t) n),
			   *pos;
	const u8   *raw;

	memcpy(&nexc, p + 2, 4);
	memcpy(&dl, p + 6, 4);
	dec_u64(p + 10, dl, n, d);
	memcpy(&pl, p + 10 + dl, 4);
	pos = scr_alloc(8 * (size_t) nexc + 8);
	if (nexc)
		dec_u64(p + 14 + dl, pl, nexc, pos);
	raw = p + 14 + dl + (nexc ? pl : 0);
	if (w == 8)
		for (u32 i = 0; i < n; i++)
			((double *) out)[i] = (double) (i64) d[i] / P10[e];
	else
		for (u32 i = 0; i < n; i++)
			((float *) out)[i] = (float) (i32) d[i] / P10f[e];
	for (u32 k = 0; k < nexc; k++)
		memcpy((u8 *) out + pos[k] * w, raw + (size_t) k * w, w);
	scr_top = m;
}

static Buf
enc_bss(const void *vals, u32 n, int w)
{
	Buf			b = {0};

	b.p = malloc((size_t) n * w + 1);
	b.n = b.cap = (size_t) n * w;
	for (u32 i = 0; i < n; i++)
		for (int k = 0; k < w; k++)
			b.p[(size_t) k * n + i] = ((const u8 *) vals)[(size_t) i * w + k];
	return b;
}

/* Gorilla: XOR with the previous value, control bits 0 (same) / 10 (reuse window) / 11 (new window) */
typedef struct BW
{
	u8		   *p;
	u64			pos;
} BW;

static void
bw_put(BW *w, u64 v, int nb)
{
	for (int i = 0; i < nb; i++, w->pos++)
	{
		if ((v >> i) & 1)
			w->p[w->pos >> 3] |= 1 << (w->pos & 7);
	}
}

static u64
bw_get(const u8 *p, u64 *pos, int nb)
{
	u64			v = 0;

	for (int i = 0; i < nb; i++, (*pos)++)
		v |= (u64) ((p[*pos >> 3] >> (*pos & 7)) & 1) << i;
	return v;
}

static Buf
enc_gorilla(const void *vals, u32 n, int w)
{
	Buf			b = {0};
	BW			bw;
	u64			prev = 0;
	int			plz = 99,
				ptz = 0,
				B = w * 8;

	b.p = calloc((size_t) n * (w + 2) + 16, 1);
	bw.p = b.p;
	bw.pos = 0;
	for (u32 i = 0; i < n; i++)
	{
		u64			x = 0;

		memcpy(&x, (const u8 *) vals + (size_t) i * w, w);
		if (i == 0)
			bw_put(&bw, x, B);
		else if (x == prev)
			bw_put(&bw, 0, 1);
		else
		{
			u64			z = x ^ prev;
			int			lz = __builtin_clzll(z) - (64 - B),
						tz = __builtin_ctzll(z);

			lz = lz > 31 ? 31 : lz;
			if (plz != 99 && lz >= plz && tz >= ptz)
			{
				bw_put(&bw, 1, 1);
				bw_put(&bw, 0, 1);
				bw_put(&bw, z >> ptz, B - plz - ptz);
			}
			else
			{
				int			len = B - lz - tz;

				bw_put(&bw, 1, 1);
				bw_put(&bw, 1, 1);
				bw_put(&bw, lz, 5);
				bw_put(&bw, len == 64 ? 0 : len, 6);
				bw_put(&bw, z >> tz, len);
				plz = lz;
				ptz = tz;
			}
		}
		prev = x;
	}
	b.n = (bw.pos + 7) / 8;
	b.cap = (size_t) n * (w + 2) + 16;
	return b;
}

static void
dec_gorilla(const u8 *p, u32 n, int w, void *out)
{
	u64			pos = 0,
				prev = 0;
	int			plz = 0,
				ptz = 0,
				B = w * 8;

	for (u32 i = 0; i < n; i++)
	{
		u64			x;

		if (i == 0)
			x = bw_get(p, &pos, B);
		else if (!bw_get(p, &pos, 1))
			x = prev;
		else
		{
			if (bw_get(p, &pos, 1))
			{
				plz = (int) bw_get(p, &pos, 5);
				int			len = (int) bw_get(p, &pos, 6);

				len = len ? len : 64;
				ptz = B - plz - len;
			}
			x = prev ^ (bw_get(p, &pos, B - plz - ptz) << ptz);
		}
		memcpy((u8 *) out + (size_t) i * w, &x, w);
		prev = x;
	}
}

/* ---- analysis driver ---- */
#define MAXS 24
typedef struct Row
{
	const char *lab[MAXS];
	double		sz[MAXS];
	int			ns;
} Row;

static void
row_add(Row *r, const char *lab, double sz)
{
	r->lab[r->ns] = lab;
	r->sz[r->ns++] = sz;
}

static u32 g_chunk = 16384;
static int g_speed;

/* values of one chunk widened to u64 (sign extended when signed); nulls take the previous value so runs and deltas survive */
static u64 *
widen(const Col *c, u32 i0, u32 n)
{
	u64		   *v = malloc(sizeof(u64) * (n + 1));
	u64			prev = 0;

	for (u32 i = 0; i < n; i++)
	{
		u64			x = 0;

		switch (c->width)
		{
			case 1: x = ((u8 *) c->d)[i0 + i]; break;
			case 2: x = ((u16 *) c->d)[i0 + i]; break;
			case 4: x = c->sgn ? (u64) (i64) ((i32 *) c->d)[i0 + i] : ((u32 *) c->d)[i0 + i]; break;
			default: x = ((u64 *) c->d)[i0 + i];
		}
		if (c->nul && c->nul[i0 + i])
			x = prev;
		v[i] = prev = x;
	}
	return v;
}

typedef struct Acc
{
	double		sz[MAXS];
	const char *lab[MAXS];
	int			ns;
	double		autosim;
	int			encs[E_N + 3];
	double		raw;
} Acc;

static void
acc_row(Acc *a, const Row *r)
{
	a->ns = r->ns;
	for (int i = 0; i < r->ns; i++)
	{
		a->sz[i] += r->sz[i];
		a->lab[i] = r->lab[i];
	}
}

static double
layered_min(const u8 *p, size_t n, double *out_by_alg)
{
	double		m = (double) n;

	for (int a = 0; a < NALG; a++)
	{
		double		z = (double) z_size(&algs[a], p, n);

		out_by_alg[a] = z;
		m = z < m ? z : m;
	}
	return m;
}

static void
analyze_int(const Col *c, Acc *acc)
{
	for (u32 i0 = 0; i0 < c->n; i0 += g_chunk)
	{
		u32			n = c->n - i0 < g_chunk ? c->n - i0 : g_chunk;
		u64		   *v = widen(c, i0, n);
		Row			r = {0};
		Buf			pl = enc_plain(v, n, c->width),
					fr = enc_int(v, n, c->width, 2, E_FOR, 0),
					de = enc_int(v, n, c->width, 2, E_DELTA, 0),
					rl = enc_int(v, n, c->width, 2, E_RLE, 0),
					di = enc_int(v, n, c->width, 2, E_DICT, 0),
					be = enc_int(v, n, c->width, 2, -1, 0),
					ba = enc_int(v, n, c->width, 2, -1, 1);
		double		zp[NALG],
					zb[NALG],
					za[NALG],
					best;

		row_add(&r, "plain", (double) n * c->width);
		row_add(&r, "plain+lz4", z_size(&algs[0], pl.p + 2, pl.n - 2));
		row_add(&r, "plain+zstd3", z_size(&algs[ALG_ZSTD3], pl.p + 2, pl.n - 2));
		row_add(&r, "for", fr.p ? (double) fr.n : (double) pl.n);
		row_add(&r, "delta", de.p ? (double) de.n : (double) pl.n);
		row_add(&r, "rle", rl.p ? (double) rl.n : (double) pl.n);
		row_add(&r, "dict", di.p ? (double) di.n : (double) pl.n);
		{
			/* PFOR estimate: width W' for most values, the others patched (u16 position + the bits above W') */
			i64			mn = (i64) v[0];
			u32			W,
						bw;
			double		pf;

			for (u32 i = 1; i < n; i++)
				mn = (i64) v[i] < mn ? (i64) v[i] : mn;
			for (u32 i = 0, mx = 0; i < n; i++)
				mx = (u32) (v[i] - (u64) mn) > mx ? (u32) (v[i] - (u64) mn) : mx, W = bits_of(mx);
			pf = fr.p ? (double) fr.n : (double) pl.n;
			for (bw = 0; bw < W; bw++)
			{
				u32			exc = 0;
				double		cost;

				for (u32 i = 0; i < n; i++)
					exc += bits_of(v[i] - (u64) mn) > bw;
				cost = 17 + (double) bp_words(n, bw) * 4 + exc * (2.0 + (W - bw + 7) / 8) + 4;
				pf = cost < pf ? cost : pf;
			}
			row_add(&r, "pfor(est)", pf);
		}
		row_add(&r, "best", (double) be.n);
		best = layered_min(be.p, be.n, zb);
		for (int a = 0; a < NALG; a++)
			row_add(&r, algs[a].name, zb[a]);
		layered_min(ba.p, ba.n, za);
		row_add(&r, "aligned+zstd3", za[ALG_ZSTD3]);
		layered_min(pl.p + 2, pl.n - 2, zp);
		for (int a = 0; a < NALG; a++)
			best = zp[a] < best ? zp[a] : best;
		best = za[ALG_ZSTD3] < best ? za[ALG_ZSTD3] : best;
		best = (double) be.n < best ? (double) be.n : best;
		row_add(&r, "autosim", best);
		acc_row(acc, &r);
		acc->raw += (double) n * c->width;
		acc->encs[node_enc(&be)]++;
		bfree(&pl); bfree(&fr); bfree(&de); bfree(&rl); bfree(&di); bfree(&be); bfree(&ba);
		free(v);
	}
}

static void
analyze_float(const Col *c, Acc *acc)
{
	int			w = c->width;

	for (u32 i0 = 0; i0 < c->n; i0 += g_chunk)
	{
		u32			n = c->n - i0 < g_chunk ? c->n - i0 : g_chunk,
					nexc;
		const u8   *src = (const u8 *) c->d + (size_t) i0 * w;
		Row			r = {0};
		Buf			bs = enc_bss(src, n, w),
					go = enc_gorilla(src, n, w),
					al = enc_alp(src, n, w, 0, &nexc);
		double		za[NALG],
					best = (double) al.n;

		row_add(&r, "plain", (double) n * w);
		row_add(&r, "plain+lz4", z_size(&algs[0], src, (size_t) n * w));
		row_add(&r, "plain+zstd3", z_size(&algs[ALG_ZSTD3], src, (size_t) n * w));
		row_add(&r, "bss+zstd3", z_size(&algs[ALG_ZSTD3], bs.p, bs.n));
		row_add(&r, "gorilla", (double) go.n);
		row_add(&r, "gorilla+zstd3", z_size(&algs[ALG_ZSTD3], go.p, go.n));
		row_add(&r, "alp", (double) al.n);
		layered_min(al.p, al.n, za);
		for (int a = 0; a < NALG; a++)
			row_add(&r, algs[a].name, za[a]);
		for (int i = 3; i < r.ns; i++)
			best = r.sz[i] < best ? r.sz[i] : best;
		best = r.sz[0] < best ? r.sz[0] : best;
		row_add(&r, "autosim", best);
		acc_row(acc, &r);
		acc->raw += (double) n * w;
		acc->encs[E_N] += nexc < n / 10;
		bfree(&bs); bfree(&go); bfree(&al);
	}
}

/* strings: sorted dictionary of the chunk, plain, front-coded */
static const char *g_sb;
static const u32 *g_so;
static u32 g_sbase;
static int
cmp_str(const void *a, const void *b)
{
	u32			x = *(const u32 *) a,
				y = *(const u32 *) b,
				lx = g_so[x + 1] - g_so[x],
				ly = g_so[y + 1] - g_so[y];
	int			c = memcmp(g_sb + g_so[x], g_sb + g_so[y], lx < ly ? lx : ly);

	return c ? c : (lx > ly) - (lx < ly);
}

static void
analyze_str(const Col *c, Acc *acc)
{
	for (u32 i0 = 0; i0 < c->n; i0 += g_chunk)
	{
		u32			n = c->n - i0 < g_chunk ? c->n - i0 : g_chunk;
		u32		   *idx = malloc(sizeof(u32) * n),
				   *codes = malloc(sizeof(u32) * n);
		u64		   *lens = malloc(sizeof(u64) * n),
				   *dl = malloc(sizeof(u64) * n),
				   *cc = malloc(sizeof(u64) * n);
		const char *blob = c->d;
		Row			r = {0};
		Buf			pb = {0},
					db = {0},
					fb = {0};
		Buf			lb,
					dlb,
					cb,
					cb8;
		u32			d = 0;
		size_t		fc_extra;
		u32			ploff = c->off[i0];
		double		plain_raw,
					best;

		g_sb = blob;
		g_so = c->off;
		for (u32 i = 0; i < n; i++)
		{
			idx[i] = i0 + i;
			lens[i] = c->off[i0 + i + 1] - c->off[i0 + i];
		}
		bput(&pb, blob + ploff, c->off[i0 + n] - ploff);
		plain_raw = (double) pb.n + 4.0 * n;
		qsort(idx, n, 4, cmp_str);
		for (u32 k = 0; k < n; k++)
		{
			u32			j = idx[k];

			if (k && !cmp_str(&idx[k - 1], &j))
				codes[j - i0] = d - 1;
			else
			{
				codes[j - i0] = d;
				dl[d++] = c->off[j + 1] - c->off[j];
				bput(&db, blob + c->off[j], c->off[j + 1] - c->off[j]);
			}
		}
		/* front coding, buckets of 16: first string whole, the others as (shared prefix, suffix) */
		fc_extra = 0;
		{
			u32			di = 0,
						k = 0;
			const char *prev = NULL;
			u32			plen = 0;

			for (; k < n; k++)
			{
				u32			j = idx[k],
							l = c->off[j + 1] - c->off[j];
				const char *s = blob + c->off[j];

				if (k && !cmp_str(&idx[k - 1], &j))
					continue;
				if (di % 16 == 0)
					fc_extra += l;
				else
				{
					u32			pre = 0;

					while (pre < l && pre < plen && s[pre] == prev[pre])
						pre++;
					fc_extra += 2 + (l - pre);
				}
				di++;
				prev = s;
				plen = l;
			}
		}
		for (u32 i = 0; i < n; i++)
			cc[i] = codes[i];
		lb = enc_int(lens, n, 4, 2, -1, 0);
		dlb = enc_int(dl, d, 4, 2, -1, 0);
		cb = enc_int(cc, n, 4, 2, -1, 0);
		cb8 = enc_int(cc, n, 4, 2, -1, 1);
		fb.n = 0;
		row_add(&r, "plain", plain_raw);
		row_add(&r, "plain+lz4", z_size(&algs[0], pb.p, pb.n) + 4.0 * n);
		row_add(&r, "plain+zstd3", z_size(&algs[ALG_ZSTD3], pb.p, pb.n) + 4.0 * n);
		row_add(&r, "plain(lens enc)", (double) lb.n + pb.n);
		row_add(&r, "plain+zstd3(lens enc)", (double) lb.n + z_size(&algs[ALG_ZSTD3], pb.p, pb.n));
		row_add(&r, "dict", (double) dlb.n + db.n + cb.n);
		row_add(&r, "dict+front", (double) dlb.n + fc_extra + cb.n);
		row_add(&r, "dict+zstd3 blob", (double) dlb.n + z_size(&algs[ALG_ZSTD3], db.p, db.n) + cb.n);
		row_add(&r, "dict+lz4 blob", (double) dlb.n + z_size(&algs[0], db.p, db.n) + cb.n);
		row_add(&r, "dict, codes+zstd3", (double) dlb.n + z_size(&algs[ALG_ZSTD3], db.p, db.n) + z_size(&algs[ALG_ZSTD3], cb.p, cb.n));
		row_add(&r, "dict, u8 codes+zstd3", (double) dlb.n + z_size(&algs[ALG_ZSTD3], db.p, db.n) + z_size(&algs[ALG_ZSTD3], cb8.p, cb8.n));
		best = 1e30;
		for (int i = 3; i < r.ns; i++)
			best = r.sz[i] < best ? r.sz[i] : best;
		row_add(&r, "autosim", best);
		acc_row(acc, &r);
		acc->raw += plain_raw;
		acc->encs[0] += d * 4 < n;
		bfree(&pb); bfree(&db); bfree(&fb); bfree(&lb); bfree(&dlb); bfree(&cb); bfree(&cb8);
		free(idx); free(codes); free(lens); free(dl); free(cc);
	}
}

/* null bitmaps */
static void
analyze_nulls(const Col *c, Acc *acc)
{
	for (u32 i0 = 0; i0 < c->n; i0 += g_chunk)
	{
		u32			n = c->n - i0 < g_chunk ? c->n - i0 : g_chunk;
		u8		   *bits = calloc(n / 8 + 8, 1);
		u64		   *pos = malloc(sizeof(u64) * n),
				   *runs = malloc(sizeof(u64) * n);
		u32			np = 0,
					nr = 0;
		Row			r = {0};
		Buf			pb,
					rb;
		double		best;

		for (u32 i = 0; i < n; i++)
		{
			int			x = c->nul[i0 + i];

			if (x)
			{
				bits[i >> 3] |= 1 << (i & 7);
				pos[np++] = i;
			}
			if (i && x == c->nul[i0 + i - 1])
				runs[nr - 1]++;
			else
				runs[nr++] = 1;
		}
		pb = enc_int(pos, np, 4, 2, -1, 0);
		rb = enc_int(runs, nr, 4, 2, -1, 0);
		row_add(&r, "bitmap", (double) (n + 7) / 8);
		row_add(&r, "bitmap+lz4", z_size(&algs[0], bits, (n + 7) / 8));
		row_add(&r, "bitmap+zstd3", z_size(&algs[ALG_ZSTD3], bits, (n + 7) / 8));
		row_add(&r, "positions", np ? (double) pb.n : 1);
		row_add(&r, "runs", (double) rb.n);
		best = r.sz[0];
		for (int i = 1; i < r.ns; i++)
			best = r.sz[i] < best ? r.sz[i] : best;
		row_add(&r, "autosim", best);
		acc_row(acc, &r);
		acc->raw += (double) (n + 7) / 8;
		bfree(&pb); bfree(&rb);
		free(bits); free(pos); free(runs);
	}
}

static void
print_acc(const char *tab, const Col *c, const char *suffix, const Acc *a, int *last_ns, const char **last_lab)
{
	if (*last_ns != a->ns || *last_lab != a->lab[1])
	{
		printf("\n%-34s %9s", "column", "raw KB");
		for (int i = 0; i < a->ns; i++)
			printf(" %*.*s", 9, 9, a->lab[i]);
		printf("\n");
		*last_ns = a->ns;
		*last_lab = a->lab[1];
	}
	char		nm[64];

	snprintf(nm, sizeof nm, "%s.%s%s", tab, c->name, suffix);
	printf("%-34s %9.0f", nm, a->raw / 1024);
	for (int i = 0; i < a->ns; i++)
	{
		if (a->sz[i] < 0)
			printf(" %9s", "-");
		else
			printf(" %9.1f", a->sz[i] / 1024);
	}
	if (c->kind == K_INT && !strcmp(suffix, ""))
	{
		printf("  ");
		for (int e = 0; e < E_N; e++)
			if (a->encs[e])
				printf(" %s:%d", enc_name[e], a->encs[e]);
	}
	printf("\n");
}

/* ---- decode speed of the chosen encodings ---- */
static void
range_u32(const u32 *v, u32 n, u32 lo, u32 hi, u64 *mask)
{
	u32			span = hi - lo;

	for (u32 w = 0; w < n / 64; w++)
	{
		u64			m = 0;

		for (int j = 0; j < 64; j++)
			m |= (u64) ((v[w * 64 + j] - lo) <= span) << j;
		mask[w] = m;
	}
}

static volatile u64 sink;

static void
speed_int(const Col *c)
{
	u32			nch = (c->n / g_chunk > 48 ? 48 : c->n / g_chunk);
	Buf		   *enc = calloc(nch, sizeof(Buf)),
			   *encz = calloc(nch, sizeof(Buf));
	u64		  **vals = calloc(nch, sizeof(u64 *));
	u32			total = nch * g_chunk;
	u32		   *out32 = malloc(sizeof(u32) * (g_chunk + 1024));
	u64		   *out64 = malloc(sizeof(u64) * (g_chunk + 1024)),
			   *mask = calloc(g_chunk / 64 + 8, 8);
	double		t_enc,
				t_dec,
				t_filt,
				t_z4,
				t_z3,
				t_raw;
	size_t		sz = 0,
				szz = 0;
	int			wide = c->width == 8;
	char		encs[64] = "";
	u8		   *zbuf = malloc(g_chunk * 8 + 1024),
			   *zout = malloc(g_chunk * 8 + 1024);

	for (u32 k = 0; k < nch; k++)
		vals[k] = widen(c, k * g_chunk, g_chunk);
	TIMEIT(t_enc, total, {
		for (u32 k = 0; k < nch; k++)
		{
			Buf			b = enc_int(vals[k], g_chunk, c->width, 2, -1, 0);

			bfree(&b);
		}
	});
	for (u32 k = 0; k < nch; k++)
	{
		enc[k] = enc_int(vals[k], g_chunk, c->width, 2, -1, 0);
		sz += enc[k].n;
	}
	{
		int			cnt[E_N] = {0};

		for (u32 k = 0; k < nch; k++)
			cnt[node_enc(&enc[k])]++;
		for (int e = 0; e < E_N; e++)
			if (cnt[e])
				snprintf(encs + strlen(encs), sizeof encs - strlen(encs), "%s:%d ", enc_name[e], cnt[e]);
	}
	TIMEIT(t_dec, total, {
		for (u32 k = 0; k < nch; k++)
		{
			scr_top = 0;
			if (wide)
				dec_u64(enc[k].p, enc[k].n, g_chunk, out64);
			else
				dec_u32(enc[k].p, enc[k].n, g_chunk, out32);
		}
		sink += wide ? out64[7] : out32[7];
	});
	if (!wide)
	{
		TIMEIT(t_filt, total, {
			for (u32 k = 0; k < nch; k++)
			{
				scr_top = 0;
				dec_u32(enc[k].p, enc[k].n, g_chunk, out32);
				range_u32(out32, g_chunk, 100, 120, mask);
			}
			sink += mask[0];
		});
		TIMEIT(t_raw, total, {
			for (u32 k = 0; k < nch; k++)
			{
				for (u32 i = 0; i < g_chunk; i++)
					out32[i] = (u32) vals[k][i];
				range_u32(out32, g_chunk, 100, 120, mask);
			}
			sink += mask[0];
		});
	}
	else
		t_filt = t_raw = 0;
	/* zstd3 / lz4 layered over the chosen encoding: decompress cost */
	for (u32 k = 0; k < nch; k++)
	{
		size_t		cl = z_comp(&algs[ALG_ZSTD3], enc[k].p, enc[k].n, zbuf, g_chunk * 8 + 1024);

		szz += cl;
		encz[k].p = malloc(cl);
		encz[k].n = cl;
		memcpy(encz[k].p, zbuf, cl);
	}
	TIMEIT(t_z3, total, {
		for (u32 k = 0; k < nch; k++)
			sink += z_decomp(&algs[ALG_ZSTD3], encz[k].p, encz[k].n, zout, enc[k].n);
	});
	{
		size_t		cl = z_comp(&algs[0], enc[0].p, enc[0].n, zbuf, g_chunk * 8 + 1024);

		TIMEIT(t_z4, total, {
			for (u32 k = 0; k < nch; k++)
				sink += z_decomp(&algs[0], zbuf, cl, zout, enc[0].n);
		});
	}
	printf("%-26s enc %6.2f  dec %5.2f  dec+filter %5.2f (raw array+filter %5.2f)  zstd3 unwrap %5.2f  lz4 unwrap %5.2f  ns/value  size %6.1f B/chunk  %s\n",
		   c->name, t_enc, t_dec, t_filt, t_raw, t_z3, t_z4, (double) sz / nch, encs);
	for (u32 k = 0; k < nch; k++)
	{
		bfree(&enc[k]);
		bfree(&encz[k]);
		free(vals[k]);
	}
	free(enc); free(encz); free(vals); free(out32); free(out64); free(mask); free(zbuf); free(zout);
}

static void
speed_float(const Col *c)
{
	int			w = c->width;
	u32			nch = (c->n / g_chunk > 48 ? 48 : c->n / g_chunk),
				total = nch * g_chunk,
				nexc;
	Buf		   *al = calloc(nch, sizeof(Buf)),
			   *go = calloc(nch, sizeof(Buf));
	u8		   *out = malloc((size_t) g_chunk * 8 + 64);
	double		t_alp,
				t_gor,
				t_ealp,
				t_egor;
	size_t		sa = 0,
				sg = 0;

	TIMEIT(t_ealp, total, {
		for (u32 k = 0; k < nch; k++)
		{
			Buf			b = enc_alp((u8 *) c->d + (size_t) k * g_chunk * w, g_chunk, w, 0, &nexc);

			bfree(&b);
		}
	});
	TIMEIT(t_egor, total, {
		for (u32 k = 0; k < nch; k++)
		{
			Buf			b = enc_gorilla((u8 *) c->d + (size_t) k * g_chunk * w, g_chunk, w);

			bfree(&b);
		}
	});
	for (u32 k = 0; k < nch; k++)
	{
		al[k] = enc_alp((u8 *) c->d + (size_t) k * g_chunk * w, g_chunk, w, 0, &nexc);
		go[k] = enc_gorilla((u8 *) c->d + (size_t) k * g_chunk * w, g_chunk, w);
		sa += al[k].n;
		sg += go[k].n;
	}
	TIMEIT(t_alp, total, {
		for (u32 k = 0; k < nch; k++)
		{
			scr_top = 0;
			dec_alp(al[k].p, g_chunk, w, out);
		}
		sink += out[3];
	});
	TIMEIT(t_gor, total, {
		for (u32 k = 0; k < nch; k++)
			dec_gorilla(go[k].p, g_chunk, w, out);
		sink += out[3];
	});
	/* correctness of the decoders on the measured data */
	for (u32 k = 0; k < nch; k++)
	{
		scr_top = 0;
		dec_alp(al[k].p, g_chunk, w, out);
		if (memcmp(out, (u8 *) c->d + (size_t) k * g_chunk * w, (size_t) g_chunk * w))
			printf("  ALP MISMATCH chunk %u\n", k);
		dec_gorilla(go[k].p, g_chunk, w, out);
		if (memcmp(out, (u8 *) c->d + (size_t) k * g_chunk * w, (size_t) g_chunk * w))
			printf("  GORILLA MISMATCH chunk %u\n", k);
	}
	printf("%-26s alp: enc %6.2f dec %5.2f ns/v %5.2f B/v | gorilla: enc %6.2f dec %5.2f ns/v %5.2f B/v\n", c->name, t_ealp, t_alp,
		   (double) sa / total, t_egor, t_gor, (double) sg / total);
	for (u32 k = 0; k < nch; k++)
	{
		bfree(&al[k]);
		bfree(&go[k]);
	}
	free(al); free(go); free(out);
}

/* compress / decompress speed and ratio of every general-purpose algorithm on representative streams */
static void
speed_z(const Table *ot, const Table *it)
{
	static const char *const names[] = {"orders.description", "items.state_code", "items.category_code", "items.price", "orders.updated_at", "items.order_id"};

	printf("\n=== general-purpose compressors on the chosen lightweight streams (16384-row chunks; MB/s of uncompressed bytes) ===\n");
	for (int w = 0; w < 6; w++)
	{
		const Col  *c = NULL;
		const Table *ts[2] = {ot, it};
		Buf			b = {0},
					bal = {0};
		u32			nch = 0;
		Buf		   *chunks;

		for (int t = 0; t < 2; t++)
			for (int ci = 0; ci < ts[t]->ncol; ci++)
			{
				char		nm[64];

				snprintf(nm, sizeof nm, "%s.%s", ts[t]->name, ts[t]->col[ci].name);
				if (!strcmp(nm, names[w]))
					c = &ts[t]->col[ci];
			}
		if (!c)
			continue;
		nch = c->n / g_chunk > 24 ? 24 : c->n / g_chunk;
		chunks = calloc(nch, sizeof(Buf));
		for (u32 k = 0; k < nch; k++)
		{
			if (c->kind == K_STR)
				bput(&chunks[k], (const char *) c->d + c->off[k * g_chunk], c->off[(k + 1) * g_chunk] - c->off[k * g_chunk]);
			else if (c->kind == K_INT)
			{
				u64		   *v = widen(c, k * g_chunk, g_chunk);
				Buf			e = enc_int(v, g_chunk, c->width, 2, -1, 0);

				bput(&chunks[k], e.p, e.n);
				bfree(&e);
				free(v);
			}
			else
			{
				u32			ne;
				Buf			e = enc_alp((u8 *) c->d + (size_t) k * g_chunk * c->width, g_chunk, c->width, 0, &ne);

				bput(&chunks[k], e.p, e.n);
				bfree(&e);
			}
			b.n += chunks[k].n;
		}
		printf("%-22s %8.0f B/chunk\n", names[w], (double) b.n / nch);
		for (int a = 0; a < 6; a++)
		{
			size_t		csum = 0;
			double		tc,
						td;
			u8		  **cb = calloc(nch, sizeof(u8 *));
			size_t	   *cl = calloc(nch, sizeof(size_t));
			u8		   *out = malloc(1 << 22);

			for (u32 k = 0; k < nch; k++)
			{
				cb[k] = malloc(chunks[k].n + chunks[k].n / 8 + 1024);
				cl[k] = z_comp(&algs[a], chunks[k].p, chunks[k].n, cb[k], chunks[k].n + chunks[k].n / 8 + 1024);
				csum += cl[k];
			}
			TIMEIT(tc, b.n, {
				for (u32 k = 0; k < nch; k++)
					sink += z_comp(&algs[a], chunks[k].p, chunks[k].n, out, 1 << 22);
			});
			TIMEIT(td, b.n, {
				for (u32 k = 0; k < nch; k++)
					sink += z_decomp(&algs[a], cb[k], cl[k], out, chunks[k].n);
			});
			printf("    %-8s ratio %5.2f   compress %7.1f MB/s   decompress %8.1f MB/s\n", algs[a].name, (double) b.n / csum, 1000.0 / tc, 1000.0 / td);
			for (u32 k = 0; k < nch; k++)
				free(cb[k]);
			free(cb); free(cl); free(out);
		}
		for (u32 k = 0; k < nch; k++)
			bfree(&chunks[k]);
		free(chunks);
		(void) bal;
	}
}

int
main(int argc, char **argv)
{
	u32			norders = 400000;
	int			clustered = 0;

	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-n") && i + 1 < argc)
			norders = (u32) atoi(argv[++i]);
		else if (!strcmp(argv[i], "-c") && i + 1 < argc)
			g_chunk = (u32) atoi(argv[++i]);
		else if (!strcmp(argv[i], "-k"))
			clustered = 1;
		else if (!strcmp(argv[i], "-s"))
			g_speed = 1;
	}
	{
		Table		rt = {"orders"},
					ot = {"items"};
		Table	   *ts[2] = {&rt, &ot};
		double		set7[5] = {0},
				bk[2][4][5] = {{{0}}};
		double		tot_raw[2] = {0}, tot_best[2] = {0}, tot_auto[2] = {0}, tot_zstd3[2] = {0}, tot_lw[2] = {0};

		gen_tables(&rt, &ot, norders, clustered);
		printf("chunk %u rows, %s order, %u orders, %u items; sizes in KB (1024 B)\n", g_chunk, clustered ? "clustered" : "heap", rt.n, ot.n);
		for (int t = 0; t < 2; t++)
		{
			int			last_ns = -1;
			const char *last_lab = NULL;

			for (int ci = 0; ci < ts[t]->ncol; ci++)
			{
				const Col  *c = &ts[t]->col[ci];
				Acc		   *a = calloc(1, sizeof(Acc));

				if (c->kind == K_INT)
					analyze_int(c, a);
				else if (c->kind == K_STR)
					analyze_str(c, a);
				else
					analyze_float(c, a);
				print_acc(ts[t]->name, c, "", a, &last_ns, &last_lab);
				if (t == 1 && c->kind == K_INT && strstr("order_id store_id order_date category_code variant_code state_code price_x2", c->name))
				{
					set7[0] += a->raw;
					set7[1] += a->sz[2];
					set7[2] += a->sz[3];
					set7[3] += a->sz[8];
					set7[4] += a->sz[a->ns - 1];
				}
				{
					int			li = c->kind == K_INT ? 8 : c->kind == K_STR ? 5 : 6,
								zi = c->kind == K_INT ? 12 : c->kind == K_STR ? 9 : 10;

					bk[t][c->kind][0] += a->raw;
					bk[t][c->kind][1] += a->sz[2];
					bk[t][c->kind][2] += a->sz[li];
					bk[t][c->kind][3] += a->sz[zi];
					bk[t][c->kind][4] += a->sz[a->ns - 1];
				}
				tot_raw[t] += a->raw;
				tot_auto[t] += a->sz[a->ns - 1];
				tot_zstd3[t] += a->sz[2];
				if (c->kind == K_INT)
					tot_lw[t] += a->sz[7];
				else
					tot_lw[t] += a->sz[a->ns - 1];
				tot_best[t] += a->sz[a->ns - 1];
				free(a);
				if (c->nul)
				{
					Acc		   *b = calloc(1, sizeof(Acc));
					int			ln = -1;
					const char *ll = NULL;

					analyze_nulls(c, b);
					print_acc(ts[t]->name, c, " (nulls)", b, &ln, &ll);
					tot_raw[t] += b->raw;
					tot_auto[t] += b->sz[b->ns - 1];
					free(b);
				}
			}
			for (int k = 0; k < 4; k++)
				if (bk[t][k][0] > 0)
					printf("\nKIND %-7s %-5s raw %8.1f  plain+zstd3 %8.1f  lightweight %8.1f  lightweight+zstd3 %8.1f  autosim %8.1f  KB", ts[t]->name, k == K_INT ? "int" : k == K_STR ? "str" : "float", bk[t][k][0] / 1024, bk[t][k][1] / 1024, bk[t][k][2] / 1024, bk[t][k][3] / 1024, bk[t][k][4] / 1024);
			printf("\nTOTAL %-9s raw %.1f MB   plain+zstd3 %.1f MB   autosim %.1f MB  (%.2f B/row, ratio %.2fx)\n", ts[t]->name,
				   tot_raw[t] / 1048576, tot_zstd3[t] / 1048576, tot_auto[t] / 1048576, tot_auto[t] / ts[t]->n, tot_raw[t] / tot_auto[t]);
		}
		printf("\nSET7 (items: order_id store_id order_date category_code variant_code state_code price_x2), B/row: plain %.2f  plain+zstd3 %.2f  for(bit-packed) %.2f  best-lightweight %.2f  autosim %.2f\n",
			   set7[0] / ot.n, set7[1] / ot.n, set7[2] / ot.n, set7[3] / ot.n, set7[4] / ot.n);
		if (g_speed)
		{
			speed_z(&rt, &ot);
			printf("\n=== speed (CPU ns per value, scalar C at the build's -O level) ===\n");
			for (int t = 0; t < 2; t++)
				for (int ci = 0; ci < ts[t]->ncol; ci++)
				{
					const Col  *c = &ts[t]->col[ci];

					if (c->kind == K_INT)
						speed_int(c);
					else if (c->kind != K_STR)
						speed_float(c);
				}
		}
	}
	return 0;
}
