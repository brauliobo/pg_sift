/*
 * col/codec_gen.h - the test chunks of the codec (typical and adversarial shapes of key columns), shared by the in-backend regress test
 * (col/codec_test.c) and the standalone harness (tools/colcodec/test.c). Test code only; it needs no backend.
 *
 * The includer defines, before including this file, `CcgRng` (the generator state) and `static inline uint64 ccg_next(CcgRng *r)` (the next
 * 64 random bits).
 */
#ifndef SIFT_COLCODEC_GEN_H
#define SIFT_COLCODEC_GEN_H

#include <math.h>

#include "col/codec.h"

enum
{
	CCG_CONST, CCG_RAMP, CCG_BITS, CCG_FULL64, CCG_EXTREME, CCG_RUNS, CCG_LOWCARD, CCG_SORTED, CCG_OUTLIER, CCG_DECIMAL, CCG_DECIMAL_EXC,
	CCG_FLOAT4, CCG_RANDOM_F8, CCG_NEGATIVE, CCG_EDGE32, CCG_JITTER, CCG_ARITH, CCG_MICROS, CCG_MICROS_SORTED, CCG_STEP40, CCG_ALT, CCG_NGEN
};

static inline uint32
ccg_below(CcgRng *r, uint32 n)
{
	return (uint32) (((ccg_next(r) >> 32) * n) >> 32);
}

/* [0, 1) */
static inline double
ccg_unit(CcgRng *r)
{
	return (ccg_next(r) >> 11) * 0x1p-53;
}

/* A signed value of up to `bits` bits */
static inline int64
ccg_bits(CcgRng *r, int bits)
{
	return bits >= 64 ? (int64) ccg_next(r) : (int64) (ccg_next(r) >> (64 - bits));
}

/* The draws every generator shares: a base value, a step and a bit width */
typedef struct CcgShape
{
	int64		base;
	int64		step;
	int			W;
	SiftCcKind	kind;
} CcgShape;

static void
ccg_const(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	for (uint32 i = 0; i < n; i++)
		v[i] = s->base;
}

static void
ccg_ramp(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	for (uint32 i = 0; i < n; i++)
		v[i] = s->base + (int64) i * s->step;
}

static void
ccg_arith(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	for (uint32 i = 0; i < n; i++)
		v[i] = s->base + (int64) i * 86400000000;
}

static void
ccg_wide(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	for (uint32 i = 0; i < n; i++)
		v[i] = s->base + (int64) (ccg_next(r) >> (64 - s->W));
}

static void
ccg_full64(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	for (uint32 i = 0; i < n; i++)
		v[i] = (int64) ccg_next(r);
}

static void
ccg_extreme(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	for (uint32 i = 0; i < n; i++)
		v[i] = ccg_below(r, 3) ? (ccg_below(r, 2) ? INT64_MIN : INT64_MAX) : (int64) ccg_below(r, 5) - 2;
}

static void
ccg_runs(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	for (uint32 k = 0; k < n;)
	{
		uint32		len = 1 + ccg_below(r, ccg_below(r, 2) ? 3 : 400);
		int64		x = ccg_bits(r, 1 + ccg_below(r, 40));

		for (; len && k < n; len--)
			v[k++] = x;
	}
}

static void
ccg_lowcard(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	int64		pool[300];
	uint32		d = 2 + ccg_below(r, ccg_below(r, 2) ? 8 : 290);

	for (uint32 i = 0; i < d; i++)
		pool[i] = ccg_bits(r, 50);
	for (uint32 i = 0; i < n; i++)
		v[i] = pool[ccg_below(r, d)];
}

static void
ccg_sorted(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	int64		x = s->base;

	for (uint32 i = 0; i < n; i++)
		v[i] = x += ccg_below(r, 1 + (1u << ccg_below(r, 20)));
}

static void
ccg_outlier(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	for (uint32 i = 0; i < n; i++)
		v[i] = ccg_below(r, 200) ? (int64) ccg_below(r, 100) : ccg_bits(r, 62);
}

/* Decimals of up to four places; with `odd`, now and then a double that does not survive a scaled integer */
static void
ccg_decimal(CcgRng *r, CcgShape *s, uint32 n, int64 *v, bool odd)
{
	static const double divs[] = {10, 100, 1000, 10000};
	double		div = divs[ccg_below(r, 4)];

	s->kind = SIFT_CC_F8;
	for (uint32 i = 0; i < n; i++)
	{
		double		x = (double) ((int64) ccg_below(r, 2000000) - 1000000) / div;

		if (odd && ccg_below(r, 30) == 0)
		{
			double		special[] = {NAN, INFINITY, -INFINITY, -0.0, 5e-324, 1.7976931348623157e308, 0.1 + 0.2, 1e300, 3.14159265358979323846};

			x = special[ccg_below(r, 9)];
		}
		v[i] = sift_col_f8_key(x);
	}
}

