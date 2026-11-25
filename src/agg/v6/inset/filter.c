/*
 * agg/v6/inset/filter.c - the per-row test of an in-set (v6_inset.h).
 */
#include "postgres.h"

#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"

#include "agg/v6/inset/inset.h"

static inline uint64
mix(int64 v)
{
	uint64		z = (uint64) v + UINT64CONST(0x9e3779b97f4a7c15);

	z = (z ^ (z >> 30)) * UINT64CONST(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64CONST(0x94d049bb133111eb);
	return z ^ (z >> 31);
}

/* the two bits of the word of a value: the word by the low bits of its hash, the bits by the top ones */
static inline uint64
bloom_bits(uint64 h)
{
	return (UINT64CONST(1) << (h >> 58)) | (UINT64CONST(1) << ((h >> 52) & 63));
}

static inline uint32
table_slot(uint64 h, uint32 mask)
{
	return (uint32) (h >> 16) & mask;
}

void
sift_v6_inset_index(MemoryContext keep, SiftV6InSet *s)
{
	uint32		words = 64,
				cap = 32;

	s->vmin = s->n > 0 ? s->vals[0] : PG_INT64_MAX;
	s->vmax = s->n > 0 ? s->vals[s->n - 1] : PG_INT64_MIN;
	s->bloom = NULL;
	s->table = NULL;
	s->has_empty = false;
	if (s->n <= SIFT_V6_INSET_PLAIN)
		return;
	while (words < (uint32) s->n / 4 && words < 2048)
		words *= 2;
	while (cap < 2 * (uint32) s->n)
		cap *= 2;
	if ((Size) cap * sizeof(int64) > Min(MaxAllocSize, (Size) work_mem * 1024))
		return;
	s->bloom = MemoryContextAllocZero(keep, (Size) words * sizeof(uint64));
	s->bloom_mask = words - 1;
	s->table = MemoryContextAlloc(keep, (Size) cap * sizeof(int64));
	s->table_mask = cap - 1;
	for (uint32 i = 0; i < cap; i++)
		s->table[i] = SIFT_V6_INSET_EMPTY;
	for (int i = 0; i < s->n; i++)
	{
		const uint64 h = mix(s->vals[i]);

		s->bloom[h & s->bloom_mask] |= bloom_bits(h);
		if (s->vals[i] == SIFT_V6_INSET_EMPTY)
		{
			s->has_empty = true;
			continue;
		}
		for (uint32 at = table_slot(h, s->table_mask); ; at = (at + 1) & s->table_mask)
			if (s->table[at] == SIFT_V6_INSET_EMPTY)
			{
				s->table[at] = s->vals[i];
				break;
			}
	}
}

/* few values are searched as they are; more are asked of the bloom filter (one word, most of the values that are not in the set stop there), then of the table */
static inline bool
member(const SiftV6InSet *s, int64 v)
{
	if (v < s->vmin || v > s->vmax)
		return false;
	if (s->table == NULL)
	{
		int			lo = 0,
					hi = s->n - 1;

		while (lo <= hi)
		{
			const int	mid = lo + (hi - lo) / 2;

			if (s->vals[mid] == v)
				return true;
			if (s->vals[mid] < v)
				lo = mid + 1;
			else
				hi = mid - 1;
		}
		return false;
	}
	{
		const uint64 h = mix(v),
					m = bloom_bits(h);

		if ((s->bloom[h & s->bloom_mask] & m) != m)
			return false;
		if (v == SIFT_V6_INSET_EMPTY)
			return s->has_empty;
		for (uint32 at = table_slot(h, s->table_mask); s->table[at] != SIFT_V6_INSET_EMPTY; at = (at + 1) & s->table_mask)
			if (s->table[at] == v)
				return true;
		return false;
	}
}

uint16
sift_v6_inset_filter(const SiftV6InSet *s, const SiftVec *col, uint16 n, uint64 *out, uint16 left)
{
	const int	words = (n + 63) / 64;

	for (int w = 0; w < words; w++)
	{
		uint64		bits = out[w];

		while (bits != 0)
		{
			const int	row = w * 64 + pg_rightmost_one_pos64(bits);
			const uint64 bit = bits & -bits;
			bool		keep = col->valid == NULL || (col->valid[w] & bit) != 0;

			bits ^= bit;
			if (keep)
				keep = member(s, col->type == SVEC_I32 ? ((const int32 *) col->data)[row] : ((const int64 *) col->data)[row]);
			if (!keep)
			{
				out[w] &= ~bit;
				left--;
			}
		}
	}
	return left;
}
