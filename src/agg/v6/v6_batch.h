/*
 * agg/v6/v6_batch.h - the active rows of a SiftBatchSel as a loop: its selection vector when it has one, every row when all are active, else the set bits of its mask, in increasing row order.
 */
#ifndef SIFT_AGG_V6_BATCH_H
#define SIFT_AGG_V6_BATCH_H

#include "core/host.h"

#include "agg/vec.h"

typedef struct SiftBatchIter
{
	const SiftBatchSel *bs;
	uint32		i;
	uint32		word;
	uint64		bits;
} SiftBatchIter;

static inline void
sift_batch_iter_init(SiftBatchIter *it, const SiftBatchSel *bs)
{
	it->bs = bs;
	it->i = 0;
	it->word = 0;
	it->bits = (bs->has_sel || bs->all_active) ? 0 : bs->mask[0];
}

static inline bool
sift_batch_iter_next(SiftBatchIter *it, uint32 *row)
{
	const SiftBatchSel *bs = it->bs;

	if (bs->has_sel)
	{
		if (it->i >= bs->n_active)
			return false;
		*row = bs->sel[it->i++];
		return true;
	}
	if (bs->all_active)
	{
		if (it->i >= bs->n_raw)
			return false;
		*row = it->i++;
		return true;
	}
	while (it->bits == 0)
	{
		if (++it->word >= (uint32) ((bs->n_raw + 63) / 64))
			return false;
		it->bits = bs->mask[it->word];
	}
	*row = it->word * 64 + (uint32) __builtin_ctzll(it->bits);
	it->bits &= it->bits - 1;
	return true;
}

static inline bool
sift_vec_valid_at(const SiftVec *v, uint32 r)
{
	return v->all_valid || ((v->valid[r >> 6] >> (r & 63)) & 1);
}

#endif							/* SIFT_AGG_V6_BATCH_H */
