/*
 * col/v1_engine/store_ops_vet.c - the rows of a batch of a Format 1 source that sit on heap pages that are not all-visible, checked against the heap after the quals (col/v1/store_src.h).
 *
 * The rows of such a unit end the batch they are in (col/v1/store_ops.c batch_fill), so the unit is still the reader's: the rows the quals kept are checked as a selection of the unit, and the
 * reader decodes the offsets of the pages that hold one of them only.
 */
/* check-memory: allow-allocs the bits follow the rows of one unit, not the rows of the relation */
#include "postgres.h"

#include "col/v1/store_src.h"

bool
sift_v1_src_vet(SiftV1Src *s, SiftV6Batch *b)
{
	const int	words = (b->n + 63) / 64;
	uint64	   *bits = MemoryContextAllocZero(s->batch_cx, SIFT_COL_NWORDS(s->unit.nrow) * sizeof(uint64));
	uint64		left = 0;

	if (b->mask != s->mask)
	{
		memset(s->mask, 0xFF, words * sizeof(uint64));
		if (b->n & 63)
			s->mask[words - 1] = (UINT64CONST(1) << (b->n & 63)) - 1;
		for (int w = 0; b->mask != NULL && w < words; w++)
			s->mask[w] &= b->mask[w];
		b->mask = s->mask;
	}
	for (int r = s->vet_from; r < b->n; r++)
		if (s->mask[r / 64] >> (r % 64) & 1)
			bits[s->ords[r - s->vet_from] / 64] |= UINT64CONST(1) << (s->ords[r - s->vet_from] % 64);
	s->rd->vet(s->src, &s->unit, bits);
	for (int r = s->vet_from; r < b->n; r++)
		if (!(bits[s->ords[r - s->vet_from] / 64] >> (s->ords[r - s->vet_from] % 64) & 1))
			s->mask[r / 64] &= ~(UINT64CONST(1) << (r % 64));
	for (int w = 0; w < words; w++)
		left |= s->mask[w];
	return left != 0;
}
