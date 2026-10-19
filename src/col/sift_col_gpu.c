/*
 * sift_col_gpu.c - a codec blob read as the fields a kernel decodes (sift_col_gpu.h). The node layout is the codec's
 * (sift_colcodec.c): a 24 byte blob head (magic, version, kind, flags, u32 n, i64 min, i64 max), the root node, a check word; a node is
 * 16 bytes (tag, parameter, u32 f1, 8 bytes of fields) followed by its payload, 8-byte aligned.
 */
#include "postgres.h"

#include "sift_col_gpu.h"
#include "sift_colcodec.h"
#include "sift_colz.h"

#define NHDR	16
#define SIFT_CGPU_PAD	64		/* bytes after a decompressed node, as the codec leaves */
#define RUP8(x)	(((uint64) (x) + 7) & ~(uint64) 7)

static inline uint32
rd32(const uint8 *p)
{
	uint32		x;

	memcpy(&x, p, 4);
	return x;
}

static inline int64
rd64(const uint8 *p)
{
	int64		x;

	memcpy(&x, p, 8);
	return x;
}

/* 32-bit words of n values at width W in the vertical layout (the codec's sift_cc_words) */
static uint64
vwords(uint32 n, uint32 W)
{
	uint64		rows = ((uint64) n + 31) >> 5,
				w = (rows / 32) * 32 * W;

	if (rows % 32)
		w += 32 * (((rows % 32) * W + 31) / 32);
	return w;
}

static bool
fit64(__int128 v, int64 *out)
{
	if (v < PG_INT64_MIN || v > PG_INT64_MAX)
		return false;
	*out = (int64) v;
	return true;
}

/* The leaf node at p (avail bytes, the n values of the chunk or dictionary); *size is the node's length */
static bool
leaf_node(const uint8 *blob, const uint8 *p, uint64 avail, uint32 n, int depth, SiftCGpuLeaf *lf, uint64 *size)
{
	uint32		f1;

	if (avail < NHDR || depth > SIFT_CC_MAXDEPTH)
		return false;
	f1 = rd32(p + 4);
	memset(lf, 0, sizeof(*lf));
	lf->mul = 1;
	switch (p[0])
	{
		case SIFT_CCN_CONST:
			lf->kind = SIFT_CGL_CONST;
			lf->base = rd64(p + 8);
			*size = NHDR;
			return true;
		case SIFT_CCN_FORBIT:
			lf->kind = SIFT_CGL_BIT;
			lf->W = p[1];
			if (lf->W < 1 || lf->W > 32)
				return false;
			lf->base = rd64(p + 8);
			lf->nwords = (uint32) vwords(n, lf->W);
			*size = NHDR + RUP8((uint64) lf->nwords * 4);
			lf->woff = (uint32) (p - blob) + NHDR;
			return *size <= avail;
		case SIFT_CCN_FORBYTE:
			lf->kind = SIFT_CGL_BYTE;
			lf->W = p[1];
			if (lf->W != 1 && lf->W != 2 && lf->W != 4)
				return false;
			lf->base = rd64(p + 8);
			lf->nwords = (uint32) (((uint64) n * lf->W + 3) / 4);
			*size = NHDR + RUP8((uint64) n * lf->W);
			lf->woff = (uint32) (p - blob) + NHDR;
			return *size <= avail;
		case SIFT_CCN_WRAP:
			{
				uint32		cl = rd32(p + 8);
				uint64		isize;
				uint8	   *buf;

				*size = NHDR + RUP8(cl);
				if (*size > avail || !sift_colz_available(p[1]) || f1 < NHDR || (f1 & 7) || f1 > (uint64) n * 9 + 4096 + 64)
					return false;
				buf = palloc(f1 + SIFT_CGPU_PAD);
				if (!sift_colz_decompress(p[1], p + NHDR, cl, buf, f1) || !leaf_node(buf, buf, f1, n, depth + 1, lf, &isize) || isize != f1)
				{
					pfree(buf);
					return false;
				}
				if (lf->kind == SIFT_CGL_CONST)
					pfree(buf);
				else if (lf->wbuf == NULL)
					lf->wbuf = buf;		/* a wrapper inside this one owns the words already */
				return true;
			}
		case SIFT_CCN_SCALE:
			{
				int64		sbase = rd64(p + 8),
							mul = rd64(p + 16),
							b,
							m;
				uint64		csize;

				if (avail < 24 || f1 > avail - 24 || mul < 1)
					return false;
				if (!leaf_node(blob, p + 24, f1, n, depth + 1, lf, &csize))
					return false;
				if (!fit64((__int128) sbase + (__int128) mul * lf->base, &b) || !fit64((__int128) mul * lf->mul, &m))
					return false;
				lf->base = b;
				lf->mul = m;
				*size = 24 + (uint64) f1;
				return true;
			}
		default:
			return false;
	}
}

