/*
 * col/like/simd.c - the needle, the scalar search, the dispatch and the scan of a dictionary (col/like/simd.h).
 */
#include "core/host.h"

#include "core/cpu.h"
#include "core/popcount.h"

#include "col/like/simd.h"

static inline uint8
ascii_lower(uint8 c)
{
	return (c >= 'A' && c <= 'Z') ? (uint8) (c + 32) : c;
}

static inline uint8
fold_or(uint8 c)
{
	return (c >= 'a' && c <= 'z') ? 0x20 : 0x00;
}

void
sift_like_needle_borrow(SiftLikeNeedle *nd, const uint8 *folded, uint32 len, bool fold)
{
	*nd = (SiftLikeNeedle) {.bytes = (uint8 *) folded,.len = len,.fold = fold};
	for (uint32 i = 0; i < len; i++)
		nd->has_ik |= fold && (folded[i] == 'i' || folded[i] == 'k');
	if (len > 0)
	{
		nd->first = folded[0];
		nd->last = folded[len - 1];
		nd->first_or = fold ? fold_or(nd->first) : 0;
		nd->last_or = fold ? fold_or(nd->last) : 0;
	}
}

SiftLikeNeedle *
sift_like_needle_new(const uint8 *s, uint32 len, bool fold)
{
	SiftLikeNeedle *nd = palloc0(sizeof(SiftLikeNeedle));
	uint8	   *own = palloc(len ? len : 1);

	for (uint32 i = 0; i < len; i++)
		own[i] = fold ? ascii_lower(s[i]) : s[i];
	sift_like_needle_borrow(nd, own, len, fold);
	return nd;
}

void
sift_like_needle_free(SiftLikeNeedle *nd)
{
	pfree(nd->bytes);
	pfree(nd);
}

/* The bytes between the first and the last of the needle, at the position of its first byte in the haystack */
bool
sift_like_verify(const SiftLikeNeedle *nd, const uint8 *at)
{
	if (!nd->fold)
		return nd->len <= 2 || memcmp(at + 1, nd->bytes + 1, nd->len - 2) == 0;
	for (uint32 i = 1; i + 1 < nd->len; i++)
		if (ascii_lower(at[i]) != nd->bytes[i])
			return false;
	return true;
}

static inline bool
ends_match(const SiftLikeNeedle *nd, const uint8 *hay, size_t pos)
{
	if (nd->fold)
		return ascii_lower(hay[pos]) == nd->first && ascii_lower(hay[pos + nd->len - 1]) == nd->last;
	return hay[pos] == nd->first && hay[pos + nd->len - 1] == nd->last;
}

static inline uint8
hay_byte(const SiftLikeNeedle *nd, const uint8 *hay, size_t at)
{
	return nd->fold ? ascii_lower(hay[at]) : hay[at];
}

/* The maximal suffix of the needle under the byte order (rev: the reverse order) and its period, as the critical factorization of Crochemore and Perrin wants them */
static ptrdiff_t
max_suffix(const uint8 *x, size_t m, bool rev, size_t *period)
{
	ptrdiff_t	ms = -1;
	size_t		j = 0,
				k = 1,
				p = 1;

	while (j + k < m)
	{
		const uint8 a = x[j + k],
					b = x[ms + (ptrdiff_t) k];

		if (rev ? a > b : a < b)
		{
			j += k;
			k = 1;
			p = j - ms;
		}
		else if (a == b)
		{
			if (k != p)
				k++;
			else
			{
				j += p;
				k = 1;
			}
		}
		else
		{
			ms = (ptrdiff_t) j;
			j = ms + 1;
			k = p = 1;
		}
	}
	*period = p;
	return ms;
}

