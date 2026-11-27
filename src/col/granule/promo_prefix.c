/*
 * col/granule/promo_prefix.c - text held as a prefix and an integer (SIFT_PROMO_PREFIX_INT): 'tag-49102', 'INV-0001829', 'n-312'. The identifiers of a table are, for most of its rows, one short ASCII
 * prefix and a decimal number; the numbers are a chunk of the codec (a cascade that sees the order they come in) and the prefix is said once in the chunk. A row is of the type only when writing
 * its number back after the prefix, padded to the width the chunk says, gives the bytes it came from; the others stay text beside it, as for the other kinds of col/granule/promo.c.
 */
#include "core/host.h"

#include "lib/stringinfo.h"

#include "col/granule/int.h"
#include "core/damage.h"

#define SIFT_PREFIX_SAMPLE		64
#define SIFT_PREFIX_CANDIDATES	6
#define SIFT_PREFIX_TRIAL_ROWS	8192
#define SIFT_PREFIX_TRIAL_BYTES	(16u * 1024 * 1024)
#define SIFT_PREFIX_MIN_ROWS	64
#define SIFT_PREFIX_CONFORM_PCT	95
#define SIFT_PREFIX_GAIN_PCT	90

static bool
text_tag(uint8 tag)
{
	return tag == SIFT_COLTAG_UNKNOWN || tag == SIFT_COLTAG_STRING;
}

/* The prefix and the width of the number that ends s: the text before the digits that end it, and their count when they start with a zero (a padded number), else 0 */
static bool
derive(const char *s, uint32 len, SiftPromoPrefix *cx)
{
	uint32		d = 0;

	while (d < len && s[len - 1 - d] >= '0' && s[len - 1 - d] <= '9')
		d++;
	if (d == 0 || d > SIFT_PROMO_PREFIX_DIGITS || len - d > SIFT_PROMO_PREFIX_MAX)
		return false;
	cx->plen = (uint8) (len - d);
	cx->min_digits = s[len - d] == '0' && d > 1 ? (uint8) d : 0;
	memcpy(cx->prefix, s, cx->plen);
	return true;
}

uint32
sift_prefix_format(const SiftPromoPrefix *cx, int64 v, char *out)
{
	char		tmp[24];
	int			k = 0;
	uint32		n = cx->plen;

	memcpy(out, cx->prefix, cx->plen);
	do
		tmp[k++] = '0' + (char) (v % 10);
	while ((v /= 10) != 0);
	while (k < cx->min_digits)
		tmp[k++] = '0';
	for (int i = k - 1; i >= 0; i--)
		out[n++] = tmp[i];
	return n;
}

bool
sift_prefix_typed(const SiftPromoPrefix *cx, const char *s, uint32 len, uint8 tag, int64 *v)
{
	char		back[SIFT_PROMO_PREFIX_MAX + SIFT_PROMO_PREFIX_DIGITS + 1];
	int64		x = 0;

	if (!text_tag(tag) || len <= cx->plen || len - cx->plen > SIFT_PROMO_PREFIX_DIGITS || memcmp(s, cx->prefix, cx->plen) != 0)
		return false;
	for (uint32 i = cx->plen; i < len; i++)
	{
		if (s[i] < '0' || s[i] > '9')
			return false;
		x = x * 10 + (s[i] - '0');
	}
	*v = x;
	return sift_prefix_format(cx, x, back) == len && memcmp(back, s, len) == 0;
}

/* The prefix of a column from its sampled strings: the one that most of a sample spell (each of the first few strings proposes one), false when the rows are not of it */
bool
sift_prefix_derive(const SiftColIn *col, int n, SiftPromoPrefix *cx)
{
	int			nn = 0,
				step,
				sampled = 0,
				best = 0,
				ns = 0;
	int			pick[SIFT_PREFIX_SAMPLE];
	int64		v;

	for (int i = 0; i < n; i++)
		nn += col->isnull == NULL || !col->isnull[i];
	if (nn < SIFT_PREFIX_MIN_ROWS)
		return false;
	step = Max(1, nn / SIFT_PREFIX_SAMPLE);
	for (int i = 0, k = 0; i < n && ns < SIFT_PREFIX_SAMPLE; i++)
	{
		if (col->isnull != NULL && col->isnull[i])
			continue;
		if (k++ % step == 0)
			pick[ns++] = i;
	}
	sampled = ns;
	for (int c = 0; c < Min(ns, SIFT_PREFIX_CANDIDATES); c++)
	{
		SiftPromoPrefix cand;
		int			hit = 0;

		if (!derive(col->s[pick[c]], col->slen[pick[c]], &cand))
			continue;
		for (int k = 0; k < ns; k++)
			hit += sift_prefix_typed(&cand, col->s[pick[k]], col->slen[pick[k]], col->tag ? col->tag[pick[k]] : 0, &v);
		if (hit > best)
		{
			best = hit;
			*cx = cand;
		}
	}
	return sampled > 0 && best * 100 >= sampled * SIFT_PREFIX_CONFORM_PCT;
}

typedef struct Trial
{
	int64	   *v;
	int			nv;
	uint32		xbytes;			/* the text of the rows that are not of the type, and what lists them */
	uint32	   *off;
	char	   *bytes;
	int			ns;
} Trial;

