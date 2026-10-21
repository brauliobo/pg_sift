/*
 * col/wide.c - the values of the wide leaf types (col/wide.h, docs/COL-LEAFTYPES-32.md). Portable: no allocation, no host type.
 */
#include "sift_host.h"

#include "col/wide.h"

#define W128_MAXPOS	(((sift_wu128) 1 << 127) - 1)
#define W128_MAXNEG	((sift_wu128) 1 << 127)		/* |-2^127| */
#define EXP_BIAS	(1 << 23)

/* ---------------------------------------------------------------------------
 * 1. 128-bit integers and decimal text
 * ------------------------------------------------------------------------- */

/* u = u * 10 + d; true when it left 128 bits */
static inline bool
mul10_add(sift_wu128 *u, unsigned d)
{
	if (*u > ((sift_wu128) -1 - d) / 10)
		return true;
	*u = *u * 10 + d;
	return false;
}

SiftW128Status
sift_col_decimal_to_w128(const char *txt, int scale, sift_w128 *out)
{
	bool		neg = false,
				over = false,
				inexact = false,
				any = false,
				dot = false;
	sift_wu128	u = 0;
	int			frac = 0;
	const char *p = txt;

	if (*p == '-' || *p == '+')
		neg = *p++ == '-';
	for (; *p; p++)
	{
		unsigned	d;

		if (*p == '.')
		{
			if (dot)
				return SIFT_W128_BAD;
			dot = true;
			continue;
		}
		if (*p < '0' || *p > '9')
			return SIFT_W128_BAD;
		any = true;
		d = (unsigned) (*p - '0');
		if (dot && frac >= scale)
		{
			inexact |= d != 0;		/* a digit past the scale must be a zero */
			continue;
		}
		frac += dot;
		over = over || mul10_add(&u, d);
	}
	if (!any)
		return SIFT_W128_BAD;
	if (inexact)
		return SIFT_W128_INEXACT;
	for (; frac < scale && !over; frac++)
		over = mul10_add(&u, 0);
	if (over || u > (neg ? W128_MAXNEG : W128_MAXPOS))
		return neg ? SIFT_W128_BELOW : SIFT_W128_ABOVE;
	*out = neg ? (sift_w128) (0 - u) : (sift_w128) u;
	return SIFT_W128_OK;
}

int
sift_col_w128_to_decimal(sift_w128 v, int scale, char *buf)
{
	sift_wu128	u = v < 0 ? 0 - (sift_wu128) v : (sift_wu128) v;
	char		digits[SIFT_COL_W128_TEXT];
	int			nd = 0,
				len = 0;

	do
	{
		digits[nd++] = (char) ('0' + (int) (u % 10));
		u /= 10;
	} while (u != 0);
	while (nd <= scale)
		digits[nd++] = '0';
	if (v < 0)
		buf[len++] = '-';
	for (int i = nd - 1; i >= 0; i--)
	{
		buf[len++] = digits[i];
		if (i == scale && scale > 0)
			buf[len++] = '.';
	}
	buf[len] = '\0';
	return len;
}

/* ---------------------------------------------------------------------------
 * 3. The canonical image of a decimal
 * ------------------------------------------------------------------------- */

static inline bool
is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static uint32
img_special(SiftNumKind kind, uint8 *out)
{
	out[0] = (uint8) kind;
	return 1;
}

uint32
sift_col_numimg_from_text(const char *txt, uint8 *out, int32 *dscale)
{
	bool		neg;
	const char *ip,
			   *fp;
	size_t		ni = 0,
				nf = 0,
				lead,
				last,
				nd,
				n = 0;
	int64		exp10;
	uint8		inv;

	*dscale = 0;
	if (strcmp(txt, "NaN") == 0)
		return img_special(SIFT_NIMG_NAN, out);
	if (strcmp(txt, "Infinity") == 0)
		return img_special(SIFT_NIMG_PINF, out);
	if (strcmp(txt, "-Infinity") == 0)
		return img_special(SIFT_NIMG_NINF, out);
	neg = *txt == '-';
	ip = txt + neg;
	while (is_digit(ip[ni]))
		ni++;
	fp = ip + ni;
	if (*fp == '.')
	{
		fp++;
		while (is_digit(fp[nf]))
			nf++;
		if (nf == 0)
			return 0;
	}
	if (ni == 0 || fp[nf] != '\0' || nf > SIFT_NIMG_DSCALE_MAX)
		return 0;
	*dscale = (int32) nf;
	/* digit i of the number: the integer digits, then the fraction digits */
#define DIG(i)	((size_t) (i) < ni ? ip[i] : fp[(size_t) (i) - ni])
	for (lead = 0; lead < ni + nf && DIG(lead) == '0'; lead++)
		;
	if (lead == ni + nf)
		return img_special(SIFT_NIMG_ZERO, out);
	for (last = ni + nf - 1; DIG(last) == '0'; last--)
		;
	nd = last - lead + 1;
	exp10 = (int64) ni - (int64) lead;
	if (exp10 > SIFT_NIMG_EXP_MAX || exp10 < SIFT_NIMG_EXP_MIN)
		return 0;
	inv = neg ? 0xFF : 0;
	out[n++] = (uint8) (neg ? SIFT_NIMG_NEG : SIFT_NIMG_POS);
	out[n++] = (uint8) (((uint32) (exp10 + EXP_BIAS) >> 16) ^ inv);
	out[n++] = (uint8) (((uint32) (exp10 + EXP_BIAS) >> 8) ^ inv);
	out[n++] = (uint8) ((uint32) (exp10 + EXP_BIAS) ^ inv);
	for (size_t i = 0; i < nd; i += 2)
	{
		unsigned	pair = (unsigned) (DIG(lead + i) - '0') * 10 + (i + 1 < nd ? (unsigned) (DIG(lead + i + 1) - '0') : 0);

		out[n++] = (uint8) ((pair + 1) ^ inv);
	}
#undef DIG
	if (neg)
		out[n++] = 0xFF;
	return (uint32) n;
}

