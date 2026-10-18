/*
 * sift_aggkern_cases.h - random inputs of every aggregation operator and a runner that feeds all their outputs to a hash, shared by
 * the SQL test (sift_aggkern_test.c) and the standalone harness (tools/aggkern). Test code only.
 *
 * The includer defines AGK_ALLOC(bytes) (SIFT_SIMD_ALIGN aligned, exactly that size, so that a sanitizer sees any overrun) and
 * AGK_FREE(ptr). Every input array gets SIFT_AGG_PAD bytes of random garbage after its last element (the padding contract: a level
 * must not depend on it), every output is poisoned before the call and allocated at its exact size.
 */
#ifndef SIFT_AGGKERN_CASES_H
#define SIFT_AGGKERN_CASES_H

#include "sift_aggkern_int.h"

#define AGK_MAXMEM		64
#define AGK_MAXSTEP		128

typedef struct AgkRng
{
	uint64		s[4];
} AgkRng;

static inline uint64
agk_split(uint64 *x)
{
	uint64		z = (*x += UINT64CONST(0x9E3779B97F4A7C15));

	z = (z ^ (z >> 30)) * UINT64CONST(0xBF58476D1CE4E5B9);
	z = (z ^ (z >> 27)) * UINT64CONST(0x94D049BB133111EB);
	return z ^ (z >> 31);
}

static inline void
agk_seed(AgkRng *r, uint64 seed)
{
	for (int i = 0; i < 4; i++)
		r->s[i] = agk_split(&seed);
}

/* xoshiro256** */
static inline uint64
agk_next(AgkRng *r)
{
	uint64	   *s = r->s,
				x = s[1] * 5,
				t = s[1] << 17;

	x = ((x << 7) | (x >> 57)) * 9;
	s[2] ^= s[0];
	s[3] ^= s[1];
	s[1] ^= s[2];
	s[0] ^= s[3];
	s[2] ^= t;
	s[3] = (s[3] << 45) | (s[3] >> 19);
	return x;
}

static inline uint32
agk_below(AgkRng *r, uint32 n)
{
	return (uint32) (((agk_next(r) >> 32) * n) >> 32);
}

static inline double
agk_unit(AgkRng *r)
{
	return (agk_next(r) >> 11) * 0x1p-53;
}

/* The inputs of one run of every operator */
typedef struct AgkCase
{
	int			n;
	int			nw;
	int32	   *i32;
	int64	   *i64;
	double	   *f64;
	uint32	   *code;			/* < ncode */
	uint8	   *bytes;
	uint64	   *mask;			/* n bits */
	uint64	   *mask2;
	int			ncode;
	uint8	   *lut;			/* ncode entries */
	uint32	   *map;			/* ncode entries */
	int32	   *v2;				/* around gbase, for lut_i32 */
	int32		gbase;
	uint32		nlut;
	uint8	   *lut2;
	int32	   *k32;			/* kbase + (0 .. msz - 1), for gid_i32 */
	int32		kbase;
	int			msz;
	uint32	   *map2;
	uint32		mult;
	uint32	   *gid0;			/* initial group ids of gid_code / gid_i32 */
	int			ngrp;
	uint32	   *gid;			/* < ngrp */
	int64	   *cnt0;
	int64	   *iacc0;
	double	   *facc0;
	int			nrows;
	uint64	   *rowmask;
	uint32	   *parent;			/* n entries < nrows */
	int			nrs;
	uint32	   *rsel;			/* nrs entries < n, any order */
	uint32		prev0;
	int32		c32,
				lo32,
				hi32;
	int64		c64,
				lo64,
				hi64;
	double		cf,
				lof,
				hif,
				cf2;
	void	   *mem[AGK_MAXMEM];
	int			nmem;
} AgkCase;

/* The hash of all outputs, and its value after each operator (to find the first one that differs) */
typedef struct AgkRes
{
	uint64		h;
	int			nstep;
	uint64		step[AGK_MAXSTEP];
	const char *label[AGK_MAXSTEP];
} AgkRes;

