/*
 * col/wide.h - the values of the leaf types that are not one machine word: the lane keys of a 128-bit
 * integer, the canonical total of an interval, the canonical image of a decimal of any precision. Portable: the C library and core/host.h
 * only, no allocation (the caller passes every buffer), no host type. __int128 is what col/preagg.h and agg/spec.h already need.
 */
#ifndef SIFT_COL_WIDE_H
#define SIFT_COL_WIDE_H

#include "core/host.h"

typedef __int128 sift_w128 __attribute__((aligned(8)));
typedef unsigned __int128 sift_wu128 __attribute__((aligned(8)));

#define SIFT_COL_USECS_PER_DAY	INT64CONST(86400000000)
#define SIFT_COL_SIGN64			UINT64CONST(0x8000000000000000)
#define SIFT_COL_MAX_WIDE_SCALE	38			/* numeric(p <= 38, s <= 38): the scaled value fits a signed 128-bit integer */

/* ---------------------------------------------------------------------------
 * 1. 128-bit integers as two lanes
 * ------------------------------------------------------------------------- */

/* Lane 0 is the high 64 bits, signed; lane 1 the low 64 bits with the sign bit flipped: the signed order of (lane 0, lane 1) is the order of the integer */
static inline int64
sift_col_wide_hi(sift_w128 v)
{
	return (int64) (v >> 64);
}

static inline int64
sift_col_wide_lo(sift_w128 v)
{
	return (int64) ((uint64) v ^ SIFT_COL_SIGN64);
}

static inline sift_w128
sift_col_wide_join(int64 hi, int64 lo)
{
	return (sift_w128) (((sift_wu128) (uint64) hi << 64) | ((uint64) lo ^ SIFT_COL_SIGN64));
}

/* The order of two pairs of lane keys, as the integers they stand for: -1, 0 or 1 */
static inline int
sift_col_wide_cmp(int64 hi1, int64 lo1, int64 hi2, int64 lo2)
{
	return hi1 != hi2 ? (hi1 < hi2 ? -1 : 1) : (lo1 < lo2 ? -1 : lo1 > lo2);
}

typedef enum SiftW128Status
{
	SIFT_W128_OK = 0,
	SIFT_W128_ABOVE = 1,		/* greater than every 128-bit integer */
	SIFT_W128_BELOW = -1,		/* less than every 128-bit integer */
	SIFT_W128_INEXACT = 2,		/* a fraction digit other than 0 past the scale: not an integer after the shift */
	SIFT_W128_BAD = 3			/* not "[+-]digits[.digits]" */
} SiftW128Status;

/*
 * The integer of the decimal text "[-+]digits[.digits]" times 10^scale (0 <= scale <= SIFT_COL_MAX_WIDE_SCALE), exactly: the text may have fewer
 * fraction digits than the scale (zeros are appended) and more only if they are all zeros. *out is set only for SIFT_W128_OK. numeric_out's
 * text is always well formed; anything else is SIFT_W128_BAD.
 */
extern SiftW128Status sift_col_decimal_to_w128(const char *txt, int scale, sift_w128 *out);

/* v / 10^scale as decimal text with exactly `scale` fraction digits ("-" for a negative, "0.05" for 5 at scale 2), NUL terminated; returns the length */
#define SIFT_COL_W128_TEXT		48
extern int	sift_col_w128_to_decimal(sift_w128 v, int scale, char *buf);

/* ---------------------------------------------------------------------------
 * 2. interval: PostgreSQL compares, hashes and groups on one number (a month is 30 days, a day is 24 hours)
 * ------------------------------------------------------------------------- */

/* The canonical total of an interval {time, day, month} in microseconds: the value PostgreSQL's interval_cmp_value computes */
static inline sift_w128
sift_col_interval_total(int64 time, int32 day, int32 month)
{
	return (sift_w128) month * 30 * SIFT_COL_USECS_PER_DAY + (sift_w128) day * SIFT_COL_USECS_PER_DAY + time;
}

/* The payload lane of an interval: its month and day, which the total does not tell apart */
static inline int64
sift_col_interval_md(int32 day, int32 month)
{
	return (int64) ((uint64) (uint32) month << 32 | (uint32) day);
}

/* The triple back from the total and the payload lane (the time is what is left of the total; it fits int64 for a total of a real interval) */
static inline void
sift_col_interval_raw(sift_w128 total, int64 md, int64 *time, int32 *day, int32 *month)
{
	int32		m = (int32) (md >> 32),
				d = (int32) (uint32) md;

	*month = m;
	*day = d;
	*time = (int64) ((uint64) total - (uint64) (int64) m * 30 * (uint64) SIFT_COL_USECS_PER_DAY - (uint64) (int64) d * (uint64) SIFT_COL_USECS_PER_DAY);
}

/* ---------------------------------------------------------------------------
 * 3. The canonical image of a decimal of any precision
 * ------------------------------------------------------------------------- */

/*
 * The image of a number is a byte string whose bytewise order (memcmp, then length) is the numeric order of PostgreSQL (-Infinity < negative <
 * 0 < positive < Infinity < NaN) and whose bytewise equality is the equality of numbers: 1.0 and 1.00 have one image, NaN is equal to NaN. The
 * display scale is not part of it. Layout: a tag byte; for a finite non-zero number the decimal exponent e (value = 0.DIGITS x 10^e, DIGITS
 * without a leading or trailing zero) as 3 bytes big endian offset by 2^23, then the digits two at a time as the bytes 1..100 (an odd last
 * digit is padded with 0); a negative number has every byte after the tag complemented and a terminating 0xFF.
 */
typedef enum SiftNumKind
{
	SIFT_NIMG_NINF = 0x10,
	SIFT_NIMG_NEG = 0x20,
	SIFT_NIMG_ZERO = 0x30,
	SIFT_NIMG_POS = 0x40,
	SIFT_NIMG_PINF = 0x50,
	SIFT_NIMG_NAN = 0x60
} SiftNumKind;

#define SIFT_NIMG_EXP_MAX		131072		/* digits before the point of a PostgreSQL numeric */
#define SIFT_NIMG_EXP_MIN		(-32768)
#define SIFT_NIMG_DSCALE_MAX	16383		/* NUMERIC_DSCALE_MAX */
#define SIFT_NIMG_MAXLEN(ndigits)	(1 + 3 + ((ndigits) + 1) / 2 + 1)
/* room for the text of any valid image with a valid display scale */
#define SIFT_NIMG_TEXT_MAX		(1 + SIFT_NIMG_EXP_MAX + 1 + SIFT_NIMG_DSCALE_MAX + 1)

/*
 * The image of the text numeric_out gives ("[-]digits[.digits]", NaN, Infinity, -Infinity); *dscale is its number of fraction digits. out has
 * SIFT_NIMG_MAXLEN(strlen(txt)) bytes. Returns the length, 0 when txt is not such a text.
 */
extern uint32 sift_col_numimg_from_text(const char *txt, uint8 *out, int32 *dscale);

/*
 * The text of an image with `dscale` fraction digits (as numeric_out prints it), NUL terminated, into buf of cap bytes. Returns its length, 0
 * when the bytes are not a canonical image (decoding then re-encoding must give the same bytes), dscale is out of range or the text does not fit.
 */
extern uint32 sift_col_numimg_to_text(const uint8 *img, uint32 len, int32 dscale, char *buf, uint32 cap);

/* Is img a canonical image (the one thing sift.col_verify and the readers need) */
extern bool sift_col_numimg_valid(const uint8 *img, uint32 len);

#endif							/* SIFT_COL_WIDE_H */
