/*
 * col/dict/order.c - the order of a dictionary: comparing, sorting and searching its strings
 * the sorted view that range and prefix matches search in place
 * the match of a pattern against the strings
 */
#include "core/host.h"
#include "core/cmp.h"
#include "col/dict/dict_int.h"

#define DICT_SORT_AFTER		16
#define DICT_SORT_AFTER_SHARED	1		/* the sorted order of a dictionary that is shared (d->built) is made for the server, not for the backend */

#define DICT_SORT_MIN		1024

/* Bytewise order of dictionary values (the C collation) */
int
sift_cstr_cmp(const char *a, uint32 alen, const char *b, uint32 blen)
{
	int			r = memcmp(a, b, Min(alen, blen));

	return r != 0 ? r : (alen > blen) - (alen < blen);
}

bool
sift_cstr_matches(SiftColOp op, const char *s, uint32 len, const SiftColValue *c)
{
	switch (op)
	{
		case SIFT_COLOP_LT:
			return sift_cstr_cmp(s, len, c->v.s, c->len) < 0;
		case SIFT_COLOP_LE:
			return sift_cstr_cmp(s, len, c->v.s, c->len) <= 0;
		case SIFT_COLOP_GT:
			return sift_cstr_cmp(s, len, c->v.s, c->len) > 0;
		case SIFT_COLOP_GE:
			return sift_cstr_cmp(s, len, c->v.s, c->len) >= 0;
		case SIFT_COLOP_SUFFIX:
			return len >= c->len && memcmp(s + len - c->len, c->v.s, c->len) == 0;
		default:					/* PREFIX */
			return len >= c->len && memcmp(s, c->v.s, c->len) == 0;
	}
}

typedef struct SortEnt
{
	uint64		key;			/* the first 8 bytes, zero padded, most significant first: byte order of the keys is byte order of the strings up to a tie */
	uint32		code;
} SortEnt;

static int
sort_ent_cmp(const void *a, const void *b, void *arg)
{
	const SiftCDict *d = arg;
	uint32		x = ((const SortEnt *) a)->code,
				y = ((const SortEnt *) b)->code;

	return sift_cstr_cmp(d->bytes + d->off[x], cdict_len(d, x), d->bytes + d->off[y], cdict_len(d, y));
}

#define SORT_SMALL	32		/* a group of this many strings or fewer is sorted by comparing whole strings */

/* the key of the 8 bytes of string c from at, zero padded: byte order of the keys is byte order of the strings from at, up to a tie */
static uint64
sort_key(const SiftCDict *d, uint32 c, uint32 at)
{
	const uint8 *s = (const uint8 *) d->bytes + d->off[c];
	uint32		len = cdict_len(d, c);
	uint64		k = 0;

	for (uint32 i = 0; i < sizeof(uint64); i++)
		k = k << 8 | (at + i < len ? s[at + i] : 0);
	return k;
}

/*
 * The strings of a[0..n) all begin with the same depth bytes: sorted by a radix sort on the 8 bytes after the prefix they still share (the strings of a column
 * of urls, paths or prefixed ids share long ones), then each run that ties on them by the 8 bytes after, until it is small or no string goes on; b is scratch.
 */