static inline void *
agk_new(AgkCase *c, AgkRng *r, size_t bytes)
{
	uint8	   *p = AGK_ALLOC(bytes + SIFT_AGG_PAD);

	for (int i = 0; i < SIFT_AGG_PAD; i++)
		p[bytes + i] = (uint8) agk_next(r);
	c->mem[c->nmem++] = p;
	return p;
}

#define AGK_ARR(c, r, T, cnt)	((T *) agk_new(c, r, (size_t) (cnt) * sizeof(T)))

static inline void
agk_case_free(AgkCase *c)
{
	for (int i = 0; i < c->nmem; i++)
		AGK_FREE(c->mem[i]);
	c->nmem = 0;
}

static inline int
agk_pick_n(AgkRng *r)
{
	int			k = agk_below(r, 65);

	switch (agk_below(r, 5))
	{
		case 0:
			return agk_below(r, 130);
		case 1:
			k = k * 64 + (int) agk_below(r, 3) - 1;
			return Min(SIFT_AGG_VEC, Max(0, k));
		case 2:
			return SIFT_AGG_VEC - agk_below(r, 3);
		default:
			return agk_below(r, SIFT_AGG_VEC + 1);
	}
}

static inline int32
agk_i32(AgkRng *r, int mode)
{
	static const int32 ext[] = {INT32_MIN, INT32_MIN + 1, -2, -1, 0, 1, 2, INT32_MAX - 1, INT32_MAX};

	switch (mode)
	{
		case 0:
			return (int32) agk_below(r, 17) - 8;
		case 1:
			return (int32) agk_next(r);
		case 2:
			return agk_below(r, 2) ? ext[agk_below(r, lengthof(ext))] : (int32) agk_next(r);
		default:
			return 1000000 + (int32) agk_below(r, 1000) - 500;
	}
}

static inline int64
agk_i64(AgkRng *r, int mode)
{
	static const int64 ext[] = {INT64_MIN, INT64_MIN + 1, -(INT64CONST(1) << 32), -(INT64CONST(1) << 31) - 1, -1, 0, 1,
								INT64CONST(1) << 31, INT64CONST(1) << 32, (INT64CONST(1) << 32) - 1, INT64_MAX - 1, INT64_MAX};

	switch (mode)
	{
		case 0:
			return (int64) agk_below(r, 17) - 8;
		case 1:
			return (int64) agk_next(r);
		case 2:
			return agk_below(r, 2) ? ext[agk_below(r, lengthof(ext))] : (int64) agk_next(r);
		default:
			return (INT64CONST(1) << 40) + (int64) agk_below(r, 1000) - 500;
	}
}

static inline double
agk_bits_f64(uint64 b)
{
	double		d;

	memcpy(&d, &b, sizeof(d));
	return d;
}

static inline double
agk_special(AgkRng *r)
{
	static const uint64 sp[] = {
		0, UINT64CONST(0x8000000000000000), UINT64CONST(0x3FF0000000000000), UINT64CONST(0xBFF0000000000000),
		UINT64CONST(0x7FF0000000000000), UINT64CONST(0xFFF0000000000000), UINT64CONST(0x7FF8000000000000),
		UINT64CONST(0xFFF8000000000000), UINT64CONST(0x7FF8000000000123), UINT64CONST(0x7FF0000000000001),
		UINT64CONST(0x7FEFFFFFFFFFFFFF), UINT64CONST(0xFFEFFFFFFFFFFFFF), 1, UINT64CONST(0x8000000000000001),
		UINT64CONST(0x0010000000000000), UINT64CONST(0x3FE0000000000000), UINT64CONST(0x4004000000000000)};

	return agk_bits_f64(sp[agk_below(r, lengthof(sp))]);
}

