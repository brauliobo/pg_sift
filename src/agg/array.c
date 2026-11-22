/*
 * agg/array.c - the element mask of an array column from the mask of its parent rows, without a parent index per element: a block of
 * 64 parents with no row selected is skipped, one with every row selected is one run of elements, and any other block sets the run of
 * each selected row.
 */
#include "core/host.h"

#include "agg/vec.h"

/* Sets the bits [lo, hi) */
static void
set_run(uint64 *out, uint32 lo, uint32 hi)
{
	const uint32 wl = lo >> 6,
				wh = (hi - 1) >> 6;
	const uint64 first = ~UINT64CONST(0) << (lo & 63),
				last = ~UINT64CONST(0) >> (63 - ((hi - 1) & 63));

	if (lo >= hi)
		return;
	if (wl == wh)
	{
		out[wl] |= first & last;
		return;
	}
	out[wl] |= first;
	for (uint32 w = wl + 1; w < wh; w++)
		out[w] = ~UINT64CONST(0);
	out[wh] |= last;
}

uint32
sift_array_elem_mask(uint64 *out, const SiftArrayLayout *lay, const uint64 *parent_mask)
{
	const uint32 nblocks = (lay->n_parents + 63) >> 6,
				nwords = (lay->n_elems + 63) >> 6;
	uint32		n = 0;

	memset(out, 0, (size_t) nwords * 8);
	for (uint32 b = 0; b < nblocks; b++)
	{
		const uint32 valid = Min(64, lay->n_parents - 64 * b);
		const uint64 all = valid == 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << valid) - 1;
		uint64		word = parent_mask[b] & all;

		if (word == 0)
			continue;
		if (word == all)
		{
			set_run(out, lay->block_elem_off[b], lay->block_elem_off[b + 1]);
			n += lay->block_elem_off[b + 1] - lay->block_elem_off[b];
			continue;
		}
		for (; word; word &= word - 1)
		{
			const uint32 r = 64 * b + __builtin_ctzll(word);

			set_run(out, lay->offsets[r], lay->offsets[r + 1]);
			n += lay->lengths[r];
		}
	}
	return n;
}
