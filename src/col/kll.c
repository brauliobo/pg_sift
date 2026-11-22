/*
 * col/kll.c - the KLL sketch: levels of items of weight 2^h in one array (level 0 at the front, growing downwards into the free space before it), a compaction that keeps every
 * second item of a full level, the merge of two sketches in one pass over their levels, and the stored form. The portable core: core/host.h and the C library only, no global state.
 */
#include "col/kll.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SIFT_KLL_SEED	UINT64_C(0x9E3779B97F4A7C15)
#define NEG_ZERO_BITS	UINT64_C(0x8000000000000000)

static uint64
rng_next(uint64 *st)
{
	uint64		z = (*st += UINT64_C(0x9E3779B97F4A7C15));

	z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
	return z ^ (z >> 31);
}

static int
cmp_double(const void *a, const void *b)
{
	double		x = *(const double *) a,
				y = *(const double *) b;

	return (x > y) - (x < y);
}

static void
copy_items(double *dst, const double *src, uint32 n)
{
	if (n > 0)
		memmove(dst, src, (size_t) n * sizeof(double));
}

static void
sort_items(double *p, uint32 n)
{
	if (n > 1)
		qsort(p, n, sizeof(double), cmp_double);
}

/* Capacity of the level at each depth below the top one: k, then two thirds rounded up, never under SIFT_KLL_MIN_LEVEL_CAP; returns their sum */
static uint32
fill_caps(uint32 k, unsigned nlevels, uint32 *by_depth)
{
	uint32		c = k,
				sum = 0;

	for (unsigned d = 0; d < nlevels; d++)
	{
		by_depth[d] = c > SIFT_KLL_MIN_LEVEL_CAP ? c : SIFT_KLL_MIN_LEVEL_CAP;
		sum += by_depth[d];
		c = (2 * c + 2) / 3;
	}
	return sum;
}

static uint32
total_cap(uint32 k, unsigned nlevels)
{
	uint32		by_depth[SIFT_KLL_MAX_LEVELS];

	return fill_caps(k, nlevels, by_depth);
}

bool
sift_kll_init_seeded(SiftKll *s, uint16 k, uint64 seed, MemoryContext cxt)
{
	if (k < SIFT_KLL_K_MIN || k > SIFT_KLL_K_MAX)
		return false;
	memset(s, 0, sizeof(*s));
	s->cxt = cxt;
	s->k = k;
	s->nlevels = 1;
	s->l0_sorted = true;
	s->rng = SIFT_KLL_SEED ^ seed ^ k;
	s->flips = rng_next(&s->rng);
	s->tcap = total_cap(k, 1);
	s->cap = 64;
	s->items = MemoryContextAlloc(cxt, (size_t) s->cap * sizeof(double));
	s->lvl[0] = s->lvl[1] = s->cap;
	return true;
}

bool
sift_kll_init(SiftKll *s, uint16 k, MemoryContext cxt)
{
	return sift_kll_init_seeded(s, k, 0, cxt);
}

void
sift_kll_free(SiftKll *s)
{
	pfree(s->items);
	if (s->tmp != NULL)
		pfree(s->tmp);
	s->items = s->tmp = NULL;
	s->cap = s->tmp_cap = 0;
}

uint32
sift_kll_size(const SiftKll *s)
{
	return s->lvl[s->nlevels] - s->lvl[0];
}

uint64
sift_kll_weight(const SiftKll *s)
{
	uint64		w = 0;

	for (unsigned h = 0; h < s->nlevels; h++)
		w += (uint64) (s->lvl[h + 1] - s->lvl[h]) << h;
	return w;
}

/* A larger array with the items at its end, so level 0 has room to grow downwards */
static void
regrow(SiftKll *s, uint32 new_cap)
{
	double	   *p = MemoryContextAlloc(s->cxt, (size_t) new_cap * sizeof(double));
	uint32		used = sift_kll_size(s),
				delta = new_cap - s->cap;

	copy_items(p + new_cap - used, s->items + s->lvl[0], used);
	pfree(s->items);
	s->items = p;
	s->cap = new_cap;
	for (unsigned h = 0; h <= s->nlevels; h++)
		s->lvl[h] += delta;
}

static double *
scratch(SiftKll *s, uint32 n)
{
	if (n > s->tmp_cap)
	{
		if (s->tmp != NULL)
			pfree(s->tmp);
		s->tmp_cap = n + n / 2 + 16;
		s->tmp = MemoryContextAlloc(s->cxt, (size_t) s->tmp_cap * sizeof(double));
	}
	return s->tmp;
}

