/*
 * col/granule/cfilt/kern.c - predicates run on the stored form of integer columns (col/granule/cfilt/kern.h).
 */
#include "core/host.h"

#include "core/par/arena.h"
#include "core/cpu.h"
#include "core/fmv.h"
#include "core/cmp.h"
#include "core/popcount.h"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

#include "col/granule/cfilt/kern.h"

#define BLOCK			64

static inline uint64
le64(const uint8 *p)
{
	uint64		v;

	memcpy(&v, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	v = __builtin_bswap64(v);
#endif
	return v;
}

static inline uint64
width_mask(uint32 bits)
{
	return bits >= 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << bits) - 1;
}

/* ---- the test of a value ---- */

bool
sift_cf_test_value(const SiftCfTest *t, int64 v)
{
	bool		in;

	switch (t->kind)
	{
		case SIFT_CFT_NONE:
			return false;
		case SIFT_CFT_ALL:
			return true;
		case SIFT_CFT_MEMBER:
			return v >= 0 && (uint64) v < t->nmember && ((t->member[v >> 6] >> (v & 63)) & 1);
		case SIFT_CFT_RANGE:
		case SIFT_CFT_NRANGE:
			in = t->lo <= t->hi && (uint64) v - (uint64) t->lo <= (uint64) t->hi - (uint64) t->lo;
			return (t->kind == SIFT_CFT_RANGE) == in;
		default:
			{
				uint32		lo = 0,
							hi = t->nset;

				while (lo < hi)
				{
					uint32		mid = lo + (hi - lo) / 2;

					if (t->set[mid] < v)
						lo = mid + 1;
					else
						hi = mid;
				}
				in = lo < t->nset && t->set[lo] == v;
				return (t->kind == SIFT_CFT_SET) == in;
			}
	}
}

static uint32
words_of(uint32 n)
{
	return (n + 63) / 64;
}

/* The bits past the last value cleared, and what the mask holds as the result */
static SiftCfResult
finish(uint64 *out, uint32 n)
{
	uint32		nw = words_of(n),
				cnt = 0;

	if (n == 0)
		return SIFT_CF_FALSE;
	if (n & 63)
		out[nw - 1] &= (UINT64CONST(1) << (n & 63)) - 1;
	for (uint32 w = 0; w < nw; w++)
		cnt += sift_pc64(out[w]);
	return cnt == 0 ? SIFT_CF_FALSE : cnt == n ? SIFT_CF_TRUE : SIFT_CF_BITS;
}

static void
invert(uint64 *out, uint32 n)
{
	for (uint32 w = 0; w < words_of(n); w++)
		out[w] = ~out[w];
}

/* What the least and the greatest value of the chunk say of the test, or BITS where they do not decide it */
static SiftCfResult
bounds_verdict(const SiftCfTest *t, const SiftCfBounds *b)
{
	bool		may_in,
				all_in;

	if (t->kind == SIFT_CFT_NONE)
		return SIFT_CF_FALSE;
	if (t->kind == SIFT_CFT_ALL)
		return SIFT_CF_TRUE;
	if (!b->known || t->kind == SIFT_CFT_MEMBER)
		return SIFT_CF_BITS;
	if (t->kind == SIFT_CFT_RANGE || t->kind == SIFT_CFT_NRANGE)
	{
		may_in = t->lo <= t->hi && t->hi >= b->vmin && t->lo <= b->vmax;
		all_in = t->lo <= t->hi && t->lo <= b->vmin && t->hi >= b->vmax;
	}
	else
	{
		uint32 lo = 0,
			   hi = t->nset;

		SIFT_BISECT(uint32, m, lo, hi, t->set[m] < b->vmin);
		may_in = lo < t->nset && t->set[lo] <= b->vmax;
		all_in = b->vmin == b->vmax && may_in;
	}
	if (t->kind == SIFT_CFT_RANGE || t->kind == SIFT_CFT_SET)
		return !may_in ? SIFT_CF_FALSE : all_in ? SIFT_CF_TRUE : SIFT_CF_BITS;
	return !may_in ? SIFT_CF_TRUE : all_in ? SIFT_CF_FALSE : SIFT_CF_BITS;
}

