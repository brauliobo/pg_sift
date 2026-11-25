/*
 * agg/filter/pivot.c - the clause bitmaps of up to 64 FILTER (WHERE ..) clauses of a batch (agg/filter/pivot.h).
 */
#include "agg/filter/pivot.h"

struct SiftFilterMasks
{
	MemoryContext cx;
	int			nfilters;
	bool		rows_valid;
	uint64		active[SIFT_VEC_WORDS];
	uint64	   *bits;			/* nfilters x SIFT_VEC_WORDS */
	uint64	   *rows;			/* SIFT_VEC_LEN */
};

SiftFilterMasks *
sift_fmasks_create(MemoryContext cx, int nfilters)
{
	SiftFilterMasks *m = MemoryContextAllocZero(cx, sizeof(SiftFilterMasks));

	SIFT_INVARIANT(nfilters >= 0 && nfilters <= SIFT_PIVOT_MAX_FILTERS, "filter masks: %d clauses", nfilters);
	m->cx = cx;
	m->nfilters = nfilters;
	m->bits = MemoryContextAlloc(cx, sizeof(uint64) * SIFT_VEC_WORDS * Max(nfilters, 1));
	m->rows = MemoryContextAlloc(cx, sizeof(uint64) * SIFT_VEC_LEN);
	return m;
}

void
sift_fmasks_eval(SiftFilterMasks *m, const SiftBatchSel *bs, SiftPivotEval eval, void *ctx)
{
	const uint32 nwords = (uint32) ((bs->n_raw + 63) / 64);
	SiftBatchIter it;
	uint32		r;

	memset(m->active, 0, sizeof(m->active));
	sift_batch_iter_init(&it, bs);
	while (sift_batch_iter_next(&it, &r))
		m->active[r >> 6] |= UINT64_C(1) << (r & 63);
	for (int f = 0; f < m->nfilters; f++)
	{
		uint64	   *b = m->bits + (size_t) f * SIFT_VEC_WORDS;

		memset(b, 0, sizeof(uint64) * SIFT_VEC_WORDS);
		eval(ctx, f, b);
		for (uint32 w = 0; w < nwords; w++)
			b[w] &= m->active[w];
	}
	m->rows_valid = false;
}

const uint64 *
sift_fmasks_active(const SiftFilterMasks *m)
{
	return m->active;
}

const uint64 *
sift_fmasks_bits(const SiftFilterMasks *m, int f)
{
	return m->bits + (size_t) f * SIFT_VEC_WORDS;
}

const uint64 *
sift_fmasks_rows(SiftFilterMasks *m)
{
	if (!m->rows_valid)
	{
		for (uint32 w = 0; w < SIFT_VEC_WORDS; w++)
		{
			uint64		blk[64];

			for (int f = 0; f < 64; f++)
				blk[f] = f < m->nfilters ? m->bits[(size_t) f * SIFT_VEC_WORDS + w] : 0;
			sift_pivot_transpose64(blk);
			memcpy(m->rows + w * 64, blk, sizeof(blk));
		}
		m->rows_valid = true;
	}
	return m->rows;
}

void
sift_fmasks_free(SiftFilterMasks *m)
{
	pfree(m->bits);
	pfree(m->rows);
	pfree(m);
}

void
sift_pivot_transpose64(uint64 *a)
{
	uint64		m = UINT64_C(0x00000000FFFFFFFF);

	for (int j = 32; j != 0; j >>= 1, m ^= m << j)
		for (int k = 0; k < 64; k = (k + j + 1) & ~j)
		{
			uint64		t = ((a[k] >> j) ^ a[k + j]) & m;

			a[k] ^= t << j;
			a[k + j] ^= t;
		}
}