/*
 * Level h keeps its first item when it has an odd count and hands every second one of the rest, from an even or odd place by the generator, to level h + 1 (made when it is the top):
 * the lower levels move up by the number of items that left, the merged level h + 1 is written ahead of its old items.
 */
static void
compact_level(SiftKll *s, unsigned h)
{
	double	   *it;
	uint32		lo = s->lvl[h],
				sz = s->lvl[h + 1] - lo,
				left = sz & 1,
				count = sz >> 1,
				par = (uint32) (s->flips >> h) & 1,
				up_lo,
				up_sz,
				w,
				p = 0,
				u = 0;
	double		keep,
			   *picks;

	s->flips ^= (uint64) 1 << h;
	if (h == 0 && !s->l0_sorted)
		sort_items(s->items + lo, sz);
	if (h + 1 == s->nlevels)
	{
		s->nlevels++;
		s->lvl[s->nlevels] = s->lvl[h + 1];
		s->tcap = total_cap(s->k, s->nlevels);
	}
	picks = scratch(s, count);
	it = s->items;
	keep = left ? it[lo] : 0;
	for (uint32 j = 0; j < count; j++)
		picks[j] = it[lo + left + par + 2 * j];
	up_lo = s->lvl[h + 1];
	up_sz = s->lvl[h + 2] - up_lo;
	w = up_lo - count;
	while (p < count || u < up_sz)
	{
		bool		take_pick = u == up_sz || (p < count && picks[p] <= it[up_lo + u]);

		it[w++] = take_pick ? picks[p++] : it[up_lo + u++];
	}
	copy_items(it + s->lvl[0] + count, it + s->lvl[0], lo - s->lvl[0]);
	if (left)
		it[lo + count] = keep;
	for (unsigned i = 0; i <= h; i++)
		s->lvl[i] += count;
	s->lvl[h + 1] = lo + count + left;
	if (h == 0)
		s->l0_sorted = true;
}

/* The lowest level at or over its capacity, -1 when none (only the top level of a sketch of SIFT_KLL_MAX_LEVELS levels is left to grow) */
static int
level_to_compact(const SiftKll *s)
{
	uint32		by_depth[SIFT_KLL_MAX_LEVELS];

	unsigned	top = s->nlevels == SIFT_KLL_MAX_LEVELS ? s->nlevels - 1 : s->nlevels;

	fill_caps(s->k, s->nlevels, by_depth);
	for (unsigned h = 0; h < top; h++)
		if (s->lvl[h + 1] - s->lvl[h] >= by_depth[s->nlevels - 1 - h])
			return (int) h;
	return -1;
}

static void
compress(SiftKll *s)
{
	while (sift_kll_size(s) >= s->tcap)
	{
		int			h = level_to_compact(s);

		if (h < 0)
			break;
		compact_level(s, (unsigned) h);
	}
}

static void
note_value(SiftKll *s, double v)
{
	if (s->n == 0)
		s->min = s->max = v;
	else if (v < s->min)
		s->min = v;
	else if (v > s->max)
		s->max = v;
}

bool
sift_kll_add(SiftKll *s, double v)
{
	if (isnan(v))
		return false;
	if (v == 0)
		v = 0.0;
	if (s->lvl[0] == 0)
		regrow(s, s->cap < 32 ? 64 : s->cap * 2);
	note_value(s, v);
	s->l0_sorted = s->l0_sorted && (s->lvl[1] == s->lvl[0] || v <= s->items[s->lvl[0]]);
	s->items[--s->lvl[0]] = v;
	s->n++;
	if (sift_kll_size(s) >= s->tcap)
		compress(s);
	return true;
}

uint32
sift_kll_add_batch(SiftKll *s, const double *vals, const bool *isnull, uint32 n)
{
	uint32		added = 0;

	for (uint32 i = 0; i < n; i++)
		if ((isnull == NULL || !isnull[i]) && sift_kll_add(s, vals[i]))
			added++;
	return added;
}

/* The items of two ascending runs as one */
static void
merge_runs(double *out, const double *a, uint32 na, const double *b, uint32 nb)
{
	uint32		i = 0,
				j = 0;

	while (i < na || j < nb)
		*out++ = j == nb || (i < na && a[i] <= b[j]) ? a[i++] : b[j++];
}