/* What an image says: its kind and, for a finite non-zero number, the exponent, the pair bytes (img + 4, np of them, complemented when negative) and the digit count */
typedef struct ImgInfo
{
	SiftNumKind kind;
	int32		exp10;
	uint32		np;
	uint32		nd;
	uint8		inv;
} ImgInfo;

static bool
img_parse(const uint8 *img, uint32 len, ImgInfo *in)
{
	uint32		end = len;
	unsigned	last;

	memset(in, 0, sizeof(*in));
	if (len == 0)
		return false;
	in->kind = (SiftNumKind) img[0];
	switch (in->kind)
	{
		case SIFT_NIMG_NINF:
		case SIFT_NIMG_ZERO:
		case SIFT_NIMG_PINF:
		case SIFT_NIMG_NAN:
			return len == 1;
		case SIFT_NIMG_NEG:
		case SIFT_NIMG_POS:
			break;
		default:
			return false;
	}
	in->inv = in->kind == SIFT_NIMG_NEG ? 0xFF : 0;
	if (len < 5 || (in->kind == SIFT_NIMG_NEG && img[len - 1] != 0xFF))
		return false;
	if (in->kind == SIFT_NIMG_NEG)
		end--;
	if (end < 5)
		return false;
	in->exp10 = (int32) ((uint32) (img[1] ^ in->inv) << 16 | (uint32) (img[2] ^ in->inv) << 8 | (uint32) (img[3] ^ in->inv)) - EXP_BIAS;
	if (in->exp10 > SIFT_NIMG_EXP_MAX || in->exp10 < SIFT_NIMG_EXP_MIN)
		return false;
	in->np = end - 4;
	for (uint32 i = 4; i < end; i++)
	{
		unsigned	pair = (unsigned) (img[i] ^ in->inv);

		if (pair < 1 || pair > 100)
			return false;
	}
	if ((unsigned) ((img[4] ^ in->inv) - 1) < 10)
		return false;			/* the first digit is not zero */
	last = (unsigned) ((img[end - 1] ^ in->inv) - 1);
	if (last == 0)
		return false;			/* the last digit is not zero (a zero low digit is the padding of an odd count) */
	in->nd = 2 * in->np - (last % 10 == 0);
	return true;
}

bool
sift_col_numimg_valid(const uint8 *img, uint32 len)
{
	ImgInfo		in;

	return img_parse(img, len, &in);
}

/* digit i (0 = the first) of the mantissa, '0' outside it */
static inline char
img_digit(const uint8 *img, const ImgInfo *in, int64 i)
{
	unsigned	pair;

	if (i < 0 || i >= in->nd)
		return '0';
	pair = (unsigned) ((img[4 + i / 2] ^ in->inv) - 1);
	return (char) ('0' + (i % 2 == 0 ? pair / 10 : pair % 10));
}

uint32
sift_col_numimg_to_text(const uint8 *img, uint32 len, int32 dscale, char *buf, uint32 cap)
{
	ImgInfo		in;
	uint64		need;
	uint32		n = 0;

	if (dscale < 0 || dscale > SIFT_NIMG_DSCALE_MAX || !img_parse(img, len, &in))
		return 0;
	switch (in.kind)
	{
		case SIFT_NIMG_NAN:
			return cap >= 4 ? (memcpy(buf, "NaN", 4), 3) : 0;
		case SIFT_NIMG_PINF:
			return cap >= 9 ? (memcpy(buf, "Infinity", 9), 8) : 0;
		case SIFT_NIMG_NINF:
			return cap >= 10 ? (memcpy(buf, "-Infinity", 10), 9) : 0;
		default:
			break;
	}
	need = (in.kind == SIFT_NIMG_NEG) + (in.kind != SIFT_NIMG_ZERO && in.exp10 > 0 ? (uint64) in.exp10 : 1) + (dscale > 0 ? 1 + (uint64) dscale : 0) + 1;
	if (need > cap)
		return 0;
	if (in.kind == SIFT_NIMG_NEG)
		buf[n++] = '-';
	if (in.kind == SIFT_NIMG_ZERO || in.exp10 <= 0)
		buf[n++] = '0';
	else
		for (int32 i = 0; i < in.exp10; i++)
			buf[n++] = img_digit(img, &in, i);
	if (dscale > 0)
	{
		buf[n++] = '.';
		for (int32 f = 0; f < dscale; f++)
			buf[n++] = in.kind == SIFT_NIMG_ZERO ? '0' : img_digit(img, &in, (int64) in.exp10 + f);
	}
	buf[n] = '\0';
	return n;
}