/* Two-Way (Crochemore and Perrin): O(hlen + len) bytes compared whatever the bytes are, no allocation */
int64
sift_like_find_twoway(const SiftLikeNeedle *nd, const uint8 *hay, size_t hlen)
{
	const uint8 *x = nd->bytes;
	const size_t m = nd->len;
	size_t		p1,
				p2,
				per,
				j = 0;
	ptrdiff_t	i1 = max_suffix(x, m, false, &p1),
				i2 = max_suffix(x, m, true, &p2),
				ell = i1 > i2 ? i1 : i2;

	if (m == 0)
		return 0;
	per = i1 > i2 ? p1 : p2;
	if (hlen < m)
		return -1;
	if (memcmp(x, x + per, (size_t) (ell + 1)) == 0)
	{
		ptrdiff_t	memory = -1;

		while (j + m <= hlen)
		{
			ptrdiff_t	i = (ell > memory ? ell : memory) + 1;

			while ((size_t) i < m && x[i] == hay_byte(nd, hay, i + j))
				i++;
			if ((size_t) i >= m)
			{
				i = ell;
				while (i > memory && x[i] == hay_byte(nd, hay, i + j))
					i--;
				if (i <= memory)
					return (int64) j;
				j += per;
				memory = (ptrdiff_t) (m - per) - 1;
			}
			else
			{
				j += (size_t) (i - ell);
				memory = -1;
			}
		}
		return -1;
	}
	per = (size_t) Max(ell + 1, (ptrdiff_t) m - ell - 1) + 1;
	while (j + m <= hlen)
	{
		ptrdiff_t	i = ell + 1;

		while ((size_t) i < m && x[i] == hay_byte(nd, hay, i + j))
			i++;
		if ((size_t) i >= m)
		{
			i = ell;
			while (i >= 0 && x[i] == hay_byte(nd, hay, i + j))
				i--;
			if (i < 0)
				return (int64) j;
			j += per;
		}
		else
			j += (size_t) (i - ell);
	}
	return -1;
}

int64
sift_like_find_scalar(const SiftLikeNeedle *nd, const uint8 *hay, size_t hlen)
{
	size_t		spent = 0;

	if (nd->len == 0)
		return 0;
	if (hlen < nd->len)
		return -1;
	if (!nd->fold && nd->len == 1)
	{
		const uint8 *p = memchr(hay, nd->first, hlen);

		return p == NULL ? -1 : (int64) (p - hay);
	}
	for (size_t pos = 0; pos + nd->len <= hlen; pos++)
	{
		if (!ends_match(nd, hay, pos))
			continue;
		if (sift_like_verify(nd, hay + pos))
			return (int64) pos;
		spent += nd->len;
		if (spent > SIFT_LIKE_VERIFY_BUDGET(pos))
		{
			const int64 r = sift_like_find_twoway(nd, hay + pos + 1, hlen - pos - 1);

			return r < 0 ? -1 : (int64) pos + 1 + r;
		}
	}
	return -1;
}

int64
sift_like_find(const SiftLikeNeedle *nd, const uint8 *hay, size_t hlen)
{
	if (nd->len == 0)
		return 0;
	if (hlen < nd->len)
		return -1;
#if defined(__x86_64__)
	if (sift_cpu.active >= SIFT_SIMD_AVX2 && (sift_cpu.flags & SIFT_CPU_AVX2))
		return sift_like_find_avx2(nd, hay, hlen);
#elif defined(__aarch64__)
	if (sift_cpu.active >= SIFT_SIMD_NEON && (sift_cpu.flags & SIFT_CPU_NEON))
		return sift_like_find_neon(nd, hay, hlen);
#endif
	return sift_like_find_scalar(nd, hay, hlen);
}

bool
sift_like_has_high(const uint8 *p, size_t len)
{
#if defined(__x86_64__)
	if (sift_cpu.active >= SIFT_SIMD_AVX2 && (sift_cpu.flags & SIFT_CPU_AVX2))
		return sift_like_has_high_avx2(p, len);
#elif defined(__aarch64__)
	if (sift_cpu.active >= SIFT_SIMD_NEON && (sift_cpu.flags & SIFT_CPU_NEON))
		return sift_like_has_high_neon(p, len);
#endif
	for (size_t i = 0; i < len; i++)
		if (p[i] & 0x80)
			return true;
	return false;
}