void
sift_kll_merge(SiftKll *dst, const SiftKll *src)
{
	unsigned	nl = dst->nlevels > src->nlevels ? dst->nlevels : src->nlevels;
	uint32		lvl[SIFT_KLL_MAX_LEVELS + 1],
				total = sift_kll_size(dst) + sift_kll_size(src),
				new_cap = total + 32;
	double	   *p;

	if (src->n == 0)
		return;
	p = MemoryContextAlloc(dst->cxt, (size_t) new_cap * sizeof(double));
	lvl[0] = new_cap - total;
	for (unsigned h = 0; h < nl; h++)
	{
		uint32		da = h < dst->nlevels ? dst->lvl[h + 1] - dst->lvl[h] : 0,
					sa = h < src->nlevels ? src->lvl[h + 1] - src->lvl[h] : 0;
		const double *a = da ? dst->items + dst->lvl[h] : NULL,
				   *b = sa ? src->items + src->lvl[h] : NULL;

		if (h == 0)
		{
			copy_items(p + lvl[0], a, da);
			copy_items(p + lvl[0] + da, b, sa);
		}
		else
			merge_runs(p + lvl[h], a, da, b, sa);
		lvl[h + 1] = lvl[h] + da + sa;
	}
	dst->l0_sorted = dst->l0_sorted && src->l0_sorted && (dst->lvl[1] == dst->lvl[0] || src->lvl[1] == src->lvl[0] ||
														  dst->items[dst->lvl[1] - 1] <= src->items[src->lvl[0]]);
	pfree(dst->items);
	dst->items = p;
	dst->cap = new_cap;
	memcpy(dst->lvl, lvl, (nl + 1) * sizeof(uint32));
	if (dst->n == 0)
		dst->min = src->min, dst->max = src->max;
	else
	{
		dst->min = src->min < dst->min ? src->min : dst->min;
		dst->max = src->max > dst->max ? src->max : dst->max;
	}
	dst->n += src->n;
	dst->nlevels = (uint8) nl;
	dst->tcap = total_cap(dst->k, nl);
	compress(dst);
}

bool
sift_kll_merge_wire(SiftKll *dst, const uint8 *blob, uint32 len)
{
	SiftKll		tmp;

	if (!sift_kll_decode(&tmp, blob, len, dst->cxt))
		return false;
	sift_kll_merge(dst, &tmp);
	sift_kll_free(&tmp);
	return true;
}

/* The items with their weights, ascending, and the weight up to and including each; the caller frees */
typedef struct WItem
{
	double		v;
	uint64		w;
} WItem;

static int
cmp_witem(const void *a, const void *b)
{
	return cmp_double(a, b);
}

static WItem *
weighted_view(const SiftKll *s, uint32 *count)
{
	uint32		m = sift_kll_size(s),
				i = 0;
	WItem	   *v = MemoryContextAlloc(s->cxt, ((size_t) m + 1) * sizeof(WItem));
	uint64		run = 0;

	for (unsigned h = 0; h < s->nlevels; h++)
		for (uint32 j = s->lvl[h]; j < s->lvl[h + 1]; j++)
			v[i++] = (WItem) {s->items[j], (uint64) 1 << h};
	if (m > 1)
		qsort(v, m, sizeof(WItem), cmp_witem);
	for (i = 0; i < m; i++)
		v[i].w = run += v[i].w;
	*count = m;
	return v;
}

static void
quantile_of(const SiftKll *s, const WItem *v, uint32 m, double p, double *out)
{
	uint32		lo = 0,
				hi = m;
	double		target = p * (double) s->n;

	if (p == 0)
		*out = s->min;
	else if (p == 1)
		*out = s->max;
	else
	{
		while (lo < hi)
		{
			uint32		mid = lo + (hi - lo) / 2;

			if ((double) v[mid].w >= target)
				hi = mid;
			else
				lo = mid + 1;
		}
		*out = v[lo < m ? lo : m - 1].v;
		*out = *out < s->min ? s->min : (*out > s->max ? s->max : *out);
	}
}

bool
sift_kll_quantiles(const SiftKll *s, const double *ps, uint32 np, double *out)
{
	uint32		m;
	WItem	   *v;

	for (uint32 i = 0; i < np; i++)
		if (!(ps[i] >= 0 && ps[i] <= 1))
			return false;
	if (s->n == 0)
		return false;
	v = weighted_view(s, &m);
	for (uint32 i = 0; i < np; i++)
		quantile_of(s, v, m, ps[i], &out[i]);
	pfree(v);
	return true;
}