static void
ccg_float4(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	s->kind = SIFT_CC_F4;
	for (uint32 i = 0; i < n; i++)
	{
		float		x = (float) ((int64) ccg_below(r, 100000) - 50000) / 100.0f;

		if (ccg_below(r, 40) == 0)
			x = ccg_below(r, 2) ? -0.0f : (float) ccg_unit(r);
		v[i] = sift_col_f8_key((double) x);
	}
}

static void
ccg_random_f8(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	s->kind = SIFT_CC_F8;
	for (uint32 i = 0; i < n; i++)
		v[i] = sift_col_f8_key((ccg_unit(r) - 0.5) * 1e6);
}

static void
ccg_negative(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	for (uint32 i = 0; i < n; i++)
		v[i] = -(int64) ccg_below(r, 1u << 20) - 1;
}

static void
ccg_edge32(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	int64		span = ccg_below(r, 2) ? 0xFFFFFFFFLL : 0x100000000LL;

	for (uint32 i = 0; i < n; i++)
		v[i] = s->base + (ccg_below(r, 2) ? 0 : span * (int64) ccg_below(r, 2));
}

static void
ccg_micros(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	for (uint32 i = 0; i < n; i++)
		v[i] = s->base + (int64) ccg_below(r, 1u << 27) * 1000000;
}

static void
ccg_micros_sorted(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	int64		x = s->base;

	for (uint32 i = 0; i < n; i++)
		v[i] = x += (int64) ccg_below(r, 3000) * 1000000;
}

static void
ccg_step40(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	for (uint32 i = 0; i < n; i++)
		v[i] = INT64_MIN + (int64) ccg_below(r, 1u << 23) * ((int64) 1 << 40);
}

static void
ccg_jitter(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	for (uint32 i = 0; i < n; i++)
		v[i] = s->base + (int64) i * 1000 + (int64) ccg_below(r, 900);
}

/* A flag alternating with rare repeats, then one long run: run values and lengths are runs themselves */
static void
ccg_alt(CcgRng *r, CcgShape *s, uint32 n, int64 *v)
{
	for (uint32 i = 0, head = n / 4 + ccg_below(r, n / 2 + 1); i < n; i++)
		v[i] = !i ? 0 : i >= head || !ccg_below(r, 16) ? v[i - 1] : 1 - v[i - 1];
}

/* Fills v[0..n) with generator g (CCG_*); *kind is the chunk kind */
static void
ccg_fill(CcgRng *r, int g, uint32 n, int64 *v, SiftCcKind *kind)
{
	CcgShape	s;

	s.base = ccg_bits(r, 40) - ((int64) 1 << 39);
	s.step = (int64) ccg_below(r, 1000) - 100;
	s.W = 1 + ccg_below(r, 32);
	s.kind = SIFT_CC_INT;
	switch (g)
	{
		case CCG_CONST:			ccg_const(r, &s, n, v); break;
		case CCG_RAMP:			ccg_ramp(r, &s, n, v); break;
		case CCG_ARITH:			ccg_arith(r, &s, n, v); break;
		case CCG_BITS:			ccg_wide(r, &s, n, v); break;
		case CCG_FULL64:		ccg_full64(r, &s, n, v); break;
		case CCG_EXTREME:		ccg_extreme(r, &s, n, v); break;
		case CCG_RUNS:			ccg_runs(r, &s, n, v); break;
		case CCG_LOWCARD:		ccg_lowcard(r, &s, n, v); break;
		case CCG_SORTED:		ccg_sorted(r, &s, n, v); break;
		case CCG_OUTLIER:		ccg_outlier(r, &s, n, v); break;
		case CCG_DECIMAL:		ccg_decimal(r, &s, n, v, false); break;
		case CCG_DECIMAL_EXC:	ccg_decimal(r, &s, n, v, true); break;
		case CCG_FLOAT4:		ccg_float4(r, &s, n, v); break;
		case CCG_RANDOM_F8:		ccg_random_f8(r, &s, n, v); break;
		case CCG_NEGATIVE:		ccg_negative(r, &s, n, v); break;
		case CCG_EDGE32:		ccg_edge32(r, &s, n, v); break;
		case CCG_MICROS:		ccg_micros(r, &s, n, v); break;
		case CCG_MICROS_SORTED:	ccg_micros_sorted(r, &s, n, v); break;
		case CCG_STEP40:		ccg_step40(r, &s, n, v); break;
		case CCG_JITTER:		ccg_jitter(r, &s, n, v); break;
		case CCG_ALT:			ccg_alt(r, &s, n, v); break;
	}
	*kind = s.kind;
}

#endif							/* SIFT_COLCODEC_GEN_H */
