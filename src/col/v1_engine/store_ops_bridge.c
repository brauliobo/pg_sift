/*
 * col/v1_engine/store_ops_bridge.c - the rows of a Format 1 source that the hits of a text query keep (col/v1/store_src.h). A text index has no entry for the version a HOT update of a
 * sift_table made: the bridge holds that row by the root of its chain, so the rows of a batch are looked up by their roots, and keep their own tags for the fetch.
 */
#include "postgres.h"

#include "col/v1/store_src.h"
#include "table/v1/table.h"

void
sift_v1_src_bridge_begin(SiftV1Src *s, const SiftTidBridge *bridge)
{
	s->bridge = bridge;
	s->hot = bridge != NULL && s->in.is_table ? sift_tbl_chains(s->rels.heap) : NULL;
}

bool
sift_v1_src_bridge_narrow(SiftV1Src *s, SiftV6Batch *b)
{
	const int	words = (b->n + 63) / 64;
	uint64		live[SIFT_VEC_WORDS],
				root[SIFT_VEC_LEN];

	memset(live, 0xFF, words * sizeof(uint64));
	if (b->n & 63)
		live[words - 1] = (UINT64CONST(1) << (b->n & 63)) - 1;
	if (sift_tidbridge_probe_vec(s->bridge, sift_tomb_hot_roots(s->hot, b->tag, b->n, root), live, b->n, s->kept, s->score) == 0)
		return false;
	if (s->score_col >= 0)
	{
		for (int r = 0; r < b->n; r++)
			((double *) s->data[s->score_col])[r] = s->score[r];
		memcpy(s->valid[s->score_col], live, words * sizeof(uint64));
	}
	b->mask = s->kept;
	return true;
}
