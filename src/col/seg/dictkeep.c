/*
 * col/seg/dictkeep.c - the compressed blocks of a dictionary read for a string by code: the last LOOK_KEEP of them are kept
 */
#include "col/seg_int.h"

/* The block b into blkraw: from the blocks kept when it was read lately, else decompressed (and kept, when it was compressed) */
void
sift_cseg_look_block(DictLook *l, uint32 b)
{
	uint32		cnt = Min(DICT_BLOCK, l->n - b * DICT_BLOCK);
	LookKept   *k = NULL;

	for (int i = 0; l->kept != NULL && i < LOOK_KEEP; i++)
		if (l->kept[i].b1 == b + 1)
			k = &l->kept[i];
	if (k != NULL)
	{
		memcpy(l->blkoff, k->off, (cnt + 1) * sizeof(uint16));
		memcpy(l->blkraw, k->raw, k->off[cnt]);
		l->blkno = b;
		return;
	}
	sift_cseg_look_load(l, b);
	if (l->blkz[0] == 0)
		return;
	DICT_BLOCK_DECODED();
	if (l->kept == NULL)
		l->kept = MemoryContextAllocZero(l->seg->mcxt, LOOK_KEEP * sizeof(LookKept));
	k = &l->kept[l->kept_next++ % LOOK_KEEP];
	if (k->cap < l->blkoff[cnt])
		k->raw = k->raw == NULL ? MemoryContextAlloc(l->seg->mcxt, k->cap = l->blkoff[cnt]) : repalloc(k->raw, k->cap = l->blkoff[cnt]);
	memcpy(k->off, l->blkoff, (cnt + 1) * sizeof(uint16));
	memcpy(k->raw, l->blkraw, l->blkoff[cnt]);
	k->b1 = b + 1;
}
