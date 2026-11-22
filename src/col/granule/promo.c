/*
 * col/granule/promo.c - the text of an astext leaf held as the type it spells. The leaf of a number, a boolean, a date or a timestamp costs a dozen bytes a row as text and its zone is no
 * range; a part whose rows are, but for a few, one canonical spelling of one type holds them as that type (SIFT_CENC_STR_PROMO: a granule chunk of the integers, the scaled decimals or the
 * bits of the codec, the other rows as text beside it) and says what the least and the greatest are. A value is of the type only when writing it back gives the bytes it came from, so a
 * read gives the text `->>` gives, byte for byte, and `1.50` among `1.5` is one of the few rows kept as text.
 */
#include <math.h>

#include "core/host.h"

#include "lib/stringinfo.h"

#include "common/shortest_dec.h"

#include "col/granule/granule_int.h"
#include "core/damage.h"

#define SIFT_PROMO_SAMPLE		4096
#define SIFT_PROMO_MIN_ROWS		64
#define SIFT_PROMO_DIGITS		18
#define SIFT_PROMO_FMT_MAX		40
#define SIFT_PROMO_ARENA_ROW	(SIFT_PROMO_PREFIX_MAX + SIFT_PROMO_PREFIX_DIGITS + 1)
#define SIFT_PROMO_PART_PERMILLE	975		/* at least this many of the rows of a part are of the type */
#define SIFT_PROMO_GRANULE_PERCENT	90		/* a granule with fewer keeps its text */
#define SIFT_PROMO_NSLOTS		25
#define SIFT_PROMO_SLOT_FLOAT64	24

static const int64 p10[10] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};

static int64
days_from_civil(int64 y, unsigned m, unsigned d)
{
	int64		era;
	unsigned	yoe,
				doy,
				doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned) (y - era * 400);
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64) doe - 719468;
}

