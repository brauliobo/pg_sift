/*
 * sift_filter.c - a TIDBitmap as page and offset masks per superblock
 * (sift_filter.h).
 *
 * The bitmap is walked once, in block order, which is also superblock order:
 * a superblock entry is opened by its first page and its exact pages append
 * one offset-mask row each. Lossy pages, and the exact pages that would take
 * more than the byte budget, only set their bit in the lossy page mask.
 */
#include "sift_host.h"

#include "sift_filter.h"

static SiftFilterSB *
open_sb(SiftFilter *f, uint32 sb)
{
	SiftFilterSB *fs;

	if (f->nsb > 0 && f->sb[f->nsb - 1].sb == sb)
		return &f->sb[f->nsb - 1];
	if (f->nsb == f->capsb)
	{
		f->capsb = Max(64, 2 * f->capsb);
		f->sb = f->sb ? repalloc_huge(f->sb, f->capsb * sizeof(SiftFilterSB)) :
			MemoryContextAllocHuge(f->mcxt, f->capsb * sizeof(SiftFilterSB));
	}
	fs = &f->sb[f->nsb++];
	memset(fs, 0, sizeof(SiftFilterSB));
	fs->sb = sb;
	fs->row = f->nrow;
	return fs;
}

static uint64 *
new_row(SiftFilter *f)
{
	if (f->nrow == f->caprow)
	{
		f->caprow = Max(64, 2 * f->caprow);
		f->row = f->row ? repalloc_huge(f->row, f->caprow * sizeof(f->row[0])) :
			MemoryContextAllocHuge(f->mcxt, f->caprow * sizeof(f->row[0]));
	}
	memset(f->row[f->nrow], 0, sizeof(f->row[0]));
	return f->row[f->nrow++];
}

SiftFilter *
sift_filter_build(TIDBitmap *tbm, Size maxbytes, MemoryContext mcxt)
{
	SiftFilter *f = MemoryContextAllocZero(mcxt, sizeof(SiftFilter));
	SiftTbmIter *it = sift_tbm_begin(tbm);
	BlockNumber blockno;
	bool		lossy;
	int			n;
	OffsetNumber offs[MaxHeapTuplesPerPage];
	int64		maxrows = Min(maxbytes / sizeof(f->row[0]), PG_UINT32_MAX);

	f->mcxt = mcxt;
	while (sift_tbm_next(it, &blockno, &lossy, offs, &n))
	{
		int			page = blockno & (SIFT_SB_PAGES - 1);
		uint64		bit = UINT64CONST(1) << (page & 63);
		SiftFilterSB *fs;
		uint64	   *row;

		lossy |= f->nrow >= maxrows;
		if (!lossy && n == 0)
			continue;
		fs = open_sb(f, sift_block_sb(blockno));
		fs->pages[page / 64] |= bit;
		if (lossy)
		{
			fs->lossy[page / 64] |= bit;
			f->lossy_pages++;
			continue;
		}
		row = new_row(f);
		for (int i = 0; i < n; i++)
			row[(offs[i] - 1) / 64] |= UINT64CONST(1) << ((offs[i] - 1) & 63);
		f->exact_pages++;
		f->tuples += n;
	}
	sift_tbm_end(it);
	return f;
}

const SiftFilterSB *
sift_filter_sb(const SiftFilter *f, uint32 sb)
{
	int64		lo = 0,
				hi = f->nsb - 1;

	while (lo <= hi)
	{
		int64		mid = lo + (hi - lo) / 2;

		if (f->sb[mid].sb == sb)
			return &f->sb[mid];
		if (f->sb[mid].sb < sb)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

/* Exact pages of the superblock before page */
static int
exact_before(const SiftFilterSB *fs, int page)
{
	int			n = 0;

	for (int wd = 0; wd < page / 64; wd++)
		n += pg_popcount64(fs->pages[wd] & ~fs->lossy[wd]);
	return n + pg_popcount64(fs->pages[page / 64] & ~fs->lossy[page / 64] & ((UINT64CONST(1) << (page & 63)) - 1));
}

bool
sift_filter_has(const SiftFilter *f, SiftKey key)
{
	const SiftFilterSB *fs = sift_filter_sb(f, sift_block_sb(sift_key_block(key)));
	int			page = sift_key_block(key) & (SIFT_SB_PAGES - 1),
				off = (key & 0xFFFF) - 1;

	if (fs == NULL || !(fs->pages[page / 64] >> (page & 63) & 1))
		return false;
	if (fs->lossy[page / 64] >> (page & 63) & 1)
		return true;
	return f->row[fs->row + exact_before(fs, page)][off / 64] >> (off & 63) & 1;
}

void
sift_filter_block(const SiftFilter *f, const SiftFilterSB *fs, int b, uint64 (*out)[SIFT_OFFMASK_WORDS])
{
	uint64		pm = fs->pages[b / 4] >> ((b % 4) * SIFT_BLK_PAGES) & 0xffff,
				lm = fs->lossy[b / 4] >> ((b % 4) * SIFT_BLK_PAGES) & 0xffff;
	int64		idx = fs->row + exact_before(fs, b * SIFT_BLK_PAGES);

	memset(out, 0, SIFT_BLK_PAGES * sizeof(out[0]));
	for (; pm; pm &= pm - 1)
	{
		int			pg = pg_rightmost_one_pos64(pm);

		if (lm >> pg & 1)
			memset(out[pg], 0xff, sizeof(out[0]));
		else
			memcpy(out[pg], f->row[idx++], sizeof(out[0]));
	}
}
