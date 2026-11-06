/*
 * col/scan_hfblocks.c - what the tombstone log says of each heap block a scan asks about, kept by the scan (the use is in scan_unit.c).
 *
 * The units of a key-cut segment list the same pages again and again (a page holds the rows of many keys), and each ask bisects the log four times and walks the entries of the
 * page for their verdicts: for the statement over the whole table that is most of its 2,600 instructions for each logged row. The answer depends on the snapshot and the log,
 * which are the scan's. A block asked about once is not worth an entry (a statement over one key asks about thousands of them once): the second ask makes it.
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "utils/memutils.h"

#include "col/am.h"
#include "core/tomb.h"
#include "col/scan_int.h"

#define HF_BLOCKS_MAX	(1 << 17)

/* heap blocks the scan remembers asking about: a bit each, up to 2 MB */
#define HF_SEEN_MAX		(1 << 24)

int64		sift_col_hf_block_hits;

typedef struct ColHfBlocks
{
	uint64		gen;
	uint32		cap,
				n,
				nseen;
	ColHfBlock *e;
	uint64	   *seen;
} ColHfBlocks;

static void
hf_blocks_make(SiftColSource *src, uint32 cap)
{
	ColHfBlocks *old = src->hfblocks;
	ColHfBlocks *c = MemoryContextAllocZero(src->mcxt, sizeof(ColHfBlocks));

	c->gen = src->hfgen;
	c->cap = cap;
	c->e = MemoryContextAllocZero(src->mcxt, cap * sizeof(ColHfBlock));
	if (old != NULL && old->gen == c->gen)
	{
		c->seen = old->seen;
		c->nseen = old->nseen;
		old->seen = NULL;
	}
	if (old != NULL && old->gen == c->gen)
		for (uint32 i = 0; i < old->cap; i++)
			if (old->e[i].used)
			{
				uint32		j = hash_uint32(old->e[i].blk) & (cap - 1);

				while (c->e[j].used)
					j = (j + 1) & (cap - 1);
				c->e[j] = old->e[i];
				c->n++;
			}
	if (old != NULL)
	{
		if (old->seen != NULL)
			pfree(old->seen);
		pfree(old->e);
		pfree(old);
	}
	src->hfblocks = c;
}

/* The entry of blk, made if there is room (NULL: the table is full, the caller asks the log each time) */
static ColHfBlock *
col_hf_block(SiftColSource *src, BlockNumber blk)
{
	ColHfBlocks *c = src->hfblocks;
	uint32		i;

	if (c == NULL || c->gen != src->hfgen)
	{
		src->hfblocks = NULL;
		hf_blocks_make(src, 64);
		c = src->hfblocks;
	}
	/* a block asked about once is not worth an entry (a statement over one key asks about thousands of them once): the second ask makes it */
	if (blk >= HF_SEEN_MAX)
		return NULL;
	if (blk / 64 >= c->nseen)
	{
		uint32		n = Max(blk / 64 + 1, c->nseen * 2);
		uint64	   *s = MemoryContextAllocZero(src->mcxt, n * sizeof(uint64));

		if (c->nseen > 0)
		{
			memcpy(s, c->seen, c->nseen * sizeof(uint64));
			pfree(c->seen);
		}
		c->seen = s;
		c->nseen = n;
	}
	if (!(c->seen[blk / 64] >> (blk % 64) & 1))
	{
		c->seen[blk / 64] |= UINT64CONST(1) << (blk % 64);
		return NULL;
	}
	for (i = hash_uint32(blk) & (c->cap - 1); c->e[i].used; i = (i + 1) & (c->cap - 1))
		if (c->e[i].blk == blk)
			return &c->e[i];
	if (c->n >= c->cap / 2)
	{
		if (c->cap >= HF_BLOCKS_MAX)
			return NULL;
		hf_blocks_make(src, c->cap * 2);
		c = src->hfblocks;
		for (i = hash_uint32(blk) & (c->cap - 1); c->e[i].used; i = (i + 1) & (c->cap - 1))
			;
	}
	c->e[i].used = true;
	c->e[i].blk = blk;
	c->n++;
	return &c->e[i];
}


/* col_hf_deleted for one block, without the mask */
bool
col_hf_block_any(SiftColSource *src, BlockNumber blk)
{
	ColHfBlock *b = col_hf_block(src, blk);

	if (b == NULL)
		return col_hf_deleted(src, blk, 1, NULL);
	if (!(b->have & HF_ANY))
	{
		b->any = col_hf_deleted(src, blk, 1, NULL);
		b->have |= HF_ANY;
	}
	else
		sift_col_hf_block_hits++;
	return b->any;
}

/* The rows of the block the log deletes for the snapshot, in gone (zeroed by the caller); false when the log has too many entries on the block to read (the caller asks the heap) */
bool
col_hf_block_gone(SiftColSource *src, BlockNumber blk, uint64 *gone)
{
	ColHfBlock *b = col_hf_block(src, blk);
	bool		ok;

	if (b != NULL && (b->have & HF_GONE))
	{
		sift_col_hf_block_hits++;
		memcpy(gone, b->gone, sizeof(b->gone));
		return b->ok;
	}
	ok = sift_tomb_deleted_offsets(src->hflog, src->snap, blk, 4096, gone, &src->hfmemo);
	if (b != NULL)
	{
		b->have |= HF_GONE;
		b->ok = ok;
		memcpy(b->gone, gone, sizeof(b->gone));
	}
	return ok;
}