static inline double
agk_f64(AgkRng *r, int mode)
{
	switch (mode)
	{
		case 0:
			return (agk_unit(r) - 0.5) * 200;
		case 1:
			{
				int			v = (int) agk_below(r, 7) - 3;

				return v == 0 && agk_below(r, 2) ? -0.0 : v;
			}
		case 2:
			return agk_special(r);
		default:
			return agk_below(r, 4) ? (agk_unit(r) - 0.5) * 1e6 : agk_special(r);
	}
}

/* A threshold near x: x, a neighbour, an extreme or random */
static inline int64
agk_pick_int(AgkRng *r, int64 x, int64 lo, int64 hi)
{
	switch (agk_below(r, 5))
	{
		case 0:
			return x;
		case 1:
			return x < hi ? x + 1 : x;
		case 2:
			return x > lo ? x - 1 : x;
		case 3:
			return agk_below(r, 2) ? lo : hi;
		default:
			{
				const int64 x = (int64) agk_next(r);

				return Max(lo, Min(hi, x));
			}
	}
}

static inline double
agk_pick_f64(AgkRng *r, const double *v, int n)
{
	return n > 0 && agk_below(r, 10) < 7 ? v[agk_below(r, n)] : agk_special(r);
}

static inline void
agk_gen_mask(AgkRng *r, uint64 *m, int n)
{
	static const double dens[] = {0.01, 0.1, 0.5, 0.9, 0.99};
	const int	mode = agk_below(r, 7),
				one = n > 0 ? agk_below(r, n) : 0;
	const double d = dens[agk_below(r, lengthof(dens))];
	bool		on = agk_below(r, 2);

	memset(m, 0, AGK_NW(n) * 8);
	for (int i = 0; i < n; i++)
	{
		bool		set;

		switch (mode)
		{
			case 0:
				set = false;
				break;
			case 1:
				set = true;
				break;
			case 2:
				set = agk_unit(r) < d;
				break;
			case 3:
				on ^= agk_below(r, 1 + agk_below(r, 100)) == 0;
				set = on;
				break;
			case 4:
				set = i == one;
				break;
			case 5:
				set = i & 1;
				break;
			default:
				set = i % 3 == 0;
		}
		m[i >> 6] |= (uint64) set << (i & 63);
	}
}

static inline uint32
agk_pick_size(AgkRng *r, int max)
{
	static const int sizes[] = {1, 2, 3, 63, 64, 65, 255, 256, 257, 1000, 4096};
	int			s = agk_below(r, 3) ? sizes[agk_below(r, lengthof(sizes))] : 1 + agk_below(r, max);

	return Min(s, max);
}

static inline uint32
parent_step(AgkRng *r, uint32 prev, int nrows)
{
	const uint32 next = prev + agk_below(r, 2);

	return Min((uint32) nrows - 1, next);
}