bool
sift_kll_quantile(const SiftKll *s, double p, double *out)
{
	return sift_kll_quantiles(s, &p, 1, out);
}

static double
rank_of(const SiftKll *s, const WItem *v, uint32 m, double x)
{
	uint32		lo = 0,
				hi = m;

	if (x < s->min)
		return 0.0;
	if (x >= s->max)
		return 1.0;
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (v[mid].v <= x)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo == 0 ? 0.0 : (double) v[lo - 1].w / (double) s->n;
}

bool
sift_kll_cdf(const SiftKll *s, const double *splits, uint32 ns, double *out)
{
	uint32		m;
	WItem	   *v;

	for (uint32 i = 0; i < ns; i++)
		if (isnan(splits[i]) || (i > 0 && !(splits[i - 1] < splits[i])))
			return false;
	if (s->n == 0)
		return false;
	v = weighted_view(s, &m);
	for (uint32 i = 0; i < ns; i++)
		out[i] = rank_of(s, v, m, splits[i]);
	pfree(v);
	return true;
}

bool
sift_kll_rank(const SiftKll *s, double x, double *out)
{
	return sift_kll_cdf(s, &x, 1, out);
}

bool
sift_kll_dirty_ok(uint64 rows, uint64 dirty, uint32 ppm)
{
	return dirty <= rows / 1000000 * ppm + (rows % 1000000) * ppm / 1000000;
}

/* ---------- the stored form ---------- */

static void
put_le(uint8 *p, uint64 v, int bytes)
{
	for (int i = 0; i < bytes; i++)
		p[i] = (uint8) (v >> (8 * i));
}

static uint64
get_le(const uint8 *p, int bytes)
{
	uint64		v = 0;

	for (int i = 0; i < bytes; i++)
		v |= (uint64) p[i] << (8 * i);
	return v;
}

static uint64
double_bits(double d)
{
	uint64		b;

	memcpy(&b, &d, sizeof(b));
	return b;
}

static double
bits_double(uint64 b)
{
	double		d;

	memcpy(&d, &b, sizeof(d));
	return d;
}

static uint32
crc_update(uint32 crc, const uint8 *p, uint32 len)
{
	for (uint32 i = 0; i < len; i++)
	{
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (UINT32_C(0x82F63B78) & (0 - (crc & 1)));
	}
	return crc;
}

uint32
sift_kll_crc32c(const void *data, uint32 len)
{
	return ~crc_update(~UINT32_C(0), data, len);
}

/* The checksum of a header and what follows it, the checksum field of the header (at crc_off) taken as zero */
static uint32
crc_with_hole(const uint8 *p, uint32 len, uint32 hdr, uint32 crc_off)
{
	uint8		h[SIFT_KLL_WIRE_HDR];
	uint32		crc = ~UINT32_C(0);

	memcpy(h, p, hdr);
	put_le(h + crc_off, 0, 4);
	crc = crc_update(crc, h, hdr);
	return ~crc_update(crc, p + hdr, len - hdr);
}

static uint32
pad8(uint32 n)
{
	return (n + 7) & ~UINT32_C(7);
}

uint32
sift_kll_wire_size(const SiftKll *s)
{
	return SIFT_KLL_WIRE_HDR + pad8(2 * s->nlevels) + 8 * sift_kll_size(s);
}

uint32
sift_kll_encode(const SiftKll *s, uint8 *out, uint32 cap)
{
	uint32		len = sift_kll_wire_size(s),
				nitems = sift_kll_size(s);
	uint8	   *cnt = out + SIFT_KLL_WIRE_HDR,
			   *items = cnt + pad8(2 * s->nlevels);

	if (cap < len)
		return 0;
	memset(out, 0, len);
	put_le(out, SIFT_KLL_MAGIC, 4);
	put_le(out + 4, s->k, 2);
	out[6] = SIFT_KLL_VERSION;
	out[7] = s->nlevels;
	put_le(out + 8, s->n, 8);
	put_le(out + 16, s->n ? double_bits(s->min) : 0, 8);
	put_le(out + 24, s->n ? double_bits(s->max) : 0, 8);
	put_le(out + 32, s->rng, 8);
	put_le(out + 40, s->flips, 8);
	put_le(out + 48, nitems, 4);
	for (unsigned h = 0; h < s->nlevels; h++)
	{
		uint32		c = s->lvl[h + 1] - s->lvl[h];
		double	   *run;

		put_le(cnt + 2 * h, c, 2);
		run = MemoryContextAlloc(s->cxt, ((size_t) c + 1) * sizeof(double));
		copy_items(run, s->items + s->lvl[h], c);
		if (h == 0 && !s->l0_sorted)
			sort_items(run, c);
		for (uint32 i = 0; i < c; i++)
			put_le(items + 8 * i, double_bits(run[i]), 8);
		items += 8 * c;
		pfree(run);
	}
	put_le(out + 52, crc_with_hole(out, len, SIFT_KLL_WIRE_HDR, 52), 4);
	return len;
}