static bool
is_range(const SiftCfTest *t)
{
	return t->kind == SIFT_CFT_RANGE || t->kind == SIFT_CFT_NRANGE;
}

/* ---- offsets at the widths of an array: vector compares ---- */

#define DEFINE_WIDTH_RANGE(W, T) \
	SIFT_FMV_DEFINE_VOID(static, cf_range_w##W, (const uint8 *SIFT_RESTRICT p, uint32 n, T a, T span, uint64 *SIFT_RESTRICT out), (p, n, a, span, out), \
	{ \
		uint32 nw = (n + 63) / 64; \
		for (uint32 w = 0; w < nw; w++) \
		{ \
			T v[BLOCK]; \
			uint32 cnt = n - w * BLOCK < BLOCK ? n - w * BLOCK : BLOCK; \
			uint64 m = 0; \
			memset(v, 0, sizeof(v)); \
			memcpy(v, p + (size_t) w * BLOCK * sizeof(T), cnt * sizeof(T)); \
			for (uint32 j = 0; j < BLOCK; j++) \
				m |= (uint64) ((T) (v[j] - a) <= span) << j; \
			out[w] = m; \
		} \
	})

DEFINE_WIDTH_RANGE(8, uint8)
DEFINE_WIDTH_RANGE(16, uint16)
DEFINE_WIDTH_RANGE(32, uint32)
DEFINE_WIDTH_RANGE(64, uint64)

#if defined(__x86_64__)
/* Bytes: 32 offsets a compare, the unsigned minimum with the span equal to the difference says it is not above it, a bit for each from the mask of the bytes */
SIFT_TARGET_AVX2 static void
cf_range_w8_avx2(const uint8 *p, uint32 n, uint8 a, uint8 span, uint64 *out)
{
	const __m256i va = _mm256_set1_epi8((char) a),
				vs = _mm256_set1_epi8((char) span);
	uint32		full = n / 64;

	for (uint32 w = 0; w < full; w++)
	{
		uint64		m[2];

		for (int h = 0; h < 2; h++)
		{
			__m256i		d = _mm256_sub_epi8(_mm256_loadu_si256((const __m256i *) (p + (size_t) w * 64 + h * 32)), va);

			m[h] = (uint32) _mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_min_epu8(d, vs), d));
		}
		out[w] = m[0] | m[1] << 32;
	}
	if (n % 64)
	{
		uint8		tmp[64];

		memset(tmp, 0, sizeof(tmp));
		memcpy(tmp, p + (size_t) full * 64, n % 64);
		{
			uint64		m = 0;

			for (uint32 j = 0; j < n % 64; j++)
				m |= (uint64) ((uint8) (tmp[j] - a) <= span) << j;
			out[full] = m;
		}
	}
}
#endif

/* The offsets of one block of cnt values, 64 bits each at most: p is the start of the block, avail the bytes of the chunk from there */
static void
unpack_block(const uint8 *p, size_t avail, uint32 bits, uint32 cnt, uint64 *out)
{
	uint8		tmp[BLOCK * 8 + 32];
	size_t		need = ((size_t) cnt * bits + 7) / 8;
	uint64		mask = width_mask(bits);

	if (avail < need + 16)
	{
		memset(tmp, 0, sizeof(tmp));
		memcpy(tmp, p, need);
		p = tmp;
	}
	if (bits <= 57)
		for (uint32 i = 0; i < cnt; i++)
		{
			size_t		bitpos = (size_t) i * bits;

			out[i] = (le64(p + (bitpos >> 3)) >> (bitpos & 7)) & mask;
		}
	else
		for (uint32 i = 0; i < cnt; i++)
		{
			size_t		bitpos = (size_t) i * bits;
			unsigned __int128 w;

			memcpy(&w, p + (bitpos >> 3), 16);
			out[i] = (uint64) (w >> (bitpos & 7)) & mask;
		}
}

