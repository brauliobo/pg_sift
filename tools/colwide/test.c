/*
 * Tests of src/col/wide.c, the portable value code of the wide leaf types: the 128-bit lane keys and
 * their decimal text, the interval total, the canonical image of a decimal. A reference compares decimal texts as digit strings (no
 * integer type involved), so the image order is checked against something that shares no code with the encoder.
 *   make -C tools/colwide [SANITIZE=1] test [N=1]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "col/wide.h"

static uint64 rng_s[2] = {0x9E3779B97F4A7C15ULL, 0xD1B54A32D192ED03ULL};
static long fails, checks;

static uint64
rnd(void)
{
	uint64		a = rng_s[0],
				b = rng_s[1];

	rng_s[0] = b;
	a ^= a << 23;
	rng_s[1] = a ^ b ^ (a >> 18) ^ (b >> 5);
	return rng_s[1] + b;
}

#define CHECK(cond, ...) \
	do { \
		checks++; \
		if (!(cond)) \
		{ \
			fails++; \
			if (fails < 30) \
			{ \
				fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
				fprintf(stderr, __VA_ARGS__); \
				fputc('\n', stderr); \
			} \
		} \
	} while (0)

/* ---------- reference: decimal texts as digit strings ---------- */

typedef struct Dec
{
	int			kind;			/* -2 -Infinity, -1 finite negative, 0 zero, 1 finite positive, 2 Infinity, 3 NaN */
	char		ip[SIFT_NIMG_EXP_MAX + 8];	/* integer digits without leading zeros */
	char		fp[SIFT_NIMG_DSCALE_MAX + 8];	/* fraction digits without trailing zeros */
} Dec;

static Dec *
dec_parse(const char *t, Dec *d)
{
	const char *p = t;
	size_t		n = 0,
				m = 0;
	bool		neg = false,
				nonzero = false;

	memset(d, 0, offsetof(Dec, ip));
	if (strcmp(t, "NaN") == 0)
		return d->kind = 3, d;
	if (strcmp(t, "Infinity") == 0)
		return d->kind = 2, d;
	if (strcmp(t, "-Infinity") == 0)
		return d->kind = -2, d;
	if (*p == '-')
		neg = true, p++;
	for (; *p && *p != '.'; p++)
	{
		if (n == 0 && *p == '0')
			continue;
		d->ip[n++] = *p;
		nonzero = true;
	}
	d->ip[n] = 0;
	if (*p == '.')
		for (p++; *p; p++)
		{
			d->fp[m++] = *p;
			nonzero |= *p != '0';
		}
	while (m > 0 && d->fp[m - 1] == '0')
		m--;
	d->fp[m] = 0;
	d->kind = !nonzero ? 0 : neg ? -1 : 1;
	return d;
}

static int
mag_cmp(const Dec *a, const Dec *b)
{
	size_t		la = strlen(a->ip),
				lb = strlen(b->ip);
	int			c;

	if (la != lb)
		return la < lb ? -1 : 1;
	c = strcmp(a->ip, b->ip);
	if (c)
		return c < 0 ? -1 : 1;
	c = strcmp(a->fp, b->fp);	/* "5" < "50"? no: the fraction has no trailing zeros, so a prefix is a smaller number */
	return c < 0 ? -1 : c > 0;
}

static int
ref_cmp(const char *x, const char *y)
{
	static Dec	a,
				b;

	dec_parse(x, &a);
	dec_parse(y, &b);
	if (a.kind != b.kind)
		return a.kind < b.kind ? -1 : 1;
	if (a.kind == -1)
		return -mag_cmp(&a, &b);
	if (a.kind == 1)
		return mag_cmp(&a, &b);
	return 0;
}

/* ---------- generators ---------- */

static char
digit(int bias)
{
	switch (rnd() % 6)
	{
		case 0:
		case 1:
			return bias ? '0' : '9';
		case 2:
			return '9';
		default:
			return (char) ('0' + rnd() % 10);
	}
}