typedef struct WireHdr
{
	uint16		k;
	uint8		nlevels;
	uint64		n;
	double		min,
				max;
	uint64		rng,
				flips;
	uint32		nitems;
	const uint8 *cnt,
			   *items;
} WireHdr;

static bool
sane_double(uint64 bits)
{
	double		d = bits_double(bits);

	return !isnan(d) && bits != NEG_ZERO_BITS;
}

static bool
wire_parse(const uint8 *p, uint32 len, WireHdr *h)
{
	uint64		weight = 0;
	uint32		sum = 0,
				pre;

	if (len < SIFT_KLL_WIRE_HDR || get_le(p, 4) != SIFT_KLL_MAGIC || p[6] != SIFT_KLL_VERSION)
		return false;
	h->k = (uint16) get_le(p + 4, 2);
	h->nlevels = p[7];
	h->n = get_le(p + 8, 8);
	h->rng = get_le(p + 32, 8);
	h->flips = get_le(p + 40, 8);
	h->nitems = (uint32) get_le(p + 48, 4);
	if (h->k < SIFT_KLL_K_MIN || h->k > SIFT_KLL_K_MAX || h->nlevels < 1 || h->nlevels > SIFT_KLL_MAX_LEVELS)
		return false;
	pre = SIFT_KLL_WIRE_HDR + pad8(2 * h->nlevels);
	if (h->nitems >= total_cap(h->k, h->nlevels) || len != pre + 8 * h->nitems)
		return false;
	if (get_le(p + 52, 4) != crc_with_hole(p, len, SIFT_KLL_WIRE_HDR, 52))
		return false;
	h->cnt = p + SIFT_KLL_WIRE_HDR;
	h->items = p + pre;
	for (unsigned l = 0; l < h->nlevels; l++)
	{
		sum += (uint32) get_le(h->cnt + 2 * l, 2);
		weight += get_le(h->cnt + 2 * l, 2) << l;
	}
	for (uint32 i = 2 * h->nlevels; i < pad8(2 * h->nlevels); i++)
		if (h->cnt[i] != 0)
			return false;
	if (sum != h->nitems || weight != h->n || (h->n == 0) != (h->nitems == 0) || (h->n == 0 && h->nlevels != 1))
		return false;
	if (!sane_double(get_le(p + 16, 8)) || !sane_double(get_le(p + 24, 8)))
		return false;
	h->min = bits_double(get_le(p + 16, 8));
	h->max = bits_double(get_le(p + 24, 8));
	if (h->n == 0)
		return get_le(p + 16, 8) == 0 && get_le(p + 24, 8) == 0;
	return h->min <= h->max;
}

static bool
levels_sorted_within(const WireHdr *h)
{
	const uint8 *it = h->items;

	for (unsigned l = 0; l < h->nlevels; l++)
		for (uint32 i = 0, c = (uint32) get_le(h->cnt + 2 * l, 2); i < c; i++, it += 8)
		{
			double		v = bits_double(get_le(it, 8));

			if (!sane_double(get_le(it, 8)) || v < h->min || v > h->max || (i > 0 && v < bits_double(get_le(it - 8, 8))))
				return false;
		}
	return true;
}

bool
sift_kll_check(const uint8 *blob, uint32 len)
{
	WireHdr		h;

	return wire_parse(blob, len, &h) && levels_sorted_within(&h);
}