/* The offsets that lie in [a, a + span], as bits: bits 0 is a column of one value, which the caller has answered */
static void
offsets_in_range(const uint8 *packed, uint32 bits, uint32 n, uint64 a, uint64 span, uint64 *out)
{
	switch (bits)
	{
		case 8:
#if defined(__x86_64__)
			if (sift_cpu.active >= SIFT_SIMD_AVX2 && (sift_cpu.flags & SIFT_CPU_AVX2))
			{
				cf_range_w8_avx2(packed, n, (uint8) a, (uint8) span, out);
				return;
			}
#endif
			cf_range_w8(packed, n, (uint8) a, (uint8) span, out);
			return;
		case 16:
			cf_range_w16(packed, n, (uint16) a, (uint16) span, out);
			return;
		case 32:
			cf_range_w32(packed, n, (uint32) a, (uint32) span, out);
			return;
		case 64:
			cf_range_w64(packed, n, a, span, out);
			return;
		default:
			break;
	}
	{
		size_t		total = ((size_t) n * bits + 7) / 8;

		for (uint32 w = 0; w < words_of(n); w++)
		{
			uint64		u[BLOCK],
						m = 0;
			uint32		cnt = Min(BLOCK, n - w * BLOCK);
			size_t		off = (size_t) w * bits * 8;

			unpack_block(packed + off, total - off, bits, cnt, u);
			for (uint32 j = 0; j < cnt; j++)
				m |= (uint64) (u[j] - a <= span) << j;
			out[w] = m;
		}
	}
}

/* ---- frame of reference ---- */

/* The range test in the space of the offsets from base: BITS with the window [*a, *a + *span] of offsets that pass, else the verdict the window gives of every value */
static SiftCfResult
offset_window(const SiftCfTest *t, int64 base, uint64 maxoff, uint64 *a, uint64 *span)
{
	__int128	lo = (__int128) t->lo - base,
				hi = (__int128) t->hi - base;
	bool		in = t->kind == SIFT_CFT_RANGE;

	if (lo < 0)
		lo = 0;
	if (hi > (__int128) maxoff)
		hi = maxoff;
	if (t->lo > t->hi || lo > hi)
		return in ? SIFT_CF_FALSE : SIFT_CF_TRUE;
	if (lo == 0 && hi == (__int128) maxoff)
		return in ? SIFT_CF_TRUE : SIFT_CF_FALSE;
	*a = (uint64) lo;
	*span = (uint64) (hi - lo);
	return SIFT_CF_BITS;
}

#define SET_BITMAP_BITS		(1 << 18)

/* The keys of a set that a chunk of offsets from base up to maxoff can hold, as a bitmap of the offsets (a bit for each of 0 .. *lim): NULL for a test that is no set, or offsets too many for one */
static uint64 *
set_bitmap(const SiftCfTest *t, int64 base, uint64 maxoff, const SiftCfBounds *range, uint64 *lim)
{
	uint64		top = range->known && (uint64) range->vmax - (uint64) base < maxoff ? (uint64) range->vmax - (uint64) base : maxoff;
	uint64	   *bm;

	if ((t->kind != SIFT_CFT_SET && t->kind != SIFT_CFT_NSET) || top >= SET_BITMAP_BITS)
		return NULL;
	bm = sift_alloc0((top / 64 + 1) * sizeof(uint64));
	for (uint32 i = 0; i < t->nset; i++)
	{
		uint64		off = (uint64) t->set[i] - (uint64) base;

		if (off <= top)
			bm[off >> 6] |= UINT64CONST(1) << (off & 63);
	}
	*lim = top;
	return bm;
}

static inline bool
bitmap_has(const uint64 *bm, uint64 lim, uint64 o)
{
	return o <= lim && ((bm[o >> 6] >> (o & 63)) & 1);
}

