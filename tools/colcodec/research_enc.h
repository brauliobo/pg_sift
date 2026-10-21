/*
 * tools/colcodec/research_enc.h - the encoders and compressors of research.c: bit-packing, integer nodes (CONST FOR DELTA RLE DICT), lz4 / zstd.
 */
#ifndef RESEARCH_ENC_H
#define RESEARCH_ENC_H

#include <lz4.h>
#include <lz4hc.h>
#include <zstd.h>

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

#endif
