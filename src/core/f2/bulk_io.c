/*
 * core/bulk_io.c - batched page writes (core/bulk_io.h).
 */
#include "core/host.h"

#include "access/xloginsert.h"
#include "catalog/pg_control.h"

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

static void
log_images(SiftPartBulkWriter *w)
{
	XLogRecPtr	lsn;

	XLogEnsureRecordSpace(w->n - 1, 0);
	XLogBeginInsert();
	for (int i = 0; i < w->n; i++)
		XLogRegisterBuffer((uint8) i, w->buf[i], REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
	lsn = XLogInsert(RM_XLOG_ID, XLOG_FPI);
	for (int i = 0; i < w->n; i++)
		PageSetLSN(BufferGetPage(w->buf[i]), lsn);
}

void
sift_bulk_end(SiftPartBulkWriter *w)
{
	const int	n = w->n;

	for (int i = 0; i < n; i++)
		if (!sift_page_layout_ok(w->page[i].data))
			ereport(ERROR,	/* check-errors: allow */
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("sift index \"%s\": a change of block %u was refused", RelationGetRelationName(w->rel), BufferGetBlockNumber(w->buf[i])),
					 errdetail("the page built is not laid out as a page of the index.")));
	START_CRIT_SECTION();
	for (int i = 0; i < n; i++)
	{
		memcpy(BufferGetPage(w->buf[i]), w->page[i].data, BLCKSZ);
		MarkBufferDirty(w->buf[i]);
	}
	if (n > 0 && RelationNeedsWAL(w->rel))
		log_images(w);
	END_CRIT_SECTION();
	for (int i = 0; i < n; i++)
		UnlockReleaseBuffer(w->buf[i]);
	w->n = 0;
	if (w->page != NULL)
		pfree(w->page);
	w->page = NULL;
	if (n > 0)
		sift_f2_pace_pages(n);
}
