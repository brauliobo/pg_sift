/*
 * col/granule/cfilt_str_simd.c - the string predicates of a chunk on its lengths and on one sweep over its bytes (col/granule/cfilt_str_simd.h).
 *
 * The strings of a chunk that is stored as FSST symbols, as a block or as offsets over bytes decode to one buffer, the strings one after the other: a substring search over the buffer finds the
 * strings that hold the needle by the offset of each hit, and an occurrence across two strings is not a hit. A string is empty exactly when its length is 0, so <> '' reads the lengths only.
 */
#include "postgres.h"

#include "core/cpu.h"

#include "col/f2/f2_pred.h"
#include "col/granule/cfilt_str_simd.h"
#include "col/like_simd.h"

#if defined(__x86_64__)
#include <immintrin.h>

SIFT_TARGET_AVX2 static uint32
len_nonzero_avx2(const uint32 *len, uint32 n, uint64 *bits)
{
	const __m256i zero = _mm256_setzero_si256();
	uint32		i = 0;

	for (; i + 8 <= n; i += 8)
	{
		const __m256i v = _mm256_loadu_si256((const __m256i *) (len + i));
		const uint32 eq = (uint32) _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(v, zero)));

		bits[i >> 6] |= (uint64) (~eq & 0xFF) << (i & 63);
	}
	return i;
}
#endif

void
sift_cfs_len_nonzero(const uint32 *len, uint32 n, uint64 *bits)
{
	uint32		i = 0;

	memset(bits, 0, ((n + 63) / 64) * sizeof(uint64));
#if defined(__x86_64__)
	if (sift_cpu.active >= SIFT_SIMD_AVX2 && (sift_cpu.flags & SIFT_CPU_AVX2))
		i = len_nonzero_avx2(len, n, bits);
#endif
	for (; i < n; i++)
		bits[i >> 6] |= (uint64) (len[i] != 0) << (i & 63);
}

bool
sift_cfs_slab(const char *const *p, const uint32 *len, uint32 n, uint32 *off, const char **base)
{
	uint64		at = 0;

	off[0] = 0;
	*base = NULL;
	for (uint32 i = 0; i < n; i++)
	{
		if (len[i] > 0 && *base == NULL)
			*base = p[i];
		if (len[i] > 0 && p[i] != *base + at)
			return false;
		at += len[i];
		if (at > UINT32_MAX)
			return false;
		off[i + 1] = (uint32) at;
	}
	return true;
}

static uint32
contains_each(const SiftLikeNeedle *nd, uint32 n, const char *const *p, const uint32 *len, uint64 *bits)
{
	uint32		found = 0;

	for (uint32 i = 0; i < n; i++)
		if (sift_like_find(nd, (const uint8 *) p[i], len[i]) >= 0)
		{
			bits[i >> 6] |= UINT64CONST(1) << (i & 63);
			found++;
		}
	return found;
}

uint32
sift_cfs_contains(const SiftLikeNeedle *nd, uint32 n, const char *const *p, const uint32 *len, uint64 *bits)
{
	uint32	   *off = palloc(((size_t) n + 1) * sizeof(uint32));
	const char *base;
	uint32		found;

	if (sift_cfs_slab(p, len, n, off, &base))
		found = sift_like_dict(nd, n, off, (const uint8 *) (base != NULL ? base : (const char *) off), bits, NULL);
	else
	{
		memset(bits, 0, ((n + 63) / 64) * sizeof(uint64));
		found = contains_each(nd, n, p, len, bits);
	}
	pfree(off);
	return found;
}

void
sift_cfs_invert(uint64 *bits, uint32 n)
{
	for (uint32 w = 0; w < (n + 63) / 64; w++)
		bits[w] = ~bits[w];
	if (n & 63)
		bits[n >> 6] &= (UINT64CONST(1) << (n & 63)) - 1;
}

void
sift_cfs_ne(const char *lit, uint32 litlen, uint32 n, const char *const *p, const uint32 *len, uint64 *bits)
{
	if (litlen == 0)
	{
		sift_cfs_len_nonzero(len, n, bits);
		return;
	}
	memset(bits, 0, ((n + 63) / 64) * sizeof(uint64));
	for (uint32 i = 0; i < n; i++)
		bits[i >> 6] |= (uint64) (len[i] != litlen || memcmp(p[i], lit, litlen) != 0) << (i & 63);
}

static bool
chunk_is_slab(uint8 enc)
{
	return enc == SIFT_CENC_STR_FSST || enc == SIFT_CENC_STR_BLOCK || enc == SIFT_CENC_STR_INLINE || enc == SIFT_CENC_STR_VIEW;
}

bool
sift_cfstr_bits(const SiftCfiltPred *cp, const SiftChunk *c, uint64 *dense)
{
	const SiftColPred *pr = cp->pred;
	const SiftColValue *v = &pr->vals[0];
	const uint32 nn = c->nnonnull;
	const char **p;
	uint32	   *len;

	if (!chunk_is_slab(c->enc) || nn == 0)
		return false;
	if (pr->op != SIFT_COLOP_NE && pr->op != SIFT_COLOP_CONTAINS && pr->op != SIFT_COLOP_NOTCONTAINS && pr->op != SIFT_COLOP_ICONTAINS)
		return false;
	p = palloc(sizeof(char *) * nn);
	len = palloc(sizeof(uint32) * nn);
	sift_chunk_strs(c, 0, nn, p, len);
	if (pr->op == SIFT_COLOP_NE)
		sift_cfs_ne(v->v.s, v->len, nn, p, len, dense);
	else
	{
		SiftLikeNeedle nd;

		sift_like_needle_borrow(&nd, (const uint8 *) v->v.s, v->len, pr->op == SIFT_COLOP_ICONTAINS);
		sift_cfs_contains(&nd, nn, p, len, dense);
		if (pr->op == SIFT_COLOP_NOTCONTAINS)
			sift_cfs_invert(dense, nn);
	}
	pfree(p);
	pfree(len);
	return true;
}
