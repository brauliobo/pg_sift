/*
 * tools/colcodec/research_float.h - the float encoders of research.c: ALP, byte-stream-split, Gorilla (throwaway experiments).
 */
#ifndef RESEARCH_FLOAT_H
#define RESEARCH_FLOAT_H

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

#endif