bool
sift_kll_decode(SiftKll *s, const uint8 *blob, uint32 len, MemoryContext cxt)
{
	WireHdr		h;
	uint32		pos;
	const uint8 *it;

	if (!wire_parse(blob, len, &h) || !levels_sorted_within(&h) || !sift_kll_init(s, h.k, cxt))
		return false;
	pfree(s->items);
	s->nlevels = h.nlevels;
	s->tcap = total_cap(h.k, h.nlevels);
	s->n = h.n;
	s->min = h.min;
	s->max = h.max;
	s->rng = h.rng;
	s->flips = h.flips;
	s->cap = h.nitems + 32;
	s->items = MemoryContextAlloc(cxt, (size_t) s->cap * sizeof(double));
	pos = s->cap - h.nitems;
	it = h.items;
	for (unsigned l = 0; l < h.nlevels; l++)
	{
		uint32		c = (uint32) get_le(h.cnt + 2 * l, 2);

		s->lvl[l] = pos;
		for (uint32 i = 0; i < c; i++, it += 8)
			s->items[pos++] = bits_double(get_le(it, 8));
	}
	s->lvl[h.nlevels] = pos;
	s->l0_sorted = true;
	return true;
}

/* ---------- the section ---------- */

static const uint8 *
sec_entry(const uint8 *sec, uint32 i)
{
	return sec + SIFT_KLL_SEC_HDR + (size_t) i * SIFT_KLL_SEC_ENTRY;
}

uint8 *
sift_kll_sec_build(const SiftKllSecIn *in, uint32 n, MemoryContext cxt, uint32 *len)
{
	uint32		total = SIFT_KLL_SEC_HDR + n * SIFT_KLL_SEC_ENTRY,
				pos = total;
	uint8	   *sec;

	if (n == 0 || n > SIFT_KLL_SEC_MAX_ENTRIES)
		return NULL;
	for (uint32 i = 0; i < n; i++)
		total += pad8(sift_kll_wire_size(in[i].sketch));
	sec = MemoryContextAlloc(cxt, total);
	memset(sec, 0, total);
	put_le(sec, SIFT_KLL_SEC_MAGIC, 4);
	put_le(sec + 4, SIFT_KLL_SEC_VERSION, 2);
	put_le(sec + 6, n, 2);
	put_le(sec + 8, total, 4);
	for (uint32 i = 0; i < n; i++)
	{
		uint8	   *ent = sec + SIFT_KLL_SEC_HDR + (size_t) i * SIFT_KLL_SEC_ENTRY;
		uint32		bl = sift_kll_encode(in[i].sketch, sec + pos, total - pos);

		put_le(ent, in[i].col, 2);
		put_le(ent + 4, in[i].granule, 4);
		put_le(ent + 8, pos, 4);
		put_le(ent + 12, bl, 4);
		put_le(ent + 16, in[i].sketch->n, 8);
		pos += pad8(bl);
	}
	put_le(sec + 12, crc_with_hole(sec, SIFT_KLL_SEC_HDR + n * SIFT_KLL_SEC_ENTRY, SIFT_KLL_SEC_HDR, 12), 4);
	*len = total;
	return sec;
}

bool
sift_kll_sec_check(const uint8 *sec, uint32 len)
{
	uint32		ne,
				dir,
				expect;

	if (len < SIFT_KLL_SEC_HDR || get_le(sec, 4) != SIFT_KLL_SEC_MAGIC || get_le(sec + 4, 2) != SIFT_KLL_SEC_VERSION || get_le(sec + 8, 4) != len)
		return false;
	ne = (uint32) get_le(sec + 6, 2);
	if (ne == 0 || ne > SIFT_KLL_SEC_MAX_ENTRIES)
		return false;
	dir = SIFT_KLL_SEC_HDR + ne * SIFT_KLL_SEC_ENTRY;
	if (len < dir || get_le(sec + 12, 4) != crc_with_hole(sec, dir, SIFT_KLL_SEC_HDR, 12))
		return false;
	expect = dir;
	for (uint32 i = 0; i < ne; i++)
	{
		const uint8 *e = sec_entry(sec, i),
				   *prev = i ? sec_entry(sec, i - 1) : NULL;
		uint64		key = get_le(e, 2) << 32 | get_le(e + 4, 4);
		uint32		off = (uint32) get_le(e + 8, 4),
					bl = (uint32) get_le(e + 12, 4);

		if ((prev != NULL && (get_le(prev, 2) << 32 | get_le(prev + 4, 4)) >= key) || get_le(e + 2, 2) != 0 || off != expect || off > len || bl > len - off ||
			!sift_kll_check(sec + off, bl) || get_le(sec + off + 8, 8) != get_le(e + 16, 8))
			return false;
		for (uint32 j = off + bl; j < off + pad8(bl) && j < len; j++)
			if (sec[j] != 0)
				return false;
		expect = off + pad8(bl);
	}
	return expect == len;
}