static void
civil_from_days(int64 z, int64 *y, unsigned *m, unsigned *d)
{
	int64		era;
	unsigned	doe,
				yoe,
				doy,
				mp;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (unsigned) (z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = (int64) yoe + era * 400 + (*m <= 2);
}

static bool
digits(const char *s, int n, int64 *v)
{
	int64		x = 0;

	for (int i = 0; i < n; i++)
	{
		if (s[i] < '0' || s[i] > '9')
			return false;
		x = x * 10 + (s[i] - '0');
	}
	*v = x;
	return true;
}

static bool
promo_int(const char *s, uint32 len, int64 *v)
{
	bool		neg = len > 0 && s[0] == '-';
	uint32		from = neg;

	if (len == from || len - from > SIFT_PROMO_DIGITS || (s[from] == '0' && len - from > 1))
		return false;
	if (!digits(s + from, len - from, v) || (neg && *v == 0))
		return false;
	*v = neg ? -*v : *v;
	return true;
}

static bool
promo_num64(const char *s, uint32 len, int scale, int64 *v)
{
	bool		neg = len > 0 && s[0] == '-';
	uint32		from = neg;
	int64		whole,
				frac;
	uint32		dot = from;

	while (dot < len && s[dot] != '.')
		dot++;
	if (dot == from || dot == len || len - dot - 1 != (uint32) scale || dot - from + scale > SIFT_PROMO_DIGITS || (s[from] == '0' && dot - from > 1))
		return false;
	if (!digits(s + from, dot - from, &whole) || !digits(s + dot + 1, scale, &frac))
		return false;
	*v = whole * p10[scale] + frac;
	if (neg && *v == 0)
		return false;
	*v = neg ? -*v : *v;
	return true;
}

static bool
promo_date(const char *s, int64 *days)
{
	int64		y,
				m,
				d;

	if (s[4] != '-' || s[7] != '-' || !digits(s, 4, &y) || !digits(s + 5, 2, &m) || !digits(s + 8, 2, &d) || y < 1 || m < 1 || m > 12 || d < 1 || d > 31)
		return false;
	*days = days_from_civil(y, (unsigned) m, (unsigned) d);
	return true;
}

static bool
promo_ts(const char *s, uint32 len, const SiftColPromo *pm, int64 *v)
{
	uint32		suffix = (pm->flags & SIFT_PROMO_FLAG_TS_Z_SUFFIX) ? 1 : 6,
				frac = pm->param ? pm->param + 1 : 0;
	int64		days,
				h,
				mi,
				sec,
				f = 0;

	if (len != 19 + frac + suffix || s[10] != ((pm->flags & SIFT_PROMO_FLAG_TS_T_SEP) ? 'T' : ' ') || s[13] != ':' || s[16] != ':')
		return false;
	if (!promo_date(s, &days) || !digits(s + 11, 2, &h) || !digits(s + 14, 2, &mi) || !digits(s + 17, 2, &sec) || h > 23 || mi > 59 || sec > 59)
		return false;
	if (frac && (s[19] != '.' || !digits(s + 20, pm->param, &f)))
		return false;
	if ((pm->flags & SIFT_PROMO_FLAG_TS_Z_SUFFIX) ? s[len - 1] != 'Z' : memcmp(s + len - 6, "+00:00", 6) != 0)
		return false;
	*v = (((days * 24 + h) * 60 + mi) * 60 + sec) * 1000000 + f * (pm->param == 3 ? 1000 : 1);
	return true;
}

/* A double as an integer that orders as it does (the bits, the magnitude of a negative one turned over), and back */
static int64
f64_key(double d)
{
	int64		b;

	memcpy(&b, &d, sizeof(b));
	return b < 0 ? b ^ PG_INT64_MAX : b;
}

static double
key_f64(int64 v)
{
	int64		b = v < 0 ? v ^ PG_INT64_MAX : v;
	double		d;

	memcpy(&d, &b, sizeof(d));
	return d;
}

/* A number of any length and exponent that a double holds: it is of the type only if the shortest text of the double is this text, which promo_value's callers check */
static bool
promo_f64(const char *s, uint32 len, int64 *v)
{
	char		buf[SIFT_PROMO_FMT_MAX];
	char	   *end;
	double		d;

	if (len == 0 || len >= sizeof(buf))
		return false;
	memcpy(buf, s, len);
	buf[len] = '\0';
	d = strtod(buf, &end);
	if (end != buf + len || isnan(d) || isinf(d))
		return false;
	*v = f64_key(d);
	return true;
}

static bool
promo_value(const SiftColPromo *pm, const char *s, uint32 len, int64 *v)
{
	switch (pm->kind)
	{
		case SIFT_PROMO_BOOL:
			*v = len == 4 && memcmp(s, "true", 4) == 0;
			return *v || (len == 5 && memcmp(s, "false", 5) == 0);
		case SIFT_PROMO_INT64:
			return promo_int(s, len, v);
		case SIFT_PROMO_NUM64:
			return promo_num64(s, len, pm->param, v);
		case SIFT_PROMO_DATE:
			return len == 10 && promo_date(s, v);
		case SIFT_PROMO_TIMESTAMPTZ:
			return promo_ts(s, len, pm, v);
		case SIFT_PROMO_FLOAT64:
			return promo_f64(s, len, v);
	}
	return false;
}

static int
put_digits(char *out, int64 v, int width)
{
	for (int i = width - 1; i >= 0; i--, v /= 10)
		out[i] = '0' + (char) (v % 10);
	return width;
}

static uint32
format_ts(const SiftColPromo *pm, int64 v, char *out)
{
	int64		days = v >= 0 ? v / 86400000000 : -((-v + 86399999999) / 86400000000),
				rem = v - days * 86400000000,
				y;
	unsigned	m,
				d;
	uint32		n = 0;

	civil_from_days(days, &y, &m, &d);
	n += put_digits(out + n, y, 4);
	out[n++] = '-';
	n += put_digits(out + n, m, 2);
	out[n++] = '-';
	n += put_digits(out + n, d, 2);
	out[n++] = (pm->flags & SIFT_PROMO_FLAG_TS_T_SEP) ? 'T' : ' ';
	n += put_digits(out + n, rem / 3600000000, 2);
	out[n++] = ':';
	n += put_digits(out + n, rem / 60000000 % 60, 2);
	out[n++] = ':';
	n += put_digits(out + n, rem / 1000000 % 60, 2);
	if (pm->param)
	{
		out[n++] = '.';
		n += put_digits(out + n, rem % 1000000 / (pm->param == 3 ? 1000 : 1), pm->param);
	}
	if (pm->flags & SIFT_PROMO_FLAG_TS_Z_SUFFIX)
		out[n++] = 'Z';
	else
	{
		memcpy(out + n, "+00:00", 6);
		n += 6;
	}
	return n;
}

/* The text of a value of the type, at most SIFT_PROMO_FMT_MAX bytes */
static uint32
format_value(const SiftColPromo *pm, int64 v, char *out)
{
	uint32		n = 0;
	uint64		a = v < 0 ? (uint64) 0 - (uint64) v : (uint64) v;
	char		tmp[24];
	int			k = 0;

	switch (pm->kind)
	{
		case SIFT_PROMO_BOOL:
			memcpy(out, v ? "true" : "false", v ? 4 : 5);
			return v ? 4 : 5;
		case SIFT_PROMO_DATE:
			{
				int64		y;
				unsigned	m,
							d;

				civil_from_days(v, &y, &m, &d);
				n += put_digits(out + n, y, 4);
				out[n++] = '-';
				n += put_digits(out + n, m, 2);
				out[n++] = '-';
				n += put_digits(out + n, d, 2);
				return n;
			}
		case SIFT_PROMO_TIMESTAMPTZ:
			return format_ts(pm, v, out);
		case SIFT_PROMO_FLOAT64:
			return (uint32) double_to_shortest_decimal_bufn(key_f64(v), out);
		default:
			break;
	}
	if (v < 0)
		out[n++] = '-';
	do
		tmp[k++] = '0' + (char) (a % 10);
	while ((a /= 10) != 0 || (pm->kind == SIFT_PROMO_NUM64 && k <= pm->param));
	for (int i = k - 1; i >= 0; i--)
	{
		if (pm->kind == SIFT_PROMO_NUM64 && i == pm->param - 1)
			out[n++] = '.';
		out[n++] = tmp[i];
	}
	return n;
}

static bool
tag_is_text(uint8 tag)
{
	return tag == SIFT_COLTAG_UNKNOWN || tag == SIFT_COLTAG_STRING;
}

/* Whether the text of a row, with the kind its value carries, is of the type: it parses and writes back as the same bytes */
static bool
row_is_typed(const SiftColPromo *pm, const char *s, uint32 len, uint8 tag, int64 *v)
{
	char		back[SIFT_PROMO_FMT_MAX];

	if (pm->orig_tag == SIFT_COLTAG_UNKNOWN ? !tag_is_text(tag) : tag != pm->orig_tag)
		return false;
	if (len == 0 || len >= SIFT_PROMO_FMT_MAX || !promo_value(pm, s, len, v))
		return false;
	return format_value(pm, *v, back) == len && memcmp(back, s, len) == 0;
}

/* The types a row may be of, each as the part would hold it: slot 0 the boolean, 1 the integer, 2..10 the decimals of scale 1..9, 11 the date, 12..23 the timestamps */
static SiftColPromo
slot_promo(int slot)
{
	SiftColPromo pm = {0};

	if (slot == 0)
		pm = (SiftColPromo) {.kind = SIFT_PROMO_BOOL,.orig_tag = SIFT_COLTAG_BOOLEAN};
	else if (slot == 1)
		pm = (SiftColPromo) {.kind = SIFT_PROMO_INT64,.orig_tag = SIFT_COLTAG_NUMBER};
	else if (slot < 11)
		pm = (SiftColPromo) {.kind = SIFT_PROMO_NUM64,.param = slot - 1,.orig_tag = SIFT_COLTAG_NUMBER};
	else if (slot == 11)
		pm = (SiftColPromo) {.kind = SIFT_PROMO_DATE};
	else if (slot == SIFT_PROMO_SLOT_FLOAT64)
		pm = (SiftColPromo) {.kind = SIFT_PROMO_FLOAT64,.orig_tag = SIFT_COLTAG_NUMBER};
	else
	{
		int			k = slot - 12;

		pm = (SiftColPromo) {.kind = SIFT_PROMO_TIMESTAMPTZ,.param = (uint8) (k / 4 * 3),.flags = (uint8) ((k & 2 ? SIFT_PROMO_FLAG_TS_Z_SUFFIX : 0) | (k & 1 ? SIFT_PROMO_FLAG_TS_T_SEP : 0))};
	}
	return pm;
}

/* The type most of the sampled rows are of, the narrowest first; of the decimals and of the timestamps the spelling most of them have */
static int
pick_slot(const int *count, int sampled)
{
	int			best = -1;

	for (int slot = 0; slot < 2; slot++)
		if (count[slot] * 100 >= sampled * 98)
			return slot;
	for (int slot = 2; slot < 11; slot++)
		if (count[slot] * 100 >= sampled * 98 && (best < 0 || count[slot] > count[best]))
			best = slot;
	if (best >= 0 || count[11] * 100 >= sampled * 98)
		return best >= 0 ? best : 11;
	for (int slot = 12; slot < SIFT_PROMO_SLOT_FLOAT64; slot++)
		if (count[slot] * 100 >= sampled * 98 && (best < 0 || count[slot] > count[best]))
			best = slot;
	return best >= 0 ? best : count[SIFT_PROMO_SLOT_FLOAT64] * 100 >= sampled * 98 ? SIFT_PROMO_SLOT_FLOAT64 : -1;
}

bool
sift_promo_infer(const SiftColIn *col, int n, const SiftDictEnc *dict, SiftColPromo *out)
{
	int			nn = 0,
				step,
				sampled = 0,
				best = -1;
	int			count[SIFT_PROMO_NSLOTS] = {0};
	int64		conforming = 0,
				v;

	if (dict != NULL)
		return sift_promo_prefix_infer(col, n, dict, out);
	if (col->kind != SIFT_CELL_STR || !col->astext || col->hint != SIFT_CHINT_AUTO || col->dcode != NULL || col->group1 > 0 || col->s == NULL)
		return sift_promo_prefix_infer(col, n, NULL, out);
	for (int i = 0; i < n; i++)
		nn += col->isnull == NULL || !col->isnull[i];
	if (nn < SIFT_PROMO_MIN_ROWS)
		return false;
	step = Max(1, nn / SIFT_PROMO_SAMPLE);
	for (int i = 0, k = 0; i < n; i++)
	{
		if (col->isnull != NULL && col->isnull[i])
			continue;
		if (k++ % step != 0)
			continue;
		sampled++;
		for (int slot = 0; slot < SIFT_PROMO_NSLOTS; slot++)
		{
			SiftColPromo pm = slot_promo(slot);

			count[slot] += row_is_typed(&pm, col->s[i], col->slen[i], col->tag ? col->tag[i] : 0, &v);
		}
	}
	best = pick_slot(count, sampled);
	if (best < 0)
		return sift_promo_prefix_infer(col, n, NULL, out);
	*out = slot_promo(best);
	for (int i = 0; i < n; i++)
		if ((col->isnull == NULL || !col->isnull[i]) && row_is_typed(out, col->s[i], col->slen[i], col->tag ? col->tag[i] : 0, &v))
			conforming++;
	return conforming * 1000 >= (int64) nn * SIFT_PROMO_PART_PERMILLE || sift_promo_prefix_infer(col, n, NULL, out);
}

bool
sift_chunk_put_promo(StringInfo s, const SiftColIn *col, int n, int nn, SiftChunkRef *ref, const SiftChunkOpts *opt)
{
	const SiftColPromo *pm = col->promo;
	SiftPromoPrefix px = {0};
	bool		prefixed = pm->kind == SIFT_PROMO_PREFIX_INT;
	int64	   *typed = palloc(Max(nn, 1) * sizeof(int64));
	uint16	   *xrank = palloc(Max(nn, 1) * sizeof(uint16));
	uint32	   *xoff = palloc((Max(nn, 1) + 1) * sizeof(uint32));
	StringInfoData xbytes,
				inner;
	SiftChunkRef iref = {0};
	SiftColIn	t = {.kind = SIFT_CELL_INT,.hint = pm->kind == SIFT_PROMO_BOOL ? SIFT_CHINT_BOOL : pm->kind == SIFT_PROMO_NUM64 ? SIFT_CHINT_NUM64 : SIFT_CHINT_AUTO,.scale = pm->param};
	int			nt = 0,
				nx = 0,
				rank = 0;
	uint32		hdr[2];

	initStringInfo(&xbytes);
	initStringInfo(&inner);
	if (prefixed && !sift_prefix_derive(col, n, &px))
		nn = 0;
	for (int i = 0; i < n; i++)
	{
		int64		v;

		if (col->isnull != NULL && col->isnull[i])
			continue;
		if (nn > 0 && (prefixed ? sift_prefix_typed(&px, col->s[i], col->slen[i], col->tag ? col->tag[i] : 0, &v) : row_is_typed(pm, col->s[i], col->slen[i], col->tag ? col->tag[i] : 0, &v)))
			typed[nt++] = v;
		else
		{
			xrank[nx] = (uint16) rank;
			xoff[nx++] = xbytes.len;
			appendBinaryStringInfo(&xbytes, col->s[i], col->slen[i]);
		}
		rank++;
	}
	if (nt == 0 || (int64) nt * 100 < (int64) nn * SIFT_PROMO_GRANULE_PERCENT)
	{
		pfree(typed);
		pfree(xrank);
		pfree(xoff);
		pfree(xbytes.data);
		pfree(inner.data);
		return false;
	}
	xoff[nx] = xbytes.len;
	t.i = typed;
	sift_chunk_put_int(&inner, &t, nt, nt, &iref, opt);
	appendBinaryStringInfo(s, (char *) pm, sizeof(*pm));
	hdr[0] = nx;
	hdr[1] = iref.enc;
	appendBinaryStringInfo(s, (char *) hdr, 8);
	if (prefixed)
		sift_prefix_put_head(s, &px);
	appendBinaryStringInfo(s, (char *) xrank, nx * sizeof(uint16));
	pad8(s);
	appendBinaryStringInfo(s, (char *) xoff, (nx + 1) * sizeof(uint32));
	pad8(s);
	appendBinaryStringInfo(s, xbytes.data, xbytes.len);
	pad8(s);
	appendBinaryStringInfo(s, inner.data, inner.len);
	ref->enc |= SIFT_CENC_STR_PROMO;
	pfree(typed);
	pfree(xrank);
	pfree(xoff);
	pfree(xbytes.data);
	pfree(inner.data);
	return true;
}

/* The least and the greatest value of the rows of the type in a granule, and whether any row is not of it */
void
sift_promo_zone(const SiftColPromo *pm, const SiftColIn *col, int n, SiftZone *z)
{
	bool		any = false;

	z->flags = SIFT_ZONE_NONE;
	for (int i = 0; i < n; i++)
	{
		int64		v;

		if (col->isnull != NULL && col->isnull[i])
		{
			z->flags |= col->tag != NULL && col->tag[i] != 0 ? SIFT_ZONE_PROMO_EXC : 0;
			continue;
		}
		if (!row_is_typed(pm, col->s[i], col->slen[i], col->tag ? col->tag[i] : 0, &v))
			z->flags |= SIFT_ZONE_PROMO_EXC;
		else if (!any)
		{
			z->min = z->max = v;
			any = true;
		}
		else
		{
			z->min = Min(z->min, v);
			z->max = Max(z->max, v);
		}
	}
	if (any)
		z->flags &= ~SIFT_ZONE_NONE;
	z->flags |= SIFT_ZONE_PROMO | ((uint32) pm->kind << 8) | ((uint32) pm->param << 12) | ((uint32) pm->flags << 16);
}

bool
sift_promo_literal(uint32 zone_flags, const char *s, uint32 len, int64 *v)
{
	SiftColPromo pm = {.kind = SIFT_ZONE_PROMO_KIND(zone_flags),.param = SIFT_ZONE_PROMO_PARAM(zone_flags),.flags = SIFT_ZONE_PROMO_FLAGS(zone_flags)};
	char		back[SIFT_PROMO_FMT_MAX];

	if (len == 0 || len >= SIFT_PROMO_FMT_MAX || !promo_value(&pm, s, len, v))
		return false;
	return format_value(&pm, *v, back) == len && memcmp(back, s, len) == 0;
}

void
sift_promo_strs(const SiftChunk *c, int k0, int k1, const char **p, uint32 *len)
{
	SiftColPromo pm;
	SiftPromoPrefix px = {0};
	uint32		hdr[2];
	const char *b = c->body;
	const char *end = c->body + c->body_len;
	uint32		nx;
	const uint16 *xrank;
	const uint32 *xoff;
	const char *xbytes;
	SiftChunkRef iref = {.flags = SIFT_CHUNK_ALL_VALID};
	SiftChunk	ic = {0};
	int64	   *tv;
	char	   *arena;
	int			x0 = 0,
				x1 = 0,
				nt;

	if (c->body_len < sizeof(pm) + 8)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a promoted chunk is shorter than its header"));
	memcpy(&pm, b, sizeof(pm));
	memcpy(hdr, b + sizeof(pm), 8);
	nx = hdr[0];
	if (nx > (uint32) c->nnonnull)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a promoted chunk holds %u texts for %d rows", nx, c->nnonnull));
	b += sizeof(pm) + 8;
	if (pm.kind == SIFT_PROMO_PREFIX_INT)
		b += sift_prefix_get_head(b, end, &px);
	xrank = (const uint16 *) b;
	b += MAXALIGN((uint64) nx * 2);
	xoff = (const uint32 *) b;
	b += MAXALIGN(((uint64) nx + 1) * 4);
	if (b > end || (nx > 0 && xoff[nx] > (uint64) (end - b)))
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: the texts of a promoted chunk do not fit it"));
	xbytes = b;
	b += MAXALIGN((uint64) xoff[nx]);
	if (b > end || pm.kind == SIFT_PROMO_NONE || pm.kind > SIFT_PROMO_PREFIX_INT)
		SIFT_DAMAGE_DATA(NULL, errmsg("sift: a promoted chunk has type %u", pm.kind));
	nt = c->nnonnull - nx;
	iref.enc = (uint16) hdr[1];
	iref.nnonnull = nt;
	ic = (SiftChunk) {.ref = &iref,.view = c->view,.enc = iref.enc & SIFT_ENC_MASK,.body = b,.body_len = (uint32) (end - b),.nrows = nt,.nnonnull = nt};
	while (x0 < (int) nx && xrank[x0] < k0)
		x0++;
	x1 = x0;
	while (x1 < (int) nx && xrank[x1] < k1)
		x1++;
	tv = palloc(Max(k1 - k0, 1) * sizeof(int64));
	arena = palloc(Max(k1 - k0, 1) * SIFT_PROMO_ARENA_ROW);
	sift_chunk_ints(&ic, k0 - x0, k1 - x1, tv);
	for (int r = k0, xi = x0, ti = 0; r < k1; r++)
	{
		if (xi < x1 && xrank[xi] == r)
		{
			p[r - k0] = xbytes + xoff[xi];
			len[r - k0] = xoff[xi + 1] - xoff[xi];
			xi++;
		}
		else
		{
			p[r - k0] = arena + (uint64) (r - k0) * SIFT_PROMO_ARENA_ROW;
			len[r - k0] = pm.kind == SIFT_PROMO_PREFIX_INT ? sift_prefix_format(&px, tv[ti++], arena + (uint64) (r - k0) * SIFT_PROMO_ARENA_ROW) :
				format_value(&pm, tv[ti++], arena + (uint64) (r - k0) * SIFT_PROMO_ARENA_ROW);
		}
	}
	pfree(tv);
}