SiftCfResult
sift_cf_for(const SiftCfFor *c, const SiftCfTest *t, uint64 *out)
{
	SiftCfResult res;

	if (c->bits > 64)
		return SIFT_CF_UNSUPPORTED;
	if (c->n == 0)
		return SIFT_CF_FALSE;
	res = bounds_verdict(t, &c->range);
	if (res != SIFT_CF_BITS)
		return res;
	if (c->bits == 0)
		return sift_cf_test_value(t, c->base) ? SIFT_CF_TRUE : SIFT_CF_FALSE;
	memset(out, 0, words_of(c->n) * sizeof(uint64));
	if (is_range(t))
	{
		uint64		a,
					span;

		res = offset_window(t, c->base, width_mask(c->bits), &a, &span);
		if (res != SIFT_CF_BITS)
			return res;
		offsets_in_range(c->packed, c->bits, c->n, a, span, out);
		if (t->kind == SIFT_CFT_NRANGE)
			invert(out, c->n);
		return finish(out, c->n);
	}
	{
		size_t		total = ((size_t) c->n * c->bits + 7) / 8;
		uint64		lim = 0;
		uint64	   *bm = set_bitmap(t, c->base, width_mask(c->bits), &c->range, &lim);

		for (uint32 w = 0; w < words_of(c->n); w++)
		{
			uint64		u[BLOCK],
						m = 0;
			uint32		cnt = Min(BLOCK, c->n - w * BLOCK);
			size_t		off = (size_t) w * c->bits * 8;

			unpack_block(c->packed + off, total - off, c->bits, cnt, u);
			if (bm != NULL)
				for (uint32 j = 0; j < cnt; j++)
					m |= (uint64) bitmap_has(bm, lim, u[j]) << j;
			else
				for (uint32 j = 0; j < cnt; j++)
					m |= (uint64) sift_cf_test_value(t, (int64) ((uint64) c->base + u[j])) << j;
			out[w] = m;
		}
		if (bm != NULL)
		{
			sift_free(bm);
			if (t->kind == SIFT_CFT_NSET)
				invert(out, c->n);
		}
	}
	return finish(out, c->n);
}

/* ---- delta ---- */

SiftCfResult
sift_cf_delta(const SiftCfDelta *c, const SiftCfTest *t, uint64 *out)
{
	SiftCfResult res;
	uint64		v;
	size_t		total;
	uint32		nd;

	if (c->bits > 64)
		return SIFT_CF_UNSUPPORTED;
	if (c->n == 0)
		return SIFT_CF_FALSE;
	res = bounds_verdict(t, &c->range);
	if (res != SIFT_CF_BITS)
		return res;
	memset(out, 0, words_of(c->n) * sizeof(uint64));
	v = (uint64) c->first;
	if (sift_cf_test_value(t, c->first))
		out[0] |= 1;
	nd = c->n - 1;
	total = ((size_t) nd * c->bits + 7) / 8;
	for (uint32 blk = 0; blk * BLOCK < nd; blk++)
	{
		uint64		u[BLOCK];
		uint32		cnt = Min(BLOCK, nd - blk * BLOCK);
		size_t		off = (size_t) blk * c->bits * 8;

		unpack_block(c->packed + off, total - off, c->bits, cnt, u);
		for (uint32 j = 0; j < cnt; j++)
		{
			uint32		i = blk * BLOCK + j + 1;

			v += c->monotone ? u[j] : (u[j] >> 1) ^ (~(u[j] & 1) + 1);
			if (c->monotone && t->kind == SIFT_CFT_RANGE && (int64) v > t->hi)
				return finish(out, c->n);
			if (sift_cf_test_value(t, (int64) v))
				out[i >> 6] |= UINT64CONST(1) << (i & 63);
		}
	}
	return finish(out, c->n);
}

/* ---- run length ---- */

static void
set_bits(uint64 *out, uint32 pos, uint32 len)
{
	uint32		end = pos + len;

	while (pos < end)
	{
		uint32		w = pos >> 6,
					lo = pos & 63,
					k = Min(end - pos, 64 - lo);
		uint64		m = (k == 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << k) - 1) << lo;

		out[w] |= m;
		pos += k;
	}
}

SiftCfResult
sift_cf_rle(const SiftCfRle *c, const SiftCfTest *t, uint64 *out)
{
	uint64		sum = 0;
	uint32		pos = 0;

	for (uint32 r = 0; r < c->nruns; r++)
		sum += c->lens[r];
	if (sum != c->n)
		return SIFT_CF_UNSUPPORTED;
	if (c->n == 0)
		return SIFT_CF_FALSE;
	memset(out, 0, words_of(c->n) * sizeof(uint64));
	for (uint32 r = 0; r < c->nruns; r++)
	{
		if (sift_cf_test_value(t, c->vals[r]))
			set_bits(out, pos, c->lens[r]);
		pos += c->lens[r];
	}
	return finish(out, c->n);
}

/* ---- dictionary ---- */

