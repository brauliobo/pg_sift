/*
 * col/batch_dead.c - the rows of a kept batch of the write buffer that a VACUUM or a fold of marks flagged dead (col/wbuf/batches.c). The slots and the unit image the
 * scans made of the batch stay as they are: a bit says the row is not read (sift_cbatch_dead), so a kept batch costs a bit to change, not an encode.
 */
#include "core/host.h"

#include "col/enc.h"

void
sift_cbatch_mark_dead(SiftColBatch *b, const SiftKey *keys, int n)
{
	uint32		r = 0;

	if (b->deadrows < b->nrows)
	{
		uint32		words = (b->nrows + 63) / 64;
		uint64	   *dead = MemoryContextAllocZero(b->mcxt, words * sizeof(uint64));

		if (b->ndead > 0)
			memcpy(dead, b->dead, (b->deadrows / 64) * sizeof(uint64));
		if (b->dead)
			pfree(b->dead);
		b->dead = dead;
		b->deadrows = words * 64;
	}
	for (int k = 0; k < n; k++)
	{
		uint32		hi = b->nrows;

		while (r < hi)
		{
			uint32		m = r + (hi - r) / 2;

			if (b->keys[m] < keys[k])
				r = m + 1;
			else
				hi = m;
		}
		if (r < b->nrows && b->keys[r] == keys[k] && !sift_cbatch_dead(b, r))
		{
			b->dead[r >> 6] |= UINT64CONST(1) << (r & 63);
			b->ndead++;
		}
	}
}
