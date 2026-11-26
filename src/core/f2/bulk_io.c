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
	if (w->n == SIFT_BULK_PAGES)
		sift_bulk_end(w);
	if (w->page == NULL)
		w->page = palloc_aligned(sizeof(PGIOAlignedBlock) * SIFT_BULK_PAGES, PG_IO_ALIGN_SIZE, 0);
	w->buf[w->n] = sift_blk_new(w->rel, blk);
	memset(w->page[w->n].data, 0, BLCKSZ);
	return w->page[w->n++].data;
}

void
sift_bulk_end(SiftPartBulkWriter *w)
{
	const int	n = w->n;
	Page		pages[SIFT_BULK_PAGES];

	for (int i = 0; i < n; i++)
		pages[i] = w->page[i].data;
	if (n > 0)
		sift_log_pages(w->rel, w->buf, pages, n);
	for (int i = 0; i < n; i++)
		UnlockReleaseBuffer(w->buf[i]);
	w->n = 0;
	if (w->page != NULL)
		pfree(w->page);
	w->page = NULL;
	if (n > 0)
		sift_f2_pace_pages(n);
}