/* a text in the form numeric_out prints: no leading zeros, "0" or "0.xx", a sign only on a non-zero number, "NaN" and the infinities now and then */
static void
gen_text(char *out, int maxint, int maxfrac)
{
	int			ni = (int) (rnd() % (maxint + 1)),
				nf = (int) (rnd() % (maxfrac + 1)),
				n = 0;
	bool		nonzero = false;
	char		ip[SIFT_NIMG_EXP_MAX + 8],
				fp[SIFT_NIMG_DSCALE_MAX + 8];
	int			bias = (int) (rnd() % 2);

	switch (rnd() % 40)
	{
		case 0:
			strcpy(out, "NaN");
			return;
		case 1:
			strcpy(out, "Infinity");
			return;
		case 2:
			strcpy(out, "-Infinity");
			return;
	}
	for (int i = 0; i < ni; i++)
	{
		char		c = digit(bias);

		if (n == 0 && c == '0')
			continue;
		ip[n++] = c;
		nonzero = true;
	}
	if (n == 0)
		ip[n++] = '0';
	ip[n] = 0;
	for (int i = 0; i < nf; i++)
	{
		fp[i] = digit(bias);
		nonzero |= fp[i] != '0';
	}
	fp[nf] = 0;
	sprintf(out, "%s%s%s%s", nonzero && rnd() % 2 ? "-" : "", ip, nf ? "." : "", fp);
}

/* the same number with another number of trailing fraction zeros (or without a fraction) */
static void
rescale(const char *in, char *out, int extra)
{
	const char *dot = strchr(in, '.');
	size_t		n;

	if (!strcmp(in, "NaN") || !strcmp(in, "Infinity") || !strcmp(in, "-Infinity"))
	{
		strcpy(out, in);
		return;
	}
	strcpy(out, in);
	n = strlen(out);
	if (!dot && extra > 0)
		out[n++] = '.';
	if (dot || extra > 0)
		for (int i = 0; i < extra; i++)
			out[n++] = '0';
	out[n] = 0;
}

/* ---------- 128-bit lanes and decimal text ---------- */

static sift_w128
rand_w128(void)
{
	sift_w128	v = (sift_w128) (((sift_wu128) rnd() << 64) | rnd());
	int			sh = (int) (rnd() % 128);

	switch (rnd() % 8)
	{
		case 0:
			return v >> sh;
		case 1:
			return (sift_w128) ((sift_wu128) 1 << (rnd() % 127)) * (rnd() % 2 ? 1 : -1);
		case 2:
			return (sift_w128) (rnd() % 3) - 1;
		case 3:
			return rnd() % 2 ? (sift_w128) (((sift_wu128) 1 << 127) - 1 - rnd() % 4) : -(sift_w128) (((sift_wu128) 1 << 127) - 1) - (sift_w128) (rnd() % 2);
		default:
			return v;
	}
}

static void
test_lanes(long n)
{
	for (long i = 0; i < n; i++)
	{
		sift_w128	a = rand_w128(),
					b = rand_w128();
		int64		ha = sift_col_wide_hi(a),
					la = sift_col_wide_lo(a),
					hb = sift_col_wide_hi(b),
					lb = sift_col_wide_lo(b);

		CHECK(sift_col_wide_join(ha, la) == a, "join");
		CHECK(sift_col_wide_cmp(ha, la, hb, lb) == (a < b ? -1 : a > b), "order of lanes");
	}
}

