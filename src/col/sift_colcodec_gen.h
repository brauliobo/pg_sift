/*
 * sift_colcodec_gen.h - the test chunks of the codec (typical and adversarial shapes of key columns), shared by the in-backend regress test
 * (sift_colcodec_test.c) and the standalone harness (tools/colcodec/test.c). Test code only; it needs no backend.
 *
 * The includer defines, before including this file, `CcgRng` (the generator state) and `static inline uint64 ccg_next(CcgRng *r)` (the next
 * 64 random bits).
 */
#ifndef SIFT_COLCODEC_GEN_H
#define SIFT_COLCODEC_GEN_H

#include <math.h>

#include "sift_colcodec.h"

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

/* Fills v[0..n) with generator g (CCG_*); *kind is the chunk kind */
static void
ccg_fill(CcgRng *r, int g, uint32 n, int64 *v, SiftCcKind *kind)
{
	static const double divs[] = {10, 100, 1000, 10000};
	int64		base = ccg_bits(r, 40) - ((int64) 1 << 39),
				step = (int64) ccg_below(r, 1000) - 100;
	int			W = 1 + ccg_below(r, 32);
	uint32		k = 0;

	*kind = SIFT_CC_INT;
	switch (g)
	{
		case CCG_CONST:
			for (uint32 i = 0; i < n; i++)
				v[i] = base;
			break;
		case CCG_RAMP:
			for (uint32 i = 0; i < n; i++)
				v[i] = base + (int64) i * step;
			break;
		case CCG_ARITH:
			for (uint32 i = 0; i < n; i++)
				v[i] = base + (int64) i * 86400000000;
			break;
		case CCG_BITS:
			for (uint32 i = 0; i < n; i++)
				v[i] = base + (int64) (ccg_next(r) >> (64 - W));
			break;
		case CCG_FULL64:
			for (uint32 i = 0; i < n; i++)
				v[i] = (int64) ccg_next(r);
			break;
		case CCG_EXTREME:
			for (uint32 i = 0; i < n; i++)
				v[i] = ccg_below(r, 3) ? (ccg_below(r, 2) ? INT64_MIN : INT64_MAX) : (int64) ccg_below(r, 5) - 2;
			break;
		case CCG_RUNS:
			while (k < n)
			{
				uint32		len = 1 + ccg_below(r, ccg_below(r, 2) ? 3 : 400);
				int64		x = ccg_bits(r, 1 + ccg_below(r, 40));

				for (; len && k < n; len--)
					v[k++] = x;
			}
			break;
		case CCG_LOWCARD:
			{
				int64		pool[300];
				uint32		d = 2 + ccg_below(r, ccg_below(r, 2) ? 8 : 290);

				for (uint32 i = 0; i < d; i++)
					pool[i] = ccg_bits(r, 50);
				for (uint32 i = 0; i < n; i++)
					v[i] = pool[ccg_below(r, d)];
				break;
			}
		case CCG_SORTED:
			{
				int64		x = base;

				for (uint32 i = 0; i < n; i++)
					v[i] = x += ccg_below(r, 1 + (1u << ccg_below(r, 20)));
				break;
			}
		case CCG_OUTLIER:
			for (uint32 i = 0; i < n; i++)
				v[i] = ccg_below(r, 200) ? (int64) ccg_below(r, 100) : ccg_bits(r, 62);
			break;
		case CCG_DECIMAL:
		case CCG_DECIMAL_EXC:
			{
				double		div = divs[ccg_below(r, 4)];

				*kind = SIFT_CC_F8;
				for (uint32 i = 0; i < n; i++)
				{
					double		x = (double) ((int64) ccg_below(r, 2000000) - 1000000) / div;

					if (g == CCG_DECIMAL_EXC && ccg_below(r, 30) == 0)
					{
						double		odd[] = {NAN, INFINITY, -INFINITY, -0.0, 5e-324, 1.7976931348623157e308, 0.1 + 0.2, 1e300, 3.14159265358979323846};

						x = odd[ccg_below(r, 9)];
					}
					v[i] = sift_col_f8_key(x);
				}
				break;
			}
		case CCG_FLOAT4:
			*kind = SIFT_CC_F4;
			for (uint32 i = 0; i < n; i++)
			{
				float		x = (float) ((int64) ccg_below(r, 100000) - 50000) / 100.0f;

				if (ccg_below(r, 40) == 0)
					x = ccg_below(r, 2) ? -0.0f : (float) ccg_unit(r);
				v[i] = sift_col_f8_key((double) x);
			}
			break;
		case CCG_RANDOM_F8:
			*kind = SIFT_CC_F8;
			for (uint32 i = 0; i < n; i++)
				v[i] = sift_col_f8_key((ccg_unit(r) - 0.5) * 1e6);
			break;
		case CCG_NEGATIVE:
			for (uint32 i = 0; i < n; i++)
				v[i] = -(int64) ccg_below(r, 1u << 20) - 1;
			break;
		case CCG_EDGE32:
			{
				int64		span = ccg_below(r, 2) ? 0xFFFFFFFFLL : 0x100000000LL;

				for (uint32 i = 0; i < n; i++)
					v[i] = base + (ccg_below(r, 2) ? 0 : span * (int64) ccg_below(r, 2));
				break;
			}
		case CCG_MICROS:
			for (uint32 i = 0; i < n; i++)
				v[i] = base + (int64) ccg_below(r, 1u << 27) * 1000000;
			break;
		case CCG_MICROS_SORTED:
			{
				int64		x = base;

				for (uint32 i = 0; i < n; i++)
					v[i] = x += (int64) ccg_below(r, 3000) * 1000000;
				break;
			}
		case CCG_STEP40:
			for (uint32 i = 0; i < n; i++)
				v[i] = INT64_MIN + (int64) ccg_below(r, 1u << 23) * ((int64) 1 << 40);
			break;
		case CCG_JITTER:
			for (uint32 i = 0; i < n; i++)
				v[i] = base + (int64) i * 1000 + (int64) ccg_below(r, 900);
			break;
		case CCG_ALT:			/* a flag alternating with rare repeats, then one long run: run values and lengths are runs themselves */
			for (uint32 i = 0, head = n / 4 + ccg_below(r, n / 2 + 1); i < n; i++)
				v[i] = !i ? 0 : i >= head || !ccg_below(r, 16) ? v[i - 1] : 1 - v[i - 1];
			break;
	}
}

#endif							/* SIFT_COLCODEC_GEN_H */