/* Field i of the leaf (not its value) */
static uint32
field_of(const SiftCGpuLeaf *lf, const uint8 *blob, uint32 i)
{
	const uint8 *w = (lf->wbuf ? lf->wbuf : blob) + lf->woff;
	uint32		x = 0;

	switch (lf->kind)
	{
		case SIFT_CGL_BIT:
			{
				uint32		b = i / 1024,
							r = (i % 1024) / 32,
							l = i % 32,
							bp = r * lf->W,
							s = bp & 31,
							x2;
				size_t		o = (size_t) b * 32 * lf->W + (size_t) (bp >> 5) * 32 + l;

				memcpy(&x, w + o * 4, 4);
				x >>= s;
				if (s + lf->W > 32)
				{
					memcpy(&x2, w + (o + 32) * 4, 4);
					x |= x2 << (32 - s);
				}
				return lf->W < 32 ? x & ((UINT32_C(1) << lf->W) - 1) : x;
			}
		case SIFT_CGL_BYTE:
			memcpy(&x, w + (size_t) i * lf->W, lf->W);
			return x;
		default:
			return 0;
	}
}

uint32
sift_cgpu_fmax(const SiftCGpuLeaf *lf)
{
	switch (lf->kind)
	{
		case SIFT_CGL_BIT:
			return lf->W < 32 ? (UINT32_C(1) << lf->W) - 1 : UINT32_MAX;
		case SIFT_CGL_BYTE:
			return lf->W < 4 ? (UINT32_C(1) << (8 * lf->W)) - 1 : UINT32_MAX;
		default:
			return 0;
	}
}

void
sift_cgpu_plan_const(int64 v, uint32 n, SiftCGpuPlan *p)
{
	memset(p, 0, sizeof(*p));
	p->leaf.kind = SIFT_CGL_CONST;
	p->leaf.base = v;
	p->leaf.mul = 1;
	p->n = n;
	p->vmin = p->vmax = v;
}

bool
sift_cgpu_plan(const uint8 *blob, uint32 len, SiftCGpuPlan *p)
{
	const uint8 *root = blob + 24;
	uint64		avail,
				size;

	memset(p, 0, sizeof(*p));
	if (len < 24 + NHDR || blob[0] != SIFT_CC_MAGIC || blob[1] != SIFT_CC_VERSION || blob[2] != SIFT_CC_INT)
		return false;
	p->n = rd32(blob + 4);
	p->vmin = rd64(blob + 8);
	p->vmax = rd64(blob + 16);
	avail = len - 24;
	if (p->n == 0 || p->n > SIFT_CC_MAXROWS)
		return false;
	if (root[0] != SIFT_CCN_DICT)
		return leaf_node(blob, root, avail, p->n, 0, &p->leaf, &size);
	{
		uint32		d = rd32(root + 4),
					dl = rd32(root + 8),
					cl = rd32(root + 12);
		SiftCGpuLeaf dict;

		if (d < 1 || d > SIFT_CGPU_DICT_MAX || avail < NHDR || (uint64) dl + cl > avail - NHDR)
			return false;
		if (!leaf_node(blob, root + NHDR, dl, d, 1, &dict, &size) || !leaf_node(blob, root + NHDR + dl, cl, p->n, 1, &p->leaf, &size))
			return false;
		p->d = d;
		p->dval = palloc(d * sizeof(int64));
		for (uint32 i = 0; i < d; i++)
		{
			if (!fit64((__int128) dict.base + (__int128) dict.mul * field_of(&dict, blob, i), &p->dval[i]) || (i > 0 && p->dval[i] <= p->dval[i - 1]))
			{
				pfree(p->dval);
				p->dval = NULL;
				return false;
			}
		}
		if (dict.wbuf)
			pfree((void *) dict.wbuf);
		return true;
	}
}