uint32
sift_cf_dict_match(const int64 *dict, uint32 n, const SiftCfTest *t, uint64 *match)
{
	uint32		cnt = 0;

	memset(match, 0, words_of(n) * sizeof(uint64));
	for (uint32 i = 0; i < n; i++)
		if (sift_cf_test_value(t, dict[i]))
		{
			match[i >> 6] |= UINT64CONST(1) << (i & 63);
			cnt++;
		}
	return cnt;
}

/* How many codes the match bits set, and the least and the greatest of them. Every code of the chunk matches only when the match covers all that its width can hold: a code past the dictionary matches none */
static uint32
match_summary(const uint64 *match, uint32 dict_n, uint32 *first, uint32 *last)
{
	uint32		k = 0;

	*first = dict_n;
	*last = 0;
	for (uint32 w = 0, nw = words_of(dict_n); w < nw; w++)
	{
		uint64 word = match[w];

		if (w + 1 == nw && (dict_n & 63))
			word &= (UINT64CONST(1) << (dict_n & 63)) - 1;
		if (word == 0)
			continue;
		if (k == 0)
			*first = (w << 6) + (uint32) __builtin_ctzll(word);
		*last = (w << 6) + (uint32) (63 - __builtin_clzll(word));
		k += sift_pc64(word);
	}
	return k;
}

static bool
code_matches(const uint64 *match, uint32 dict_n, uint64 code)
{
	return code < dict_n && ((match[code >> 6] >> (code & 63)) & 1);
}