/* The first rows of the column as the numbers, and as the text of all of them: the trial ends at SIFT_PREFIX_TRIAL_BYTES of text, and a longer string is tried by its head */
static void
trial_rows(const SiftColIn *col, int n, const SiftPromoPrefix *cx, Trial *t)
{
	StringInfoData b;
	int64		v;

	t->v = palloc(sizeof(int64) * SIFT_PREFIX_TRIAL_ROWS);
	t->off = palloc(sizeof(uint32) * (SIFT_PREFIX_TRIAL_ROWS + 1));
	initStringInfo(&b);
	t->nv = t->ns = 0;
	t->xbytes = 0;
	t->off[0] = 0;
	for (int i = 0; i < n && t->ns < SIFT_PREFIX_TRIAL_ROWS && b.len < SIFT_PREFIX_TRIAL_BYTES; i++)
	{
		uint32		len;

		if (col->isnull != NULL && col->isnull[i])
			continue;
		len = Min(col->slen[i], SIFT_PREFIX_TRIAL_BYTES);
		if (sift_prefix_typed(cx, col->s[i], col->slen[i], col->tag ? col->tag[i] : 0, &v))
			t->v[t->nv++] = v;
		else
			t->xbytes += len + 6;
		appendBinaryStringInfo(&b, col->s[i], len);
		t->off[++t->ns] = b.len;
	}
	t->bytes = b.data;
}

static Size
ints_size(const int64 *v, int n, const SiftChunkOpts *opt)
{
	StringInfoData s;
	Size		len;

	initStringInfo(&s);
	sift_put_cc_ints(&s, v, n, opt);
	len = s.len;
	pfree(s.data);
	return len;
}

static Size
text_size(const Trial *t, const SiftChunkOpts *opt)
{
	SiftCcPolicy pol;
	StringInfoData s;
	Size		len;

	sift_cc_policy_tier(&pol, opt->tier, opt->alg);
	initStringInfo(&s);
	sift_cc_encode_strs(&s, t->off, t->bytes, t->ns, &pol, NULL);
	len = s.len;
	pfree(s.data);
	return len;
}

/* What a dictionary of the part spends on the rows of the trial: the code of each, as a cascade, and the strings the part keeps once, shared out by rows */
static Size
dict_size(const SiftColIn *col, int n, const SiftDictEnc *d, const Trial *t, const SiftChunkOpts *opt, int total)
{
	int64	   *codes = palloc(sizeof(int64) * Max(t->ns, 1));
	uint64		dbytes = 0;
	int			k = 0;
	Size		sz;

	for (int i = 0; i < n && k < t->ns; i++)
	{
		uint32		lo = 0,
					hi = d->n;

		if (col->isnull != NULL && col->isnull[i])
			continue;
		while (lo < hi)
		{
			uint32		mid = (lo + hi) / 2;
			uint32		l = Min(d->len[mid], col->slen[i]);
			int			c = memcmp(d->p[mid], col->s[i], l);

			if (c < 0 || (c == 0 && d->len[mid] < col->slen[i]))
				lo = mid + 1;
			else
				hi = mid;
		}
		codes[k++] = lo;
	}
	for (uint32 j = 0; j < d->n; j++)
		dbytes += d->len[j] + 2;
	dbytes = dbytes * (uint64) t->ns / Max(total, 1);
	sz = ints_size(codes, k, opt) + (Size) dbytes;
	pfree(codes);
	return sz;
}

bool
sift_promo_prefix_infer(const SiftColIn *col, int n, const SiftDictEnc *dict, SiftColPromo *out)
{
	SiftPromoPrefix cx;
	SiftChunkOpts opt = {.tier = SIFT_TIER_COLD_L2};
	Trial		t;
	Size		typed,
				alt;
	int			total = 0;

	if (col->kind != SIFT_CELL_STR || col->s == NULL || col->dcode != NULL || col->hint != SIFT_CHINT_AUTO || col->group1 > 0 || !sift_prefix_derive(col, n, &cx))
		return false;
	trial_rows(col, n, &cx, &t);
	for (int i = 0; i < n; i++)
		total += col->isnull == NULL || !col->isnull[i];
	typed = t.nv > 0 ? ints_size(t.v, t.nv, &opt) + t.xbytes : 0;
	alt = dict != NULL ? dict_size(col, n, dict, &t, &opt, total) : text_size(&t, &opt);
	pfree(t.v);
	pfree(t.off);
	pfree(t.bytes);
	if (t.nv * 100 < t.ns * SIFT_PREFIX_CONFORM_PCT || typed * 100 > alt * SIFT_PREFIX_GAIN_PCT)
		return false;
	*out = (SiftColPromo) {.kind = SIFT_PROMO_PREFIX_INT,.orig_tag = SIFT_COLTAG_UNKNOWN};
	return true;
}

/* The head of the body that says the prefix: its length, the width of a padded number, the prefix, padded to 8 bytes */
void
sift_prefix_put_head(StringInfo s, const SiftPromoPrefix *cx)
{
	char		h[8] = {(char) cx->plen, (char) cx->min_digits};

	appendBinaryStringInfo(s, h, 8);
	appendBinaryStringInfo(s, cx->prefix, cx->plen);
	pad8(s);
}

/* Reads the head at b; returns the bytes it takes */
Size
sift_prefix_get_head(const char *b, const char *end, SiftPromoPrefix *cx)
{
	if (end - b < 8)
		SIFT_FAIL_CORRUPT("sift: a prefixed chunk is shorter than its prefix");
	cx->plen = (uint8) b[0];
	cx->min_digits = (uint8) b[1];
	if (cx->plen > SIFT_PROMO_PREFIX_MAX || cx->min_digits > SIFT_PROMO_PREFIX_DIGITS || (uint64) (end - b) < 8 + MAXALIGN((uint64) cx->plen))
		SIFT_FAIL_CORRUPT("sift: the prefix of a chunk is out of range");
	memcpy(cx->prefix, b + 8, cx->plen);
	return 8 + MAXALIGN((uint64) cx->plen);
}
