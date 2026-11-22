/*
 * core/extent.c - the indirect extent list of a part (core/extent.h).
 */
#include "core/host.h"

#include "core/f2/extent.h"
#include "core/f2/fsm.h"
#include "core/page_io.h"

#define EXTL(p)		((SiftExtentListPageData *) SiftPageGetData(p))

BlockNumber
sift_extlist_write(Relation rel, uint64 part_gen, uint32 part_id, const SiftExtent *ext, int n)
{
	int			npages = (n + SIFT_EXTLIST_CAP - 1) / SIFT_EXTLIST_CAP;
	BlockNumber *blks = palloc(npages * sizeof(BlockNumber));
	PGAlignedBlock img;

	for (int p = 0; p < npages; p++)
		blks[p] = sift_fsm_alloc_meta_page(rel);
	for (int p = npages - 1; p >= 0; p--)
	{
		int			from = p * SIFT_EXTLIST_CAP,
					cnt = Min(n - from, (int) SIFT_EXTLIST_CAP);
		SiftXlog	x = sift_xlog_start(rel);
		Page		pg = sift_xlog_register(&x, sift_blk_new(rel, blks[p]), GENERIC_XLOG_FULL_IMAGE);

		sift_page_init_part(img.data, SIFT_PT_EXTLIST, part_gen, part_id);
		EXTL(img.data)->magic = SIFT_EXTLIST_MAGIC;
		EXTL(img.data)->nentries = cnt;
		EXTL(img.data)->next_blk = p + 1 < npages ? blks[p + 1] : InvalidBlockNumber;
		EXTL(img.data)->part_gen = part_gen;
		memcpy(EXTL(img.data)->ext, ext + from, cnt * sizeof(SiftExtent));
		SiftPageSetUsed(img.data, offsetof(SiftExtentListPageData, ext) + cnt * sizeof(SiftExtent));
		sift_page_seal(img.data);
		memcpy(pg, img.data, BLCKSZ);
		sift_xlog_done(&x);
	}
	return blks[0];
}

SiftExtent *
sift_extlist_read(Relation rel, BlockNumber head, uint64 part_gen, uint32 part_id, int n, BlockNumber **pages, int *npages)
{
	SiftExtent *out = palloc(Max(n, 1) * sizeof(SiftExtent));
	int			got = 0,
				np = 0;

	if (pages != NULL)
		*pages = palloc(((n + SIFT_EXTLIST_CAP - 1) / SIFT_EXTLIST_CAP + 1) * sizeof(BlockNumber));
	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);
		Page		page = BufferGetPage(buf);
		SiftExtentListPageData *l = EXTL(page);

		sift_page_expect(rel, page, blk, SIFT_PT_EXTLIST, part_gen, part_id);
		if (l->magic != SIFT_EXTLIST_MAGIC || l->part_gen != part_gen || !sift_page_sum_ok(page) || l->nentries == 0 ||
			l->nentries > SIFT_EXTLIST_CAP || got + l->nentries > n)
			SIFT_CORRUPT(rel, "Block %u: the extent list of part %llu is damaged.", blk, (unsigned long long) part_gen);
		memcpy(out + got, l->ext, l->nentries * sizeof(SiftExtent));
		got += l->nentries;
		if (pages != NULL)
			(*pages)[np] = blk;
		np++;
		blk = l->next_blk;
		UnlockReleaseBuffer(buf);
	}
	if (got != n)
		SIFT_CORRUPT(rel, "The extent list of part %llu holds %d extents, its header says %d.", (unsigned long long) part_gen, got, n);
	if (npages != NULL)
		*npages = np;
	return out;
}
