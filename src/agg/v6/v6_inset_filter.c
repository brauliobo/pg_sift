/*
 * agg/v6/v6_inset_filter.c - the per-row test of an in-set (v6_inset.h).
 */
#include "postgres.h"

#include "port/pg_bitutils.h"

#include "agg/v6/v6_inset.h"

static bool
member(const SiftV6InSet *s, int64 v)
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