uint32
sift_kll_sec_entries(const uint8 *sec)
{
	return (uint32) get_le(sec + 6, 2);
}

bool
sift_kll_sec_find(const uint8 *sec, uint32 len, uint16 col, uint32 granule, const uint8 **blob, uint32 *bloblen, uint64 *n)
{
	uint32		lo = 0,
				hi = sift_kll_sec_entries(sec);
	uint64		want = (uint64) col << 32 | granule;

	(void) len;
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;
		const uint8 *e = sec_entry(sec, mid);
		uint64		key = get_le(e, 2) << 32 | get_le(e + 4, 4);

		if (key == want)
		{
			*blob = sec + get_le(e + 8, 4);
			*bloblen = (uint32) get_le(e + 12, 4);
			*n = get_le(e + 16, 8);
			return true;
		}
		if (key < want)
			lo = mid + 1;
		else
			hi = mid;
	}
	return false;
}

/* ---------- the option ---------- */

static const char *
skip_blank(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

static bool
name_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

/* "(k=N)" after a column name; *k stays as it was when there is none */
static const char *
param_k(const char **pp, uint16 *k)
{
	const char *p = skip_blank(*pp + 1);
	uint32		v = 0;
	int			digits = 0;

	if (p[0] != 'k' || *(p = skip_blank(p + 1)) != '=')
		return "a column takes only the parameter k, as name(k=200)";
	for (p = skip_blank(p + 1); *p >= '0' && *p <= '9'; p++, digits++)
		v = v * 10 + (uint32) (*p - '0');
	p = skip_blank(p);
	if (digits == 0 || digits > 5 || *p != ')')
		return "k must be a number, as name(k=200)";
	if (v < SIFT_KLL_K_MIN || v > SIFT_KLL_K_MAX)
		return "k must be between 64 and 1024";
	*k = (uint16) v;
	*pp = p + 1;
	return NULL;
}

const char *
sift_kll_spec_parse(const char *opt, SiftKllSpecCol *cols, uint32 cap, uint32 *n)
{
	const char *p = skip_blank(opt);

	*n = 0;
	while (*p != '\0')
	{
		const char *kind = p;
		bool		is_kll;

		while (name_char(*p))
			p++;
		if (p == kind)
			return "expected hll(...) or kll(...)";
		is_kll = (p - kind == 3) && strncmp(kind, "kll", 3) == 0;
		if (!is_kll && !((p - kind == 3) && strncmp(kind, "hll", 3) == 0))
			return "a sketch is hll(...) or kll(...)";
		p = skip_blank(p);
		if (*p++ != '(')
			return "a sketch kind is followed by its columns in parentheses";
		for (;;)
		{
			const char *name = skip_blank(p);
			uint16		k = SIFT_KLL_K_DEFAULT;
			uint32		nlen;

			for (p = name; name_char(*p); p++)
				;
			if (p == name)
				return "expected a column name";
			nlen = (uint32) (p - name);
			if (*(p = skip_blank(p)) == '(')
			{
				if (is_kll)
				{
					const char *err = param_k(&p, &k);

					if (err)
						return err;
				}
				else
					for (int depth = 0; *p != '\0'; p++)
					{
						depth += *p == '(';
						if (*p == ')' && --depth == 0)
						{
							p++;
							break;
						}
					}
			}
			for (uint32 i = 0; i < *n && i < cap && cols != NULL; i++)
				if (cols[i].kll == is_kll && cols[i].len == nlen && strncmp(cols[i].name, name, nlen) == 0)
					return is_kll ? "a column is listed once in kll(...)" : "a column is listed once in hll(...)";
			if (cols != NULL && *n < cap)
				cols[*n] = (SiftKllSpecCol) {name, nlen, k, is_kll};
			(*n)++;
			p = skip_blank(p);
			if (*p == ',')
			{
				p++;
				continue;
			}
			if (*p++ != ')')
				return "a column list ends with a closing parenthesis";
			break;
		}
		p = skip_blank(p);
		if (*p == ';' || *p == ',')
			p = skip_blank(p + 1);
		else if (*p != '\0')
			return "sketches are separated by a semicolon";
	}
	return NULL;
}
