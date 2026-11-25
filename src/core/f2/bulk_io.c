/*
 * core/bulk_io.c - batched page writes (core/bulk_io.h).
 */
#include "core/host.h"

#include "core/f2/bulk_io.h"
#include "core/f2/pace.h"
#include "core/extlock.h"
#include "core/f2/meta.h"
#include "core/page_io.h"

void
sift_bulk_extend(Relation rel, const SiftExtent *ext, int next)
{
	BlockNumber end = 0;

	for (int e = 0; e < next; e++)
		end = Max(end, ext[e].start + ext[e].nblocks);
	if (end > RelationGetNumberOfBlocks(rel))
		UnlockReleaseBuffer(sift_extend_zeroed_to(rel, end));
}

void
sift_bulk_begin(SiftPartBulkWriter *w, Relation rel)
{
	*w = (SiftPartBulkWriter) {.rel = rel};
}

Page
sift_bulk_page(SiftPartBulkWriter *w, BlockNumber blk)
{
	if (w->open && w->x.n == MAX_GENERIC_XLOG_PAGES)
		sift_bulk_end(w);
	if (!w->open)
	{
		w->x = sift_xlog_start(w->rel);
		w->open = true;
	}
	return sift_xlog_register(&w->x, sift_blk_new(w->rel, blk), GENERIC_XLOG_FULL_IMAGE);
}

void
sift_bulk_end(SiftPartBulkWriter *w)
{
	if (w->open)
	{
		uint64		npages = w->x.n;

		sift_xlog_done(&w->x);
		sift_f2_pace_pages(npages);
	}
	w->open = false;
}