static void
sort_group(const SiftCDict *d, SortEnt *a, SortEnt *b, uint32 n, uint32 depth)
{
	const uint8 *s0;
	uint32		len0,
				share,
				maxlen = 0,
				at;
	SortEnt    *src = a,
			   *dst = b;

	if (n <= SORT_SMALL)
	{
		qsort_arg(a, n, sizeof(SortEnt), sort_ent_cmp, (void *) d);
		return;
	}
	s0 = (const uint8 *) d->bytes + d->off[a[0].code];
	len0 = cdict_len(d, a[0].code);
	share = len0 > depth ? len0 - depth : 0;
	for (uint32 i = 0; i < n; i++)
	{
		const uint8 *s = (const uint8 *) d->bytes + d->off[a[i].code];
		uint32		len = cdict_len(d, a[i].code),
					k = 0;

		maxlen = Max(maxlen, len);
		while (k < share && depth + k < len && s[depth + k] == s0[depth + k])
			k++;
		share = k;
	}
	at = depth + share;
	if (at >= maxlen)
	{
		qsort_arg(a, n, sizeof(SortEnt), sort_ent_cmp, (void *) d);
		return;
	}
	for (uint32 i = 0; i < n; i++)
		a[i].key = sort_key(d, a[i].code, at);
	for (int pass = 0; pass < 8; pass++)
	{
		uint32		count[256] = {0},
					pos = 0;
		int			shift = pass * 8;

		for (uint32 i = 0; i < n; i++)
			count[(src[i].key >> shift) & 255]++;
		if (count[(src[0].key >> shift) & 255] == n)
			continue;
		for (int v = 0; v < 256; v++)
		{
			uint32		c = count[v];

			count[v] = pos;
			pos += c;
		}
		for (uint32 i = 0; i < n; i++)
			dst[count[(src[i].key >> shift) & 255]++] = src[i];
		{
			SortEnt    *t = src;

			src = dst;
			dst = t;
		}
	}
	if (src != a)
		memcpy(a, src, n * sizeof(SortEnt));
	for (uint32 i = 0, j; i < n; i = j)
	{
		for (j = i + 1; j < n && a[j].key == a[i].key; j++)
			;
		if (j - i > 1)
			sort_group(d, a + i, b + i, j - i, at + sizeof(uint64));
	}
}

/* d->sorted: the codes of the strings in byte order */
void
cdict_sort(SiftCDict *d)
{
	SortEnt    *a = palloc(d->n * sizeof(SortEnt)),
			   *b = palloc(d->n * sizeof(SortEnt));

	for (uint32 c = 0; c < d->n; c++)
		a[c].code = c;
	sort_group(d, a, b, d->n, 0);
	d->sorted = MemoryContextAlloc(d->mcxt, (Size) d->n * sizeof(uint32));
	for (uint32 i = 0; i < d->n; i++)
		d->sorted[i] = a[i].code;
	pfree(a);
	pfree(b);
	if (d->built)
		d->built(d, d->built_arg);
}

/* The first position of the sorted order whose string is not below c (after: not at or below it) */
uint32
sift_csorted_bound(const SiftCSorted *s, const SiftColValue *c, bool after)
{
	uint32		lo = 0,
				hi = s->n;

	SIFT_BISECT(uint32, m, lo, hi,
				after ? sift_csorted_cmp(s, m, c) <= 0 : sift_csorted_cmp(s, m, c) < 0);
	return lo;
}

/* The positions [from, to) of the sorted order whose strings pass op (LT, LE, GT, GE in byte order, PREFIX) against c */
void
sift_csorted_range(const SiftCSorted *s, SiftColOp op, const SiftColValue *c, uint32 *from, uint32 *to)
{
	switch (op)
	{
		case SIFT_COLOP_LT:
			*from = 0;
			*to = sift_csorted_bound(s, c, false);
			break;
		case SIFT_COLOP_LE:
			*from = 0;
			*to = sift_csorted_bound(s, c, true);
			break;
		case SIFT_COLOP_GT:
			*from = sift_csorted_bound(s, c, true);
			*to = s->n;
			break;
		case SIFT_COLOP_GE:
			*from = sift_csorted_bound(s, c, false);
			*to = s->n;
			break;
		default:					/* PREFIX: the strings that start with c are the run that begins where c would be inserted */
			{
				uint32		lo = sift_csorted_bound(s, c, false),
							hi = s->n;

				*from = lo;
				SIFT_BISECT(uint32, m, lo, hi, sift_csorted_prefix(s, m, c));
				*to = lo;
			}
	}
}

static const char *
mem_str(const void *ctx, uint32 pos, uint32 *len)
{
	const SiftCDict *d = ctx;

	return sift_cdict_str(d, d->sorted[pos], len);
}

/*
 * The strings of bytes/off[0..n) that hold the needle c[0] (CONTAINS), or whose lower() does (ICONTAINS: the ASCII letters of the strings compared in lower case;
 * the other characters have a lower case that is not ASCII, but for U+0130 and U+212A, which a test of nvals 3 spells as c[1] and c[2] before the compare).
 */