static void
test_text(long n)
{
	char		buf[SIFT_COL_W128_TEXT];
	sift_w128	out = 0;
	static const struct
	{
		const char *txt;
		int			scale;
		SiftW128Status st;
	}			cases[] = {
		{"170141183460469231731687303715884105727", 0, SIFT_W128_OK},
		{"170141183460469231731687303715884105728", 0, SIFT_W128_ABOVE},
		{"-170141183460469231731687303715884105728", 0, SIFT_W128_OK},
		{"-170141183460469231731687303715884105729", 0, SIFT_W128_BELOW},
		{"1.70141183460469231731687303715884105727", 38, SIFT_W128_OK},
		{"1.70141183460469231731687303715884105728", 38, SIFT_W128_ABOVE},
		{"99999999999999999999999999999999999999", 0, SIFT_W128_OK},
		{"99999999999999999999999999999999999999.99", 2, SIFT_W128_ABOVE},
		{"9999999999999999999999999999999999.99", 2, SIFT_W128_OK},
		{"1000000000000000000000000000000000000000000000000000000000000", 0, SIFT_W128_ABOVE},
		{"-1000000000000000000000000000000000000000000000000000000000000.5", 5, SIFT_W128_BELOW},
		{"1.5", 0, SIFT_W128_INEXACT},
		{"1.00", 0, SIFT_W128_OK},
		{"0.05", 2, SIFT_W128_OK},
		{"0", 38, SIFT_W128_OK},
		{"-0.00000000000000000000000000000000000001", 38, SIFT_W128_OK},
		{"", 0, SIFT_W128_BAD},
		{"-", 0, SIFT_W128_BAD},
		{".", 0, SIFT_W128_BAD},
		{"1.2.3", 3, SIFT_W128_BAD},
		{"12a", 0, SIFT_W128_BAD},
		{"NaN", 0, SIFT_W128_BAD},
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		CHECK(sift_col_decimal_to_w128(cases[i].txt, cases[i].scale, &out) == cases[i].st, "case %zu \"%s\" scale %d", i, cases[i].txt, cases[i].scale);
	CHECK(sift_col_decimal_to_w128("170141183460469231731687303715884105727", 0, &out) == SIFT_W128_OK && out == (sift_w128) (((sift_wu128) 1 << 127) - 1), "max");
	CHECK(sift_col_decimal_to_w128("-170141183460469231731687303715884105728", 0, &out) == SIFT_W128_OK && out == (sift_w128) ((sift_wu128) 1 << 127), "min");
	CHECK(sift_col_decimal_to_w128("-12.5", 3, &out) == SIFT_W128_OK && out == -12500, "pad fraction");
	CHECK(sift_col_w128_to_decimal(-12500, 3, buf) == 7 && strcmp(buf, "-12.500") == 0, "format -12.500");
	CHECK(sift_col_w128_to_decimal(5, 2, buf) == 4 && strcmp(buf, "0.05") == 0, "format 0.05");
	CHECK(sift_col_w128_to_decimal(0, 0, buf) == 1 && strcmp(buf, "0") == 0, "format 0");
	CHECK(sift_col_w128_to_decimal(0, 3, buf) == 5 && strcmp(buf, "0.000") == 0, "format 0.000");
	CHECK(sift_col_w128_to_decimal((sift_w128) ((sift_wu128) 1 << 127), 0, buf) > 0 && strcmp(buf, "-170141183460469231731687303715884105728") == 0, "format min");
	for (long i = 0; i < n; i++)
	{
		sift_w128	v = rand_w128();
		int			scale = (int) (rnd() % (SIFT_COL_MAX_WIDE_SCALE + 1)),
					len = sift_col_w128_to_decimal(v, scale, buf);
		const char *dot = strchr(buf, '.');

		CHECK(len == (int) strlen(buf) && len < SIFT_COL_W128_TEXT, "length");
		CHECK((scale == 0) == (dot == NULL) && (dot == NULL || (int) strlen(dot + 1) == scale), "fraction digits of %s at %d", buf, scale);
		CHECK(sift_col_decimal_to_w128(buf, scale, &out) == SIFT_W128_OK && out == v, "round trip %s at %d", buf, scale);
		/* more zeros than the scale needs are exact, a longer scale pads */
		if (scale < SIFT_COL_MAX_WIDE_SCALE)
		{
			char		wider[SIFT_COL_W128_TEXT + 8];

			strcpy(wider, buf);
			if (!dot)
				strcat(wider, ".");
			strcat(wider, "0");
			CHECK(sift_col_decimal_to_w128(wider, scale, &out) == SIFT_W128_OK && out == v, "trailing zero of %s", wider);
		}
	}
}

/* digit strings around 2^127 and 2^128, decided by comparing the strings */
static void
test_text_range(long n)
{
	static const char *maxpos = "170141183460469231731687303715884105727",
			   *maxneg = "170141183460469231731687303715884105728";
	sift_w128	out;

	for (long i = 0; i < n; i++)
	{
		char		s[64];
		int			len = 36 + (int) (rnd() % 7),
					k = 0;
		bool		neg = rnd() % 2;
		int			expect;
		const char *lim = neg ? maxneg : maxpos;

		if (neg)
			s[k++] = '-';
		for (int d = 0; d < len; d++)
			s[k++] = d == 0 ? (char) ('1' + rnd() % 9) : (rnd() % 3 == 0 ? lim[d % 39] : (char) ('0' + rnd() % 10));
		if (rnd() % 4 == 0 && len == 39)
			memcpy(s + neg, lim, 39);			/* the limit itself */
		s[k] = 0;
		len = (int) strlen(s + neg);
		expect = len < 39 || (len == 39 && strcmp(s + neg, lim) <= 0) ? 0 : neg ? -1 : 1;
		CHECK((int) sift_col_decimal_to_w128(s, 0, &out) == expect, "range of %s: %d, expected %d", s, (int) sift_col_decimal_to_w128(s, 0, &out), expect);
	}
}

/* ---------- interval ---------- */

static void
test_interval(long n)
{
	CHECK(sift_col_interval_total(0, 30, 0) == sift_col_interval_total(0, 0, 1), "1 mon = 30 days");
	CHECK(sift_col_interval_total(SIFT_COL_USECS_PER_DAY, 0, 0) == sift_col_interval_total(0, 1, 0), "24 h = 1 day");
	CHECK(sift_col_interval_total(0, 1, 1) == sift_col_interval_total(0, 31, 0), "1 mon 1 day = 31 days");
	CHECK(sift_col_interval_total(0, 0, 2) > sift_col_interval_total(0, 59, 0), "2 mon > 59 days");
	CHECK(sift_col_interval_total(INT64_MAX, INT32_MAX, INT32_MAX) > sift_col_interval_total(INT64_MAX, INT32_MAX - 1, INT32_MAX), "extreme totals do not wrap");
	CHECK(sift_col_interval_total(INT64_MIN, INT32_MIN, INT32_MIN) < sift_col_interval_total(INT64_MIN, INT32_MIN + 1, INT32_MIN), "extreme negative totals");
	for (long i = 0; i < n; i++)
	{
		int64		time = (int64) rnd() >> (rnd() % 64);
		int32		day = (int32) rnd() >> (rnd() % 32),
					month = (int32) rnd() >> (rnd() % 32),
					d2 = 0,
					m2 = 0;
		int64		t2 = 0;
		sift_w128	total = sift_col_interval_total(time, day, month),
					back;

		if (rnd() % 10 == 0)
			time = rnd() % 2 ? INT64_MAX : INT64_MIN;
		total = sift_col_interval_total(time, day, month);
		back = sift_col_wide_join(sift_col_wide_hi(total), sift_col_wide_lo(total));
		CHECK(back == total, "lanes of a total");
		sift_col_interval_raw(back, sift_col_interval_md(day, month), &t2, &d2, &m2);
		CHECK(t2 == time && d2 == day && m2 == month, "raw triple (%lld, %d, %d) -> (%lld, %d, %d)", (long long) time, day, month, (long long) t2, d2, m2);
	}
}

/* ---------- images ---------- */

static char tbuf[SIFT_NIMG_TEXT_MAX + 16];

static int
img_cmp(const uint8 *a, uint32 la, const uint8 *b, uint32 lb)
{
	int			c = memcmp(a, b, la < lb ? la : lb);

	return c ? (c < 0 ? -1 : 1) : (la < lb ? -1 : la > lb);
}

static uint32
img_of(const char *txt, uint8 *img, int32 *dscale)
{
	return sift_col_numimg_from_text(txt, img, dscale);
}

static void
test_image_fixed(void)
{
	static const char *ordered[] = {"-Infinity", "-1000000000000000000000", "-100.5", "-100.49", "-100", "-99.99", "-1.01", "-1", "-0.5", "-0.05",
		"-0.0000000001", "0", "0.0000000001", "0.05", "0.5", "1", "1.01", "2", "9", "10", "99.99", "100", "100.49", "100.5",
		"1000000000000000000000", "Infinity", "NaN"};
	uint8		a[64],
				b[64];
	int32		da,
				db;

	for (size_t i = 0; i < sizeof(ordered) / sizeof(ordered[0]); i++)
	{
		uint32		la = img_of(ordered[i], a, &da);

		CHECK(la > 0 && sift_col_numimg_valid(a, la), "image of %s", ordered[i]);
		CHECK(sift_col_numimg_to_text(a, la, da, tbuf, sizeof(tbuf)) == strlen(ordered[i]) && strcmp(tbuf, ordered[i]) == 0, "text of %s is %s", ordered[i], tbuf);
		for (size_t j = 0; j < sizeof(ordered) / sizeof(ordered[0]); j++)
		{
			uint32		lb = img_of(ordered[j], b, &db);

			CHECK(img_cmp(a, la, b, lb) == (i < j ? -1 : i > j), "%s vs %s", ordered[i], ordered[j]);
		}
	}
	/* equal numbers, one image, the display scale apart */
	{
		static const char *same[][2] = {{"1.0", "1.00"}, {"0", "0.000"}, {"100", "100.0"}, {"0.50", "0.5"}, {"-12.3400", "-12.34"}};

		for (size_t i = 0; i < sizeof(same) / sizeof(same[0]); i++)
		{
			uint32		la = img_of(same[i][0], a, &da),
						lb = img_of(same[i][1], b, &db);

			CHECK(img_cmp(a, la, b, lb) == 0 && da != db, "%s = %s", same[i][0], same[i][1]);
		}
	}
	/* a malformed text is not an image */
	CHECK(img_of("", a, &da) == 0 && img_of("-", a, &da) == 0 && img_of(".", a, &da) == 0 && img_of("1x", a, &da) == 0 && img_of("nan", a, &da) == 0, "bad text");
	/* invalid images */
	{
		static const uint8 bad[][8] = {{0}, {0x10, 1}, {0x30, 1}, {0x40}, {0x40, 0x80, 0, 0}, {0x40, 0x80, 0, 1}, {0x40, 0x80, 0, 1, 0},
			{0x40, 0x80, 0, 1, 101}, {0x40, 0x80, 0, 1, 1}, {0x20, 0x7F, 0xFF, 0xFF, 0xFE}, {0x60, 0}, {0x70}, {0x40, 0xFF, 0xFF, 0xFF, 11}};
		static const uint32 badlen[] = {1, 2, 2, 1, 4, 5, 5, 5, 5, 5, 2, 1, 5};

		for (size_t i = 0; i < sizeof(badlen) / sizeof(badlen[0]); i++)
			CHECK(!sift_col_numimg_valid(bad[i], badlen[i]) && sift_col_numimg_to_text(bad[i], badlen[i], 0, tbuf, sizeof(tbuf)) == 0, "invalid image %zu", i);
	}
	CHECK(!sift_col_numimg_valid(NULL, 0), "empty");
}

static void
test_image_limits(void)
{
	size_t		n = SIFT_NIMG_EXP_MAX;
	char	   *big = malloc(n + SIFT_NIMG_DSCALE_MAX + 8),
			   *round = malloc(n + SIFT_NIMG_DSCALE_MAX + 8);
	uint8	   *img = malloc(SIFT_NIMG_MAXLEN(n + SIFT_NIMG_DSCALE_MAX));
	int32		dscale = 0;
	uint32		len;

	/* 131072 digits before the point and 16383 after it, the largest text of a numeric */
	for (size_t i = 0; i < n; i++)
		big[i] = i == 0 ? '7' : (char) ('0' + (i * 7) % 10);
	big[n] = '.';
	for (int i = 0; i < SIFT_NIMG_DSCALE_MAX; i++)
		big[n + 1 + i] = (char) ('0' + (i * 3 + 1) % 10);
	big[n + 1 + SIFT_NIMG_DSCALE_MAX - 1] = '5';
	big[n + 1 + SIFT_NIMG_DSCALE_MAX] = 0;
	len = sift_col_numimg_from_text(big, img, &dscale);
	CHECK(len > 0 && dscale == SIFT_NIMG_DSCALE_MAX && len <= SIFT_NIMG_MAXLEN(strlen(big)), "largest numeric: image %u", len);
	CHECK(sift_col_numimg_valid(img, len), "largest numeric is valid");
	{
		uint32		tl = sift_col_numimg_to_text(img, len, dscale, round, n + SIFT_NIMG_DSCALE_MAX + 8);

		CHECK(tl == strlen(big) && strcmp(round, big) == 0, "largest numeric round trip");
		CHECK(sift_col_numimg_to_text(img, len, dscale, round, tl) == 0, "one byte short: nothing written beyond cap");
		CHECK(sift_col_numimg_to_text(img, len, SIFT_NIMG_DSCALE_MAX + 1, round, n + SIFT_NIMG_DSCALE_MAX + 8) == 0, "display scale out of range");
	}
	/* the smallest: 0.000...01 with 16383 fraction digits */
	for (int i = 0; i < SIFT_NIMG_DSCALE_MAX - 1; i++)
		big[2 + i] = '0';
	big[0] = '0';
	big[1] = '.';
	big[2 + SIFT_NIMG_DSCALE_MAX - 1] = '1';
	big[2 + SIFT_NIMG_DSCALE_MAX] = 0;
	len = sift_col_numimg_from_text(big, img, &dscale);
	CHECK(len > 0 && dscale == SIFT_NIMG_DSCALE_MAX, "smallest numeric: image %u dscale %d", len, dscale);
	CHECK(sift_col_numimg_to_text(img, len, dscale, round, n + SIFT_NIMG_DSCALE_MAX + 8) == strlen(big) && strcmp(round, big) == 0, "smallest numeric round trip");
	free(big);
	free(round);
	free(img);
}

static void
test_image_random(long n)
{
	char		x[SIFT_NIMG_TEXT_MAX],
				y[SIFT_NIMG_TEXT_MAX],
				z[SIFT_NIMG_TEXT_MAX],
				back[SIFT_NIMG_TEXT_MAX];
	uint8		a[SIFT_NIMG_MAXLEN(80)],
				b[SIFT_NIMG_MAXLEN(80)],
				c[SIFT_NIMG_MAXLEN(80)];

	for (long i = 0; i < n; i++)
	{
		int32		da,
					db,
					dc;
		uint32		la,
					lb,
					lc;
		int			maxint = rnd() % 4 == 0 ? 3 : 38,
					maxfrac = rnd() % 4 == 0 ? 3 : 38;

		gen_text(x, maxint, maxfrac);
		gen_text(y, maxint, maxfrac);
		if (rnd() % 3 == 0)
			rescale(x, y, (int) (rnd() % 4));		/* equal numbers, other display scale */
		rescale(x, z, (int) (rnd() % 5));
		la = img_of(x, a, &da);
		lb = img_of(y, b, &db);
		lc = img_of(z, c, &dc);
		CHECK(la > 0 && lb > 0 && lc > 0, "image of %s / %s / %s", x, y, z);
		CHECK(la <= SIFT_NIMG_MAXLEN(strlen(x)), "length bound");
		CHECK(sift_col_numimg_valid(a, la), "valid %s", x);
		CHECK(sift_col_numimg_to_text(a, la, da, back, sizeof(back)) == strlen(x) && strcmp(back, x) == 0, "round trip %s -> %s", x, back);
		CHECK(img_cmp(a, la, b, lb) == ref_cmp(x, y), "order %s vs %s: image %d, reference %d", x, y, img_cmp(a, la, b, lb), ref_cmp(x, y));
		CHECK((ref_cmp(x, z) == 0) == (img_cmp(a, la, c, lc) == 0) && ref_cmp(x, z) == 0, "equal %s %s", x, z);
		/* a prefix of the digits is a different, smaller or larger number, never the same image */
		CHECK(ref_cmp(x, y) != 0 || img_cmp(a, la, b, lb) == 0, "equal numbers %s %s have one image", x, y);
	}
}

static void
test_image_bytes(long n)
{
	uint8		img[96];
	char		back[SIFT_NIMG_TEXT_MAX],
				again[SIFT_NIMG_TEXT_MAX];
	uint8		re[96];

	for (long i = 0; i < n; i++)
	{
		uint32		len = (uint32) (rnd() % 24);
		int32		ds = (int32) (rnd() % 40),
					ds2;

		if (rnd() % 2)
		{
			/* a plausible image: a tag, an exponent near zero and pairs, then flipped bytes */
			static const uint8 tags[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};

			img[0] = tags[rnd() % 6];
			for (uint32 k = 1; k < sizeof(img); k++)
				img[k] = (uint8) (k < 4 ? (k == 1 ? 0x7F + rnd() % 2 : rnd() % 3 ? 0 : rnd()) : rnd() % 3 == 0 ? rnd() : 1 + rnd() % 100);
			if (img[0] == 0x20)
				for (uint32 k = 1; k < sizeof(img); k++)
					img[k] ^= 0xFF;
			if (img[0] == 0x20 && len > 1)
				img[len - 1] = 0xFF;
		}
		else
			for (uint32 k = 0; k < sizeof(img); k++)
				img[k] = (uint8) rnd();
		if (sift_col_numimg_valid(img, len))
		{
			/* a valid image prints with a scale that shows every digit, and the text, read back, is the same image: the form is canonical */
			bool		finite = img[0] == 0x20 || img[0] == 0x40;
			int			inv = img[0] == 0x20 ? 0xFF : 0;
			long		exp = finite ? (long) ((img[1] ^ inv) << 16 | (img[2] ^ inv) << 8 | (img[3] ^ inv)) - (1 << 23) : 0,
						need = finite ? 2L * (len - 4 - (img[0] == 0x20)) - exp : 0;
			uint32		tl,
						rl;

			ds = (int32) (need < 0 ? 0 : need > 300 ? 300 : need);
			tl = sift_col_numimg_to_text(img, len, ds, back, sizeof(back));
			CHECK(tl > 0 || need > 300 || exp > 300, "a valid image prints (%u bytes, exp %ld, scale %d)", len, exp, ds);
			if (tl > 0 && need <= 300)
			{
				rl = sift_col_numimg_from_text(back, re, &ds2);
				CHECK(rl == len && memcmp(re, img, len) == 0 && ds2 == ds, "image %u bytes, text %s, back %u bytes", len, back, rl);
				CHECK(sift_col_numimg_to_text(re, rl, ds2, again, sizeof(again)) == tl && strcmp(again, back) == 0, "again");
			}
		}
		else
			CHECK(sift_col_numimg_to_text(img, len, ds, back, sizeof(back)) == 0, "invalid image prints nothing");
		/* tiny caps must never write past the buffer (ASan) */
		{
			char		tiny[5];

			(void) sift_col_numimg_to_text(img, len, ds, tiny, (uint32) (rnd() % 6));
		}
	}
}

int
main(int argc, char **argv)
{
	long		n = argc > 1 ? atol(argv[1]) : 1;

	test_text(20000 * n);
	test_text_range(100000 * n);
	test_lanes(200000 * n);
	test_interval(100000 * n);
	test_image_fixed();
	test_image_limits();
	test_image_random(30000 * n);
	test_image_bytes(300000 * n);
	printf("colwide: %ld checks, %ld failures\n", checks, fails);
	return fails != 0;
}
