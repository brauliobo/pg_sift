/*
 * col/f2/compact/outputs.c - the output parts of a compaction given back (compact.h).
 */
#include "core/host.h"

#include "col/f2/compact/compact.h"

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