void
sift_cstrs_contains(uint64 *out, const char *bytes, const uint32 *off, uint32 n, SiftColOp op, const SiftColValue *c, int nvals)
{
	uint32		nw = (n + 63) / 64;
	uint64	   *sp;
	char	   *buf = NULL;
	Size		cap = 0;

	sift_colk->str_contains(out, bytes, off, n, c->v.s, c->len, op == SIFT_COLOP_ICONTAINS);
	if (nvals < 3)
		return;
	sp = palloc(2 * nw * sizeof(uint64));
	sift_colk->str_contains(sp, bytes, off, n, "\xC4\xB0", 2, false);
	sift_colk->str_contains(sp + nw, bytes, off, n, "\xE2\x84\xAA", 3, false);
	for (uint32 w = 0; w < nw; w++)
		for (uint64 m = sp[w] | sp[nw + w]; m; m &= m - 1)
		{
			uint32		s = w * 64 + pg_rightmost_one_pos64(m),
						len = off[s + 1] - off[s],
						k = 0;
			const uint8 *p = (const uint8 *) bytes + off[s];

			if (len * Max(c[1].len, c[2].len) + 1 > cap)
			{
				cap = len * Max(c[1].len, c[2].len) + 1;
				buf = buf ? repalloc(buf, cap) : palloc(cap);
			}
			for (uint32 i = 0; i < len;)
				if (i + 1 < len && p[i] == 0xC4 && p[i + 1] == 0xB0)
				{
					memcpy(buf + k, c[1].v.s, c[1].len);
					k += c[1].len;
					i += 2;
				}
				else if (i + 2 < len && p[i] == 0xE2 && p[i + 1] == 0x84 && p[i + 2] == 0xAA)
				{
					memcpy(buf + k, c[2].v.s, c[2].len);
					k += c[2].len;
					i += 3;
				}
				else
					buf[k++] = p[i++];
			if (sift_colk_contains1((const uint8 *) buf, k, (const uint8 *) c->v.s, c->len, true))
				out[w] |= UINT64CONST(1) << (s & 63);
			else
				out[w] &= ~(UINT64CONST(1) << (s & 63));
		}
	if (buf)
		pfree(buf);
	pfree(sp);
}

uint32
sift_cdict_match(SiftCDict *d, SiftColOp op, const SiftColValue *c, int nvals, uint64 *lut)
{
	uint32		count = 0,
				from,
				to;

	if (op == SIFT_COLOP_NOTCONTAINS)
	{
		count = sift_cdict_match(d, SIFT_COLOP_CONTAINS, c, nvals, lut);
		for (uint32 w = 0; w < (d->n + 63) / 64; w++)
			lut[w] = ~lut[w];
		if (d->n % 64)
			lut[d->n / 64] &= (UINT64CONST(1) << (d->n % 64)) - 1;
		return d->n - count;
	}
	if (d->look)
		return d->look->match(d->look, op, c, nvals, lut);
	if (op == SIFT_COLOP_CONTAINS || op == SIFT_COLOP_ICONTAINS)
	{
		sift_cstrs_contains(lut, d->bytes, d->off, d->n, op, c, nvals);
		return sift_colk->bits_count(lut, (d->n + 63) / 64);
	}
	if (op != SIFT_COLOP_SUFFIX && d->sorted == NULL && d->n >= DICT_SORT_MIN && d->tests++ >= (d->built != NULL ? DICT_SORT_AFTER_SHARED : DICT_SORT_AFTER))
		cdict_sort(d);
	if (d->sorted == NULL || op == SIFT_COLOP_SUFFIX)
	{
		for (uint32 k = 0; k < d->n; k++)
			if (op == SIFT_COLOP_PREFIX ? cdict_len(d, k) >= c->len && memcmp(SIFT_COL_DICT_STR(d, k), c->v.s, c->len) == 0
				: sift_cstr_matches(op, SIFT_COL_DICT_STR(d, k), cdict_len(d, k), c))
			{
				lut[k >> 6] |= UINT64CONST(1) << (k & 63);
				count++;
			}
		return count;
	}
	sift_csorted_range(&(SiftCSorted) {d->n, mem_str, d}, op, c, &from, &to);
	for (uint32 i = from; i < to; i++)
		lut[d->sorted[i] >> 6] |= UINT64CONST(1) << (d->sorted[i] & 63);
	return to - from;
}

const char *
sift_cdict_str(const SiftCDict *d, uint32 code, uint32 *len)
{
	*len = cdict_len(d, code);
	return d->bytes + d->off[code];
}
