/*
 * col/f2/prefetch/pg.c - the read ahead of the granules a scan reaches in a cold part (col/f2/prefetch/prefetch.h): the blocks of the plan go to one ReadStream (core/f2/io_stream.h), which
 * PostgreSQL 18 gives to io_uring or the I/O workers and PostgreSQL 17 to fadvise and vectored reads; each buffer it delivers is released at once, so that the reads of the scan find the pages in
 * shared buffers.
 */
#include "postgres.h"

#include "col/f2/f2.h"
#include "col/f2/marks.h"
#include "col/f2/prefetch/prefetch.h"
#include "col/granule/granule.h"

typedef struct DirRange
{
	const SiftGranuleDesc *dir;
	uint32		ng;
} DirRange;

static bool
dir_range(void *ctx, uint32 g, uint64 *off, uint32 *len)
{
	const DirRange *d = ctx;

	if (g >= d->ng)
		return false;
	*off = d->dir[g].data_off;
	*len = d->dir[g].data_len;
	return true;
}

uint64
sift_f2_prefetch_spans(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, const uint32 spans[][2], int nspans)
{
	const SiftSecDirEntry *gs = sift_part_section(part, SEC_GRANULES);
	DirRange	dr = {.dir = dir,.ng = part->hdr.ngranules};
	SiftPfPlan *plan;
	uint64		pages[SIFT_PF_BATCH_PAGES];
	SiftBlockReq reqs[SIFT_PF_BATCH_PAGES];
	uint64		asked;
	uint32		n;

	if (gs == NULL || dir == NULL || (part->hdr.flags & SIFT_PART_FLAG_INLINE_STREAM))
		return 0;
	plan = sift_pf_plan(dir_range, &dr, gs->off, SIFT_PAGE_PAYLOAD, spans, nspans);
	if (plan == NULL)
		return 0;
	asked = plan->total_pages;
	while ((n = sift_pf_batch(plan, SIFT_PF_BATCH_PAGES, pages)) > 0)
	{
		SiftExtentReadStream ios;
		SiftIoBufMeta meta;
		Buffer		buf;

		for (uint32 i = 0; i < n; i++)
			reqs[i] = (SiftBlockReq) {.blkno = sift_part_page_block(part, pages[i]),.logical_page = (uint32) pages[i]};
		sift_ios_begin_reqs(&ios, rel, reqs, n, SIFT_PT_DATA, part->hdr.part_gen, part->hdr.part_id, SIFT_IOS_SELECTIVE);
		while ((buf = sift_ios_next(&ios, &meta)) != InvalidBuffer)
			UnlockReleaseBuffer(buf);
		sift_ios_end(&ios);
	}
	sift_pf_free(plan);
	return asked;
}