SiftLikeVerdict
sift_like_test(const SiftLikeNeedle *nd, const uint8 *s, uint32 len)
{
	if (sift_like_find(nd, s, len) >= 0)
		return SIFT_LIKE_YES;
	return nd->has_ik && sift_like_has_high(s, len) ? SIFT_LIKE_MAYBE : SIFT_LIKE_NO;
}

bool
sift_like_shape_holds(SiftLikeShape shape, const SiftLikeNeedle *lit, const uint8 *s, uint32 len)
{
	switch (shape)
	{
		case SIFT_LIKE_CONTAINS:
			return sift_like_find(lit, s, len) >= 0;
		case SIFT_LIKE_PREFIX:
			return len >= lit->len && memcmp(s, lit->bytes, lit->len) == 0;
		case SIFT_LIKE_SUFFIX:
			return len >= lit->len && memcmp(s + len - lit->len, lit->bytes, lit->len) == 0;
		default:
			return len == lit->len && memcmp(s, lit->bytes, len) == 0;
	}
}

uint32
sift_like_dict(const SiftLikeNeedle *nd, uint32 n, const uint32 *off, const uint8 *bytes, uint64 *match, uint64 *maybe)
{
	uint32		nw = (n + 63) / 64,
				found = 0,
				e = 0;
	size_t		start = 0,
				total = n > 0 ? off[n] : 0;

	memset(match, 0, nw * sizeof(uint64));
	if (maybe != NULL)
		memset(maybe, 0, nw * sizeof(uint64));
	if (nd->len == 0)
	{
		for (uint32 i = 0; i < n; i++)
			match[i >> 6] |= UINT64CONST(1) << (i & 63);
		return n;
	}
	while (start + nd->len <= total)
	{
		int64		at = sift_like_find(nd, bytes + start, total - start);
		size_t		pos;

		if (at < 0)
			break;
		pos = start + (size_t) at;
		while (off[e + 1] <= pos)
			e++;
		if (pos + nd->len <= off[e + 1])
		{
			match[e >> 6] |= UINT64CONST(1) << (e & 63);
			found++;
			start = off[e + 1];
		}
		else
			start = pos + 1;
	}
	if (nd->has_ik && maybe != NULL)
		for (uint32 i = 0; i < n; i++)
			if (!((match[i >> 6] >> (i & 63)) & 1) && sift_like_has_high(bytes + off[i], off[i + 1] - off[i]))
			{
				maybe[i >> 6] |= UINT64CONST(1) << (i & 63);
				found++;
			}
	return found;
}

SiftLikeShape
sift_like_parse(const uint8 *pat, uint32 len, uint8 *lit, uint32 *litlen)
{
	uint32		i = 0,
				end = len,
				n = 0;
	bool		lead = false,
				trail = false;

	while (i < end && pat[i] == '%')
	{
		lead = true;
		i++;
	}
	while (end > i && pat[end - 1] == '%')
	{
		uint32		slashes = 0;

		while (end - 1 > i + slashes && pat[end - 2 - slashes] == '\\')
			slashes++;
		if (slashes % 2 == 1)
			break;
		trail = true;
		end--;
	}
	for (; i < end; i++)
	{
		if (pat[i] == '\\' && i + 1 < end)
			lit[n++] = pat[++i];
		else if (pat[i] == '%' || pat[i] == '_' || pat[i] == '\\')
			return SIFT_LIKE_OTHER;
		else
			lit[n++] = pat[i];
	}
	*litlen = n;
	if (lead && trail)
		return n > 0 ? SIFT_LIKE_CONTAINS : SIFT_LIKE_OTHER;
	if (lead)
		return n > 0 ? SIFT_LIKE_SUFFIX : SIFT_LIKE_OTHER;
	return trail ? (n > 0 ? SIFT_LIKE_PREFIX : SIFT_LIKE_OTHER) : SIFT_LIKE_EQUAL;
}
