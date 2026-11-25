/*
 * bm25/eval/fields.c - the length of a field in the score of a leaf that searches it (see SiftPlan.nfields): the averages over the docs that keep the
 * lengths, and the norms of a block of such docs.
 */
#include "core/host.h"

#include "bm25/eval/int.h"

/* The sums over the live docs of the segments that keep field lengths; a parallel participant takes the exporter's */
void
bind_field_totals(SiftPlan *p, const void *shared)
{
	if (!p->scoring || !p->fmask)
		return;
	if (shared)
	{
		const SharedBind *h = shared;

		p->seg_fdocs = h->seg_fdocs;
		memcpy(p->seg_fsum, h->seg_fsum, sizeof(p->seg_fsum));
		return;
	}
	for (int s = 0; s < p->nsegs; s++)
	{
		uint64		sums[SIFT_MAX_FIELDS],
					n,
					sum_dl;

		if (sift_seg_nfields(p->seg[s]) == 0)
			continue;
		sift_seg_live_field_sums(p->seg[s], sums);
		sift_seg_live_totals(p->seg[s], &n, &sum_dl);
		p->seg_fdocs += n;
		for (int f = 0; f < sift_seg_nfields(p->seg[s]); f++)
			p->seg_fsum[f] += sums[f];
	}
}

/* The parameters of each field the plan searches, once the write buffers are bound */
void
bind_field_params(SiftPlan *p, float4 k1, float4 b)
{
	double		docs = (double) p->seg_fdocs + p->wbuf_fdocs;

	p->fscored = docs > 0;
	for (int f = 0; f < p->nfields; f++)
	{
		double		avg = p->fscored ? (p->seg_fsum[f] + p->wbuf_fsum[f]) / docs : 0;

		if (p->fmask >> f & 1)
			sift_kern_bm25_init(&p->bmf[f], k1, b, avg > 0 ? (float4) avg : 1.0f);
	}
}

/* The norms of the fields the plan searches for the docs of block b of the SB in dc (the pages in want), as wk->knorm is the norm of the docs' own lengths */
void
rank_field_knorm(SiftPlan *p, SiftSegment *seg, const SiftDocChunk *dc, int block, uint32 want, RankWork *wk)
{
	bool		kept = sift_seg_nfields(seg) > 0;

	for (int f = 0; f < p->nfields; f++)
	{
		const uint16 *rows[SIFT_BLK_PAGES];

		if (!(p->fmask >> f & 1))
			continue;
		if (!kept)
		{
			wk->lknorm[f] = wk->knorm;
			continue;
		}
		sift_seg_block_field_dl(seg, dc, block, want, f, rows);
		for (int pg = 0; pg < SIFT_BLK_PAGES; pg++)
			if (rows[pg] == NULL)
				rows[pg] = wk->zero;
		sift_kern->knorm(wk->fknorm[f], rows, wk->res[0], SIFT_BLK_PAGES, &p->bmf[f]);
		wk->lknorm[f] = wk->fknorm[f];
	}
}
