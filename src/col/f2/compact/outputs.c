/*
 * col/f2/compact/outputs.c - the output parts of a compaction: the slack of their cubes, and the parts given back (compact.h).
 */
#include "core/host.h"

#include "col/f2/compact/compact.h"
#include "col/f2/preagg/v2.h"

static uint64
live_rows(const SiftPartDesc *d)
{
	return d->nrows - Min(d->ndead, d->nrows);
}

uint64
sift_compact_preagg_slack(const SiftPartDesc *parts, int nparts, const uint64 *gens, int ngens)
{
	uint64		mine = 0,
				all = 0;

	for (int i = 0; i < nparts; i++)
	{
		all += live_rows(&parts[i]);
		for (int k = 0; k < ngens; k++)
			mine += parts[i].part_gen == gens[k] ? live_rows(&parts[i]) : 0;
	}
	return all == 0 ? SIFT_PAG_BUDGET_SLACK : SIFT_PAG_BUDGET_SLACK * mine / all;
}

void
sift_compact_release_outputs_dv(Relation rel, const SiftPartDesc *out)
{
	if (out->dv_blk != InvalidBlockNumber)
		sift_fsm_release(rel, &(SiftExtent) {out->dv_blk, out->dv_nblocks}, 1, InvalidFullTransactionId);
}

void
sift_compact_release_outputs(Relation rel, uint64 part_id, const SiftPartDesc *out, int nout)
{
	for (int k = 0; k < nout; k++)
	{
		int			nb;
		SiftExtent *blocks = sift_part_blocks(sift_part_open(rel, out[k].header_blk, out[k].part_gen, (uint32) part_id), &nb);

		sift_fsm_release(rel, blocks, nb, InvalidFullTransactionId);
		sift_compact_release_outputs_dv(rel, &out[k]);
	}
}