static inline void
agk_case_gen(AgkRng *r, AgkCase *c, int n)
{
	const int	m32 = agk_below(r, 4),
				m64 = agk_below(r, 4),
				mf = agk_below(r, 4),
				cmode = agk_below(r, 4);

	memset(c, 0, sizeof(*c));
	c->n = n >= 0 ? n : agk_pick_n(r);
	n = c->n;
	c->nw = AGK_NW(n);

	c->i32 = AGK_ARR(c, r, int32, n);
	c->i64 = AGK_ARR(c, r, int64, n);
	c->f64 = AGK_ARR(c, r, double, n);
	c->bytes = AGK_ARR(c, r, uint8, n);
	c->code = AGK_ARR(c, r, uint32, n);
	c->parent = AGK_ARR(c, r, uint32, n);
	c->gid = AGK_ARR(c, r, uint32, n);
	c->gid0 = AGK_ARR(c, r, uint32, n);
	c->v2 = AGK_ARR(c, r, int32, n);
	c->k32 = AGK_ARR(c, r, int32, n);
	c->mask = AGK_ARR(c, r, uint64, c->nw);
	c->mask2 = AGK_ARR(c, r, uint64, c->nw);
	agk_gen_mask(r, c->mask, n);
	agk_gen_mask(r, c->mask2, n);

	c->ncode = agk_pick_size(r, 5000);
	c->nlut = agk_below(r, 4) == 0 ? agk_below(r, 3) : agk_pick_size(r, 1000);
	c->msz = agk_pick_size(r, 3000);
	c->ngrp = agk_pick_size(r, 300);
	c->nrows = agk_pick_size(r, SIFT_AGG_VEC);
	c->lut = AGK_ARR(c, r, uint8, c->ncode);
	c->map = AGK_ARR(c, r, uint32, c->ncode);
	c->lut2 = AGK_ARR(c, r, uint8, c->nlut);
	c->map2 = AGK_ARR(c, r, uint32, c->msz);
	c->cnt0 = AGK_ARR(c, r, int64, c->ngrp);
	c->iacc0 = AGK_ARR(c, r, int64, c->ngrp);
	c->facc0 = AGK_ARR(c, r, double, c->ngrp);
	c->rowmask = AGK_ARR(c, r, uint64, AGK_NW(c->nrows));

	for (int i = 0; i < n; i++)
	{
		c->i32[i] = agk_i32(r, m32);
		c->i64[i] = agk_i64(r, m64);
		c->f64[i] = agk_f64(r, mf);
		c->bytes[i] = agk_below(r, 3) ? 0 : 1 + agk_below(r, 255);
	}
	if (agk_below(r, 3) == 0)
		for (int i = 0; i < n; i++)
			c->bytes[i] = 0;

	/* dictionary codes: uniform, runs, near the top of the LUT, sorted */
	for (int i = 0; i < n; i++)
		c->code[i] = cmode == 0 ? agk_below(r, c->ncode) :
			cmode == 1 ? (i > 0 && agk_below(r, 10) ? c->code[i - 1] : agk_below(r, c->ncode)) :
			cmode == 2 ? c->ncode - 1 - agk_below(r, Min(c->ncode, 4)) : (uint32) ((int64) i * c->ncode / Max(n, 1));
	{
		const double d = agk_unit(r);

		for (int i = 0; i < c->ncode; i++)
		{
			c->lut[i] = agk_unit(r) < d ? (uint8) (agk_below(r, 2) ? 1 + agk_below(r, 255) : 0x80) : 0;
			c->map[i] = agk_below(r, 64);
		}
		c->lut[c->ncode - 1] = agk_below(r, 2) ? 0xFF : 0;
		for (uint32 i = 0; i < c->nlut; i++)
			c->lut2[i] = agk_unit(r) < d ? 1 + agk_below(r, 255) : 0;
	}
	for (int i = 0; i < c->msz; i++)
		c->map2[i] = agk_below(r, 50);

	/* lut_i32: values around gbase + [0, nlut), with the boundaries on both sides, wrapping */
	c->gbase = agk_below(r, 3) ? agk_i32(r, 2) : 0;
	for (int i = 0; i < n; i++)
		c->v2[i] = (int32) ((uint32) c->gbase + (uint32) ((int) agk_below(r, c->nlut + 20) - 10));
	c->kbase = agk_i32(r, 2);
	for (int i = 0; i < n; i++)
		c->k32[i] = (int32) ((uint32) c->kbase + agk_below(r, c->msz));

	c->mult = agk_below(r, 2) ? agk_below(r, 6) : (uint32) agk_next(r);
	for (int i = 0; i < n; i++)
	{
		c->gid[i] = agk_below(r, c->ngrp);
		c->gid0[i] = agk_below(r, 2) ? (uint32) agk_next(r) : agk_below(r, 1000);
	}
	for (int g = 0; g < c->ngrp; g++)
	{
		c->cnt0[g] = agk_below(r, 100);
		c->iacc0[g] = (int64) agk_next(r) >> agk_below(r, 60);
		c->facc0[g] = agk_f64(r, mf);
	}

	for (int w = 0; w < AGK_NW(c->nrows); w++)
		c->rowmask[w] = agk_next(r);
	{
		const int	pmode = agk_below(r, 3);

		for (int e = 0; e < n; e++)
			c->parent[e] = pmode == 0 ? agk_below(r, c->nrows) :
				pmode == 1 ? (uint32) ((int64) e * c->nrows / Max(n, 1)) :
				(e > 0 && agk_below(r, 4) ? parent_step(r, c->parent[e - 1], c->nrows) : agk_below(r, c->nrows));
	}
	c->nrs = n > 0 ? agk_below(r, n + 1) : 0;
	c->rsel = AGK_ARR(c, r, uint32, c->nrs);
	for (int j = 0; j < c->nrs; j++)
		c->rsel[j] = agk_below(r, n);
	c->prev0 = agk_below(r, 3) ? UINT32_MAX : agk_below(r, c->ncode);

#define PICK(arr, lo, hi)	agk_pick_int(r, n > 0 ? (arr)[agk_below(r, n)] : 0, lo, hi)
	{
		int64		x = PICK(c->i32, INT32_MIN, INT32_MAX),
					y = PICK(c->i32, INT32_MIN, INT32_MAX),
					z = PICK(c->i64, INT64_MIN, INT64_MAX),
					t = PICK(c->i64, INT64_MIN, INT64_MAX);

		c->c32 = PICK(c->i32, INT32_MIN, INT32_MAX);
		c->lo32 = Min(x, y);
		c->hi32 = Max(x, y);
		c->c64 = PICK(c->i64, INT64_MIN, INT64_MAX);
		c->lo64 = Min(z, t);
		c->hi64 = Max(z, t);
	}
#undef PICK
	c->cf = agk_pick_f64(r, c->f64, n);
	c->lof = agk_pick_f64(r, c->f64, n);
	c->hif = agk_pick_f64(r, c->f64, n);
	c->cf2 = agk_special(r);
}