bool
sift_cgpu_value(const SiftCGpuPlan *p, uint32 f, int64 *v)
{
	__int128	x = (__int128) p->leaf.base + (__int128) p->leaf.mul * f;

	if (p->d == 0)
		return fit64(x, v);
	if (x < 0 || x >= p->d)
		return false;
	*v = p->dval[(uint32) x];
	return true;
}

uint32
sift_cgpu_fhi(const SiftCGpuPlan *p)
{
	__int128	top = p->d ? (__int128) p->d - 1 : (__int128) p->vmax,
				f;
	uint32		fmax = sift_cgpu_fmax(&p->leaf);

	if (p->leaf.kind == SIFT_CGL_CONST)
		return 0;
	f = top - p->leaf.base;
	if (f < 0)
		return 0;
	f /= p->leaf.mul;
	return f > fmax ? fmax : (uint32) f;
}

/* The fields f of lf with lo <= base + mul * f <= hi, as a range of [0, fmax] */
static SiftCGpuRange
field_range(const SiftCGpuLeaf *lf, int64 lo, int64 hi, uint32 *fl, uint32 *fh)
{
	__int128	a = (__int128) lo - lf->base,
				b = (__int128) hi - lf->base,
				l,
				h;
	uint32		fmax = sift_cgpu_fmax(lf);

	if (lo > hi || b < 0)
		return SIFT_CGR_NONE;
	l = a <= 0 ? 0 : (a + lf->mul - 1) / lf->mul;
	h = b / lf->mul;
	if (h > fmax)
		h = fmax;
	if (l > h)
		return SIFT_CGR_NONE;
	*fl = (uint32) l;
	*fh = (uint32) h;
	return l == 0 && h == fmax ? SIFT_CGR_ALL : SIFT_CGR_RANGE;
}

/* The first entry of the sorted values that is >= v (> v with `after`), d if none */
static uint32
dict_bound(const SiftCGpuPlan *p, int64 v, bool after)
{
	uint32		l = 0,
				h = p->d;

	while (l < h)
	{
		uint32		m = l + (h - l) / 2;

		if (after ? p->dval[m] <= v : p->dval[m] < v)
			l = m + 1;
		else
			h = m;
	}
	return l;
}

SiftCGpuRange
sift_cgpu_range(const SiftCGpuPlan *p, int64 lo, int64 hi, uint32 *fl, uint32 *fh)
{
	uint32		a,
				b;

	if (lo > hi || lo > p->vmax || hi < p->vmin)
		return SIFT_CGR_NONE;
	if (lo <= p->vmin && hi >= p->vmax)
		return SIFT_CGR_ALL;
	if (p->d == 0)
		return field_range(&p->leaf, lo, hi, fl, fh);
	/* a dictionary: the entries inside [lo, hi] are a run of the sorted values, and their codes a range of the code field */
	a = dict_bound(p, lo, false);
	b = dict_bound(p, hi, true);
	if (a >= b)
		return SIFT_CGR_NONE;
	if (a == 0 && b == p->d)
		return SIFT_CGR_ALL;
	return field_range(&p->leaf, a, (int64) b - 1, fl, fh);
}
