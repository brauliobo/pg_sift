/*
 * core/log_pages.c - new pages enter their buffers together with one full-image WAL record
 */
#include "postgres.h"

#include "access/xloginsert.h"
#include "catalog/pg_control.h"

#include "core/page_io.h"

/*
 * Copies srcs into the exclusively locked bufs and logs them as full images
 * in one record, inside one critical section (P4: content enters the buffer
 * only together with its WAL).
 */
void
sift_log_pages(Relation index, Buffer *bufs, Page *srcs, int n)
{
	bool		wal = RelationNeedsWAL(index);

	Assert(n > 0 && n <= XLR_MAX_BLOCK_ID);
	for (int i = 0; i < n; i++)
		SIFT_INVARIANT(sift_page_layout_ok(srcs[i]), "sift: block %u is logged as a page that is not laid out as one of the index", BufferGetBlockNumber(bufs[i]));
	if (wal)
	{
		XLogEnsureRecordSpace(n - 1, 0);
		XLogBeginInsert();
	}
	START_CRIT_SECTION();
	for (int i = 0; i < n; i++)
	{
		memcpy(BufferGetPage(bufs[i]), srcs[i], BLCKSZ);
		MarkBufferDirty(bufs[i]);
		if (wal)
			XLogRegisterBuffer(i, bufs[i], REGBUF_FORCE_IMAGE | REGBUF_STANDARD);
	}
	if (wal)
	{
		XLogRecPtr	lsn = XLogInsert(RM_XLOG_ID, XLOG_FPI);

		for (int i = 0; i < n; i++)
			PageSetLSN(BufferGetPage(bufs[i]), lsn);
	}
	END_CRIT_SECTION();
}