static inline void
agk_hash(AgkRes *res, const void *p, size_t len)
{
	const uint8 *b = p;
	uint64		h = res->h;
	size_t		i = 0;

	for (; i + 8 <= len; i += 8)
	{
		uint64		x;

		memcpy(&x, b + i, 8);
		h = (h ^ x) * UINT64CONST(0x9E3779B97F4A7C15);
		h ^= h >> 29;
	}
	for (; i < len; i++)
		h = (h ^ b[i]) * UINT64CONST(0x100000001B3);
	res->h = h;
}

#define AGK_HV(res, x)	do { __typeof__(x) v_ = (x); agk_hash(res, &v_, sizeof(v_)); } while (0)

static inline void
agk_step(AgkRes *res, const char *label)
{
	res->label[res->nstep] = label;
	res->step[res->nstep++] = res->h;
}

/* The first step at which two runs differ, -1 if none */
static inline int
agk_first_diff(const AgkRes *a, const AgkRes *b)
{
	for (int i = 0; i < a->nstep; i++)
		if (a->step[i] != b->step[i])
			return i;
	return -1;
}

static inline uint64 *
agk_out_words(int nw)
{
	return AGK_ALLOC(nw > 0 ? (size_t) nw * 8 : 8);
}

/* Runs every operator of table k over the case, hashing every output */
static inline void
agk_run(const SiftAggKernels *k, const AgkCase *c, AgkRes *res)
{
	static const SiftCmpOp ops[] = {SCMP_EQ, SCMP_NE, SCMP_LT, SCMP_LE, SCMP_GT, SCMP_GE};
	const int	n = c->n,
				nw = c->nw;
	const uint64 *masks[] = {c->mask, NULL};
	uint64	   *bm = agk_out_words(nw),
			   *w2 = agk_out_words(nw);
	uint32	   *sel = AGK_ALLOC((size_t) (64 * nw + SIFT_AGG_PAD / 4) * 4),
			   *ssel = AGK_ALLOC((size_t) (64 * nw + SIFT_AGG_PAD / 4) * 4),
			   *g = AGK_ALLOC(n > 0 ? (size_t) n * 4 : 4);
	int			nssel;

	memset(res, 0, sizeof(*res));
	res->h = UINT64CONST(0xCBF29CE484222325);

#define BM(label, call) do { memset(bm, 0xA5, (size_t) nw * 8); call; agk_hash(res, bm, (size_t) nw * 8); agk_step(res, label); } while (0)
	for (int o = 0; o < lengthof(ops); o++)
	{
		BM("cmp_i32", k->cmp_i32(bm, c->i32, n, ops[o], c->c32));
		BM("cmp_i64", k->cmp_i64(bm, c->i64, n, ops[o], c->c64));
		BM("cmp_f64", k->cmp_f64(bm, c->f64, n, ops[o], c->cf));
		BM("cmp_f64 special", k->cmp_f64(bm, c->f64, n, ops[o], c->cf2));
	}
	BM("between_i32", k->between_i32(bm, c->i32, n, c->lo32, c->hi32));
	BM("between_i32 empty", k->between_i32(bm, c->i32, n, c->hi32, c->lo32));
	BM("between_i64", k->between_i64(bm, c->i64, n, c->lo64, c->hi64));
	BM("between_i64 empty", k->between_i64(bm, c->i64, n, c->hi64, c->lo64));
	BM("between_f64", k->between_f64(bm, c->f64, n, c->lof, c->hif));
	BM("between_f64 swapped", k->between_f64(bm, c->f64, n, c->hif, c->lof));
	BM("between_f64 special lo", k->between_f64(bm, c->f64, n, c->cf2, c->hif));
	BM("between_f64 special hi", k->between_f64(bm, c->f64, n, c->lof, c->cf2));
	BM("lut_code", k->lut_code(bm, c->code, n, c->lut));
	BM("lut_i32", k->lut_i32(bm, c->v2, n, c->gbase, c->nlut, c->lut2));
	BM("bytes_mask", k->bytes_mask(bm, c->bytes, n));

	memcpy(w2, c->mask, (size_t) nw * 8);
	k->and_w(w2, c->mask2, nw);
	agk_hash(res, w2, (size_t) nw * 8);
	agk_step(res, "and_w");
	memcpy(w2, c->mask, (size_t) nw * 8);
	k->or_w(w2, c->mask2, nw);
	agk_hash(res, w2, (size_t) nw * 8);
	agk_step(res, "or_w");
	memcpy(w2, c->mask, (size_t) nw * 8);
	k->andnot_w(w2, c->mask2, nw);
	agk_hash(res, w2, (size_t) nw * 8);
	agk_step(res, "andnot_w");
	AGK_HV(res, k->popcount_w(c->mask, nw));
	AGK_HV(res, k->popcount_w(c->mask2, nw));
	agk_step(res, "popcount_w");
	BM("gather_bits", k->gather_bits(bm, c->rowmask, c->parent, n));

	for (int m = 0; m < 2; m++)
	{
		const uint64 *mm = m ? c->mask2 : c->mask;
		int			cnt;

		memset(sel, 0xA5, (size_t) (64 * nw + SIFT_AGG_PAD / 4) * 4);
		cnt = k->mask_sel(sel, mm, nw);
		AGK_HV(res, cnt);
		agk_hash(res, sel, (size_t) cnt * 4);
	}
	agk_step(res, "mask_sel");

	/* gathers from the scalar selection vector (so that they are checked on their own), and from a random one */
	nssel = sift_aggkern_scalar.mask_sel(ssel, c->mask, nw);
	for (int s = 0; s < 2; s++)
	{
		const uint32 *idx = s ? c->rsel : ssel;
		const int	nidx = s ? c->nrs : nssel;
		uint32	   *d32 = AGK_ALLOC(nidx > 0 ? (size_t) nidx * 4 : 4);
		uint64	   *d64 = AGK_ALLOC(nidx > 0 ? (size_t) nidx * 8 : 8);

		memset(d32, 0xA5, (size_t) nidx * 4);
		k->gather_u32(d32, c->code, idx, nidx);
		agk_hash(res, d32, (size_t) nidx * 4);
		memset(d64, 0xA5, (size_t) nidx * 8);
		k->gather_u64(d64, (const uint64 *) c->i64, idx, nidx);
		agk_hash(res, d64, (size_t) nidx * 8);
		AGK_FREE(d32);
		AGK_FREE(d64);
	}
	agk_step(res, "gather");

	memcpy(g, c->gid0, (size_t) n * 4);
	k->gid_code(g, c->code, c->map, n, c->mult);
	agk_hash(res, g, (size_t) n * 4);
	agk_step(res, "gid_code");
	memcpy(g, c->gid0, (size_t) n * 4);
	k->gid_i32(g, c->k32, c->kbase, c->map2, n, c->mult);
	agk_hash(res, g, (size_t) n * 4);
	agk_step(res, "gid_i32");

	for (int m = 0; m < 2; m++)
	{
		const uint64 *mm = masks[m];
		int32		mn32 = 0x5A5A5A5A,
					mx32 = 0x5A5A5A5A;
		int64		mn64 = 0x5A5A5A5A5A5A5A5A,
					mx64 = 0x5A5A5A5A5A5A5A5A;
		double		mnf = 1.5,
					mxf = -2.5;
		bool		ok;

		AGK_HV(res, k->sum_i32(c->i32, mm, n));
		agk_step(res, "sum_i32");
		AGK_HV(res, k->sum_f64(c->f64, mm, n));
		agk_step(res, "sum_f64");
		ok = k->minmax_i32(&mn32, &mx32, c->i32, mm, n);
		AGK_HV(res, ok);
		AGK_HV(res, mn32);
		AGK_HV(res, mx32);
		agk_step(res, "minmax_i32");
		ok = k->minmax_i64(&mn64, &mx64, c->i64, mm, n);
		AGK_HV(res, ok);
		AGK_HV(res, mn64);
		AGK_HV(res, mx64);
		agk_step(res, "minmax_i64");
		ok = k->minmax_f64(&mnf, &mxf, c->f64, mm, n);
		AGK_HV(res, ok);
		AGK_HV(res, mnf);
		AGK_HV(res, mxf);
		agk_step(res, "minmax_f64");
	}

	for (int m = 0; m < 2; m++)
	{
		const uint64 *mm = masks[m];
		int64	   *cnt = AGK_ALLOC((size_t) c->ngrp * 8),
				   *iacc = AGK_ALLOC((size_t) c->ngrp * 8);
		double	   *facc = AGK_ALLOC((size_t) c->ngrp * 8);

		memcpy(cnt, c->cnt0, (size_t) c->ngrp * 8);
		k->count_gid(cnt, c->gid, mm, n);
		agk_hash(res, cnt, (size_t) c->ngrp * 8);
		agk_step(res, "count_gid");
		memcpy(iacc, c->iacc0, (size_t) c->ngrp * 8);
		k->sum_gid_i32(iacc, c->gid, c->i32, mm, n);
		agk_hash(res, iacc, (size_t) c->ngrp * 8);
		agk_step(res, "sum_gid_i32");
		memcpy(facc, c->facc0, (size_t) c->ngrp * 8);
		k->sum_gid_f64(facc, c->gid, c->f64, mm, n);
		agk_hash(res, facc, (size_t) c->ngrp * 8);
		agk_step(res, "sum_gid_f64");
		AGK_FREE(cnt);
		AGK_FREE(iacc);
		AGK_FREE(facc);

		{
			uint32		prev = c->prev0;

			AGK_HV(res, k->count_changes(c->code, mm, n, &prev));
			AGK_HV(res, prev);
			AGK_HV(res, k->count_changes(c->code, c->mask2, n, &prev));
			AGK_HV(res, prev);
			agk_step(res, "count_changes");
		}
	}
#undef BM
	AGK_FREE(bm);
	AGK_FREE(w2);
	AGK_FREE(sel);
	AGK_FREE(ssel);
	AGK_FREE(g);
}

#endif							/* SIFT_AGGKERN_CASES_H */
