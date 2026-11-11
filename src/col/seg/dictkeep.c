/*
 * col/seg/dictkeep.c - the blocks of a dictionary read for a string by code: their load (the offsets of blocks read in turn come from one window), and the last
 * LOOK_KEEP compressed ones kept
 */
#include "col/seg_int.h"
#include "col/compress.h"

#define BOFF_WIN	256		/* block offsets read at once when blocks are read in turn: a page of them */

/*
 * The start and the end of block b in the stream. Blocks read in turn (a full read of the strings of a run) take their offsets from a window read once: the offsets and
 * the blocks lie far apart, and a read of each one in turn leaves the page of the blocks for the page of the offsets and back
 */
static void
look_boff(DictLook *l, uint32 b, uint32 *o)
{
	if (l->boffw == NULL || b < l->boffw_b0 || b + 1 >= l->boffw_b0 + l->boffw_n)
	{
		uint32		nblk = (l->n + DICT_BLOCK - 1) / DICT_BLOCK;
		uint32		k = b == l->boffw_last1 ? Min(BOFF_WIN, nblk + 1 - b) : 2;

		if (l->boffw == NULL)
			l->boffw = MemoryContextAlloc(l->seg->mcxt, BOFF_WIN * sizeof(uint32));
		seg_read(l->seg, l->boff_at + (uint64) b * sizeof(uint32), k * sizeof(uint32), l->boffw);
		l->boffw_b0 = b;
		l->boffw_n = k;
	}
	l->boffw_last1 = b + 1;
	o[0] = l->boffw[b - l->boffw_b0];
	o[1] = l->boffw[b - l->boffw_b0 + 1];
}

/* Block b of a dictionary of the searched form, decompressed into l->blkraw with the offsets of its strings in l->blkoff */
void
sift_cseg_look_load(DictLook *l, uint32 b)
{
	uint32		o[2],
				cnt = Min(DICT_BLOCK, l->n - b * DICT_BLOCK),
				rawlen,
				pos = 0;

	look_boff(l, b, o);
	if (o[1] < o[0] + DICT_BLOCK_HEAD || o[1] - o[0] > sizeof(l->blkz) || o[1] > l->blk_len)
		corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
	seg_read(l->seg, l->blk_at + o[0], o[1] - o[0], l->blkz);
	rawlen = l->blkz[2] | (uint32) l->blkz[3] << 8;
	if (rawlen > DICT_BLOCK_RAW_MAX)
		corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
	if (l->blkz[0] == 0)
	{
		if (o[1] - o[0] - DICT_BLOCK_HEAD != rawlen)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
		memcpy(l->blkraw, l->blkz + DICT_BLOCK_HEAD, rawlen);
	}
	else if ((l->blkz[0] != SIFT_CC_ALG_ZSTD && l->blkz[0] != SIFT_CC_ALG_LZ4) || !sift_colz_available(l->blkz[0]))
		corrupt(l->seg, "The dictionary of leaf %d is compressed with an algorithm this build lacks.", l->col);
	else if (!sift_colz_decompress(l->blkz[0], l->blkz + DICT_BLOCK_HEAD, o[1] - o[0] - DICT_BLOCK_HEAD, l->blkraw, rawlen))
		corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
	if (l->fixed)
	{
		if (rawlen != cnt * l->fixed)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
		for (uint32 i = 0; i <= cnt; i++)
			l->blkoff[i] = i * l->fixed;
	}
	else
	{
		if (rawlen < cnt)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
		for (uint32 i = 0; i < cnt; i++)
		{
			l->blkoff[i] = cnt + pos;
			pos += l->blkraw[i];
		}
		l->blkoff[cnt] = cnt + pos;
		if (cnt + pos != rawlen)
			corrupt(l->seg, "The dictionary of leaf %d is malformed.", l->col);
	}
	l->blkno = b;
	l->loads++;
	if (l->bc != NULL)
		l->bc->spent += l->blkz[0] == 0 ? DICT_READ_COST : l->blkz[0] == SIFT_CC_ALG_LZ4 ? DICT_LZ4_COST : DICT_ZSTD_COST;
}

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