/* The first entry of the ascending dict that is not below v (strict: above v) */
static uint32
dict_bound(const int64 *dict, uint32 n, int64 v, bool strict)
{
	uint32		lo = 0,
				hi = n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (strict ? dict[mid] <= v : dict[mid] < v)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

uint32
sift_cf_dict_match_sorted(const int64 *dict, uint32 n, const SiftCfTest *t, uint64 *match)
{
	uint32		from = 0,
				to = 0,
				cnt = 0;

	memset(match, 0, words_of(n) * sizeof(uint64));
	switch (t->kind)
	{
		case SIFT_CFT_NONE:
			return 0;
		case SIFT_CFT_ALL:
			set_bits(match, 0, n);
			return n;
		case SIFT_CFT_RANGE:
		case SIFT_CFT_NRANGE:
			if (t->lo <= t->hi)
			{
				from = dict_bound(dict, n, t->lo, false);
				to = dict_bound(dict, n, t->hi, true);
			}
			if (t->kind == SIFT_CFT_RANGE)
				set_bits(match, from, to - from);
			else
			{
				set_bits(match, 0, from);
				set_bits(match, to, n - to);
			}
			return t->kind == SIFT_CFT_RANGE ? to - from : n - (to - from);
		case SIFT_CFT_MEMBER:
			return sift_cf_dict_match(dict, n, t, match);
		default:
			if (n < 64 || t->nset > n / 8)
			{
				for (uint32 di = 0, si = 0; di < n && si < t->nset;)
				{
					if (dict[di] < t->set[si])
						di++;
					else if (dict[di] > t->set[si])
						si++;
					else
					{
						match[di >> 6] |= UINT64CONST(1) << (di & 63);
						cnt++;
						di++;
						si++;
					}
				}
			}
			else
			{
				uint32 base = 0;

				for (uint32 i = 0; i < t->nset && base < n; i++)
				{
					uint32 at = base + dict_bound(dict + base, n - base, t->set[i], false);

					base = at;
					if (at < n && dict[at] == t->set[i] && !((match[at >> 6] >> (at & 63)) & 1))
					{
						match[at >> 6] |= UINT64CONST(1) << (at & 63);
						cnt++;
					}
				}
			}
			if (t->kind == SIFT_CFT_NSET)
			{
				for (uint32 w = 0; w < words_of(n); w++)
					match[w] = ~match[w];
				if (n & 63)
					match[words_of(n) - 1] &= (UINT64CONST(1) << (n & 63)) - 1;
				return n - cnt;
			}
			return cnt;
	}
}

SiftCfResult
sift_cf_dict(const SiftCfDict *c, uint64 *out)
{
	uint32		k,
				first,
				last;
	size_t		total;

	if (c->bits > 32 || (c->dict_n == 0 && c->n > 0) || (c->bits < 32 && c->dict_n > (UINT64CONST(1) << c->bits)))
		return SIFT_CF_UNSUPPORTED;
	if (c->n == 0)
		return SIFT_CF_FALSE;
	k = match_summary(c->match, c->dict_n, &first, &last);
	if (k == 0)
		return SIFT_CF_FALSE;
	if (k == c->dict_n && c->bits < 32 && c->dict_n >= (UINT64CONST(1) << c->bits))
		return SIFT_CF_TRUE;
	memset(out, 0, words_of(c->n) * sizeof(uint64));
	if (c->bits == 0)
		return (c->match[0] & 1) ? SIFT_CF_TRUE : SIFT_CF_FALSE;
	if (last - first + 1 == k)
	{
		offsets_in_range(c->packed, c->bits, c->n, first, last - first, out);
		return finish(out, c->n);
	}
	total = ((size_t) c->n * c->bits + 7) / 8;
	for (uint32 w = 0; w < words_of(c->n); w++)
	{
		uint64		u[BLOCK],
					m = 0;
		uint32		cnt = Min(BLOCK, c->n - w * BLOCK);
		size_t		off = (size_t) w * c->bits * 8;

		unpack_block(c->packed + off, total - off, c->bits, cnt, u);
		for (uint32 j = 0; j < cnt; j++)
			m |= (uint64) code_matches(c->match, c->dict_n, u[j]) << j;
		out[w] = m;
	}
	return finish(out, c->n);
}

/* ---- the vertical layout of the codec ---- */

#define VROWS			32
#define VBLOCK			(VROWS * 32)

/* `rows` rows of 32 values of width W (1 .. 32), a row at a time: the 32 values of a row sit in the same bits of 32 consecutive words (or, across a word, of two lots of them), so each row is one shift of a vector */
SIFT_FMV_DEFINE_VOID(static, cf_unpack_rows, (const uint8 *SIFT_RESTRICT src, uint32 W, uint32 rows, uint32 *SIFT_RESTRICT dst), (src, W, rows, dst),
{
	uint32 mask = W >= 32 ? ~0u : (1u << W) - 1;
	for (uint32 r = 0; r < rows; r++)
	{
		uint32 bp = r * W;
		uint32 sh = bp & 31;
		const uint8 *a = src + (size_t) (bp >> 5) * 128;
		uint32 *d = dst + (size_t) r * 32;
		if (sh + W > 32)
		{
			const uint8 *b = a + 128;
			for (uint32 l = 0; l < 32; l++)
			{
				uint32 x, y;
				memcpy(&x, a + 4 * l, 4);
				memcpy(&y, b + 4 * l, 4);
				d[l] = ((x >> sh) | (y << (32 - sh))) & mask;
			}
		}
		else
			for (uint32 l = 0; l < 32; l++)
			{
				uint32 x;
				memcpy(&x, a + 4 * l, 4);
				d[l] = (x >> sh) & mask;
			}
	}
})

/* Block blk of a chunk of n values: how many values it holds, now in dst as u32 */
static uint32
unpack_vblock(const uint8 *words, uint32 W, uint32 n, uint32 blk, uint32 *dst)
{
	uint32		cnt = Min(VBLOCK, n - blk * VBLOCK);

	cf_unpack_rows(words + (size_t) blk * VROWS * W * 4, W, (cnt + 31) / 32, dst);
	return cnt;
}

SiftCfResult
sift_cf_for_vert(const SiftCfForVert *c, const SiftCfTest *t, uint64 *out)
{
	SiftCfResult res;
	uint32		tmp[VBLOCK];
	uint64		a = 0,
				span = 0,
				lim = 0,
			   *bm;

	if (c->bits < 1 || c->bits > 32)
		return SIFT_CF_UNSUPPORTED;
	if (c->n == 0)
		return SIFT_CF_FALSE;
	res = bounds_verdict(t, &c->range);
	if (res != SIFT_CF_BITS)
		return res;
	if (is_range(t))
	{
		res = offset_window(t, c->base, width_mask(c->bits), &a, &span);
		if (res != SIFT_CF_BITS)
			return res;
	}
	bm = set_bitmap(t, c->base, width_mask(c->bits), &c->range, &lim);
	memset(out, 0, words_of(c->n) * sizeof(uint64));
	for (uint32 blk = 0; blk * VBLOCK < c->n; blk++)
	{
		uint32		cnt = unpack_vblock(c->words, c->bits, c->n, blk, tmp);

		if (is_range(t))
			cf_range_w32((const uint8 *) tmp, cnt, (uint32) a, (uint32) span, out + blk * (VBLOCK / 64));
		else if (bm != NULL)
			for (uint32 j = 0; j < cnt; j++)
				out[(blk * VBLOCK + j) >> 6] |= (uint64) bitmap_has(bm, lim, tmp[j]) << (j & 63);
		else
			for (uint32 j = 0; j < cnt; j++)
				out[(blk * VBLOCK + j) >> 6] |= (uint64) sift_cf_test_value(t, (int64) ((uint64) c->base + tmp[j])) << (j & 63);
	}
	if (t->kind == SIFT_CFT_NRANGE || (bm != NULL && t->kind == SIFT_CFT_NSET))
		invert(out, c->n);
	if (bm != NULL)
		sift_free(bm);
	return finish(out, c->n);
}

SiftCfResult
sift_cf_dict_vert(const SiftCfDictVert *c, uint64 *out)
{
	uint32		k,
				first,
				last,
				tmp[VBLOCK];

	if (c->bits < 1 || c->bits > 32 || c->dict_n == 0 || (c->bits < 32 && c->dict_n > (UINT64CONST(1) << c->bits)))
		return SIFT_CF_UNSUPPORTED;
	if (c->n == 0)
		return SIFT_CF_FALSE;
	k = match_summary(c->match, c->dict_n, &first, &last);
	if (k == 0)
		return SIFT_CF_FALSE;
	if (k == c->dict_n && c->bits < 32 && c->dict_n >= (UINT64CONST(1) << c->bits))
		return SIFT_CF_TRUE;
	if (last - first + 1 == k)
		return sift_cf_for_vert(&(SiftCfForVert) {.base = 0,.bits = c->bits,.n = c->n,.words = c->words},
								&(SiftCfTest) {.kind = SIFT_CFT_RANGE,.lo = first,.hi = last}, out);
	memset(out, 0, words_of(c->n) * sizeof(uint64));
	for (uint32 blk = 0; blk * VBLOCK < c->n; blk++)
	{
		uint32		cnt = unpack_vblock(c->words, c->bits, c->n, blk, tmp);

		for (uint32 j = 0; j < cnt; j++)
			out[(blk * VBLOCK + j) >> 6] |= (uint64) code_matches(c->match, c->dict_n, tmp[j]) << (j & 63);
	}
	return finish(out, c->n);
}

/* The vertical delta: block b holds the values from 1024 * b on, the first of which is kept (the checkpoints), and each one is the one before it plus step plus its stored difference */
SiftCfResult
sift_cf_delta_vert(const SiftCfDeltaVert *c, const SiftCfTest *t, uint64 *out)
{
	SiftCfResult res;
	uint32		nblks = (c->n + VBLOCK - 1) / VBLOCK,
				tmp[VBLOCK];
	int64	   *first;
	bool		asc = c->step >= 0,
				desc = !asc && c->bits < 32 && c->step + (int64) ((UINT64CONST(1) << c->bits) - 1) <= 0,
				directed;
	uint64		span = (uint64) t->hi - (uint64) t->lo,
				lim = 0,
			   *bm = NULL;
	bool		fast = is_range(t) && t->lo <= t->hi;

	if (c->bits > 32)
		return SIFT_CF_UNSUPPORTED;
	if (c->n == 0)
		return SIFT_CF_FALSE;
	res = bounds_verdict(t, &c->range);
	if (res != SIFT_CF_BITS)
		return res;
	first = sift_alloc(nblks * sizeof(int64));
	first[0] = c->first;
	directed = asc || desc;
	for (uint32 b = 1; b < nblks; b++)
	{
		memcpy(&first[b], c->check + 8 * (size_t) (b - 1), 8);
		directed = directed && (asc ? first[b] >= first[b - 1] : first[b] <= first[b - 1]);
	}
	if (c->range.known && t->kind == SIFT_CFT_SET)
		bm = set_bitmap(t, c->range.vmin, (uint64) c->range.vmax - (uint64) c->range.vmin, &c->range, &lim);
	memset(out, 0, words_of(c->n) * sizeof(uint64));
	for (uint32 b = 0; b < nblks; b++)
	{
		uint32		from = b * VBLOCK,
					cnt = Min(VBLOCK, c->n - from);
		bool		last = b + 1 == nblks;
		__int128	reach = (__int128) (cnt - 1) * (asc ? c->step + (int64) ((UINT64CONST(1) << c->bits) - 1) : c->step),
					end = (__int128) first[b] + reach;
		bool		bounded = directed && (!last || (end >= INT64_MIN && end <= INT64_MAX));
		uint64		run = (uint64) first[b];

		if (bounded)
		{
			int64		lo_end = asc ? first[b] : (last ? (int64) end : first[b + 1]),
						hi_end = asc ? (last ? (int64) end : first[b + 1]) : first[b];
			SiftCfBounds blk = {.known = true,.vmin = lo_end,.vmax = hi_end};

			res = bounds_verdict(t, &blk);
			if (res == SIFT_CF_FALSE)
				continue;
			if (res == SIFT_CF_TRUE)
			{
				for (uint32 j = 0; j < cnt; j++)
					out[(from + j) >> 6] |= UINT64CONST(1) << ((from + j) & 63);
				continue;
			}
		}
		if (c->bits > 0)
			unpack_vblock(c->words, c->bits, c->n, b, tmp);
		else
			memset(tmp, 0, sizeof(tmp));
		for (uint32 j = 0; j < cnt; j++)
		{
			bool		hit;

			run = j == 0 ? run : run + (uint64) c->step + tmp[j];
			hit = fast ? ((uint64) (int64) run - (uint64) t->lo <= span) == (t->kind == SIFT_CFT_RANGE) :
				bm != NULL ? bitmap_has(bm, lim, run - (uint64) c->range.vmin) : sift_cf_test_value(t, (int64) run);
			out[(from + j) >> 6] |= (uint64) hit << ((from + j) & 63);
		}
	}
	sift_free(first);
	if (bm != NULL)
		sift_free(bm);
	return finish(out, c->n);
}

/* ---- boolean ---- */

SiftCfResult
sift_cf_bool(const uint8 *bits, uint32 n, bool pass0, bool pass1, uint64 *out)
{
	if (n == 0 || (!pass0 && !pass1))
		return SIFT_CF_FALSE;
	if (pass0 && pass1)
		return SIFT_CF_TRUE;
	memset(out, 0, words_of(n) * sizeof(uint64));
	for (uint32 i = 0; i < (n + 7) / 8; i++)
		out[i >> 3] |= (uint64) bits[i] << (8 * (i & 7));
	if (pass0)
		invert(out, n);
	return finish(out, n);
}

/* ---- the rows ---- */

/* The k bits of dense that start at bit pos */
static uint64
take_bits(const uint64 *dense, uint32 pos, uint32 k)
{
	uint32		w = pos >> 6,
				s = pos & 63;
	uint64		v = dense[w] >> s;

	if (s != 0 && s + k > 64)
		v |= dense[w + 1] << (64 - s);
	return k >= 64 ? v : v & ((UINT64CONST(1) << k) - 1);
}

void
sift_cf_scatter(const uint64 *dense, const uint8 *validity, uint32 nrows, uint64 *out)
{
	uint32		pos = 0;

	for (uint32 w = 0; w < words_of(nrows); w++)
	{
		uint64		valid = 0,
					bits,
					m = 0;
		uint32		k;
		uint32		bytes = Min(8, (nrows - w * 64 + 7) / 8);

		for (uint32 i = 0; i < bytes; i++)
			valid |= (uint64) validity[w * 8 + i] << (8 * i);
		if (nrows - w * 64 < 64)
			valid &= (UINT64CONST(1) << (nrows - w * 64)) - 1;
		k = sift_pc64(valid);
		bits = k > 0 ? take_bits(dense, pos, k) : 0;
		for (uint64 v = valid; v != 0; v &= v - 1)
		{
			m |= (bits & 1) << __builtin_ctzll(v);
			bits >>= 1;
		}
		out[w] = m;
		pos += k;
	}
}
