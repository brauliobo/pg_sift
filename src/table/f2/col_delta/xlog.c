/*
 * table/f2/col_delta/xlog.c - the append in place to the newest page of a column delta chain and its replay (table/f2/col_delta/xlog.h).
 *
 * The record registers the data page as block 0 and the directory page as block 1, both standard pages (their hole is the free space of the page). The writer changes the pages in its critical
 * section with the functions the redo calls, so a replayed page is the page the writer made; a page already past the record (its LSN) is left as it is.
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"

#include "core/damage.h"
#include "core/f2/vm_xlog.h"
#include "core/xlog.h"
#include "table/f2/col_delta/dir.h"
#include "table/f2/col_delta/xlog.h"

/* The batch of the record without its bytes, which follow it */
typedef struct SiftXlCdAppend
{
	uint32		nbytes;
	uint16		nentries;
	uint16		page_flags;
	uint16		dir_flags;
	uint16		pad;
	uint32		granule_lo;
	uint32		granule_hi;
	uint32		pad2;
	uint64		part_gen;
	FullTransactionId min_xid;
	FullTransactionId max_xid;
	uint64		part_gen_bloom;
	uint64		attnum_bloom;
	uint64		tid_bloom;
} SiftXlCdAppend;

StaticAssertDecl(sizeof(SiftXlCdAppend) == 72, "SiftXlCdAppend must be 72 bytes");

static void
apply(Page page, Page dpage, const SiftColDeltaBatch *b)
{
	SIFT_INVARIANT(sift_col_delta_page_append(page, b), "a column delta batch that fit the page did not");
	if (dpage != NULL)
		sift_cddir_page_widen_last(dpage, b);
}

/* Without the resource manager (the library was not preloaded): the same change as one of generic WAL */
static void
append_generic(Relation rel, Buffer buf, Buffer dbuf, const SiftColDeltaBatch *b)
{
	SiftXlog	x = sift_xlog_start(rel);
	Page		page = sift_xlog_register(&x, buf, 0);

	apply(page, dbuf != InvalidBuffer ? sift_xlog_register(&x, dbuf, 0) : NULL, b);
	sift_xlog_finish(&x);
}

void
sift_col_delta_xlog_append(Relation rel, Buffer buf, Buffer dbuf, const SiftColDeltaBatch *b)
{
	SiftXlCdAppend rec = {.nbytes = b->nbytes,.nentries = b->nentries,.page_flags = b->page_flags,.dir_flags = b->dir_flags,.granule_lo = b->granule_lo,.granule_hi = b->granule_hi,
		.part_gen = b->part_gen,.min_xid = b->min_xid,.max_xid = b->max_xid,.part_gen_bloom = b->part_gen_bloom,.attnum_bloom = b->attnum_bloom,.tid_bloom = b->tid_bloom};
	Page		page = BufferGetPage(buf);
	uint32		used = SiftPageGetUsed(page);

	if (RelationNeedsWAL(rel) && !sift_vm_rmgr_active())
	{
		append_generic(rel, buf, dbuf, b);
		return;
	}
	SIFT_INVARIANT((sift_col_delta_page_hdr(page)->flags & SIFT_CDPG_SEALED) == 0 && used + b->nbytes <= SIFT_PAGE_PAYLOAD, "a column delta batch the caller fitted does not fit its page");
	START_CRIT_SECTION();
	apply(page, dbuf != InvalidBuffer ? BufferGetPage(dbuf) : NULL, b);
	MarkBufferDirty(buf);
	if (dbuf != InvalidBuffer)
		MarkBufferDirty(dbuf);
	if (RelationNeedsWAL(rel))
	{
		XLogRecPtr	lsn;

		XLogBeginInsert();
		XLogRegisterData((char *) &rec, sizeof(rec));
		XLogRegisterData((char *) b->bytes, b->nbytes);
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		if (dbuf != InvalidBuffer)
			XLogRegisterBuffer(1, dbuf, REGBUF_STANDARD);
		lsn = XLogInsert(SIFT_RMGR_ID, SIFT_XLOG_CD_APPEND);
		PageSetLSN(page, lsn);
		if (dbuf != InvalidBuffer)
			PageSetLSN(BufferGetPage(dbuf), lsn);
	}
	END_CRIT_SECTION();
}

static SiftColDeltaBatch
batch_of(XLogReaderState *r)
{
	const SiftXlCdAppend *rec = (const SiftXlCdAppend *) XLogRecGetData(r);

	if (XLogRecGetDataLen(r) < sizeof(SiftXlCdAppend) || XLogRecGetDataLen(r) != sizeof(SiftXlCdAppend) + rec->nbytes)
		SIFT_DAMAGE_OTHER_DATA(errmsg("sift column delta append record of %u bytes", XLogRecGetDataLen(r)));
	return (SiftColDeltaBatch) {.bytes = (uint8 *) (rec + 1),.nbytes = rec->nbytes,.nentries = rec->nentries,.page_flags = rec->page_flags,.dir_flags = rec->dir_flags,
		.part_gen = rec->part_gen,.granule_lo = rec->granule_lo,.granule_hi = rec->granule_hi,.min_xid = rec->min_xid,.max_xid = rec->max_xid,.part_gen_bloom = rec->part_gen_bloom,
	.attnum_bloom = rec->attnum_bloom,.tid_bloom = rec->tid_bloom};
}

void
sift_col_delta_append_redo(XLogReaderState *r)
{
	SiftColDeltaBatch b = batch_of(r);
	Buffer		buf,
				dbuf = InvalidBuffer;

	if (XLogReadBufferForRedo(r, 0, &buf) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);

		SIFT_INVARIANT(sift_col_delta_page_append(page, &b), "a replayed column delta batch does not fit its page");
		PageSetLSN(page, r->EndRecPtr);
		MarkBufferDirty(buf);
	}
	if (XLogRecHasBlockRef(r, 1) && XLogReadBufferForRedo(r, 1, &dbuf) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(dbuf);

		sift_cddir_page_widen_last(page, &b);
		PageSetLSN(page, r->EndRecPtr);
		MarkBufferDirty(dbuf);
	}
	if (BufferIsValid(dbuf))
		UnlockReleaseBuffer(dbuf);
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

void
sift_col_delta_append_desc(StringInfo buf, XLogReaderState *r)
{
	const SiftXlCdAppend *rec = (const SiftXlCdAppend *) XLogRecGetData(r);

	if (XLogRecGetDataLen(r) < sizeof(SiftXlCdAppend))
		appendStringInfo(buf, "short record of %u bytes", XLogRecGetDataLen(r));
	else
		appendStringInfo(buf, "bytes %u entries %u part_gen %llu granules %u-%u flags 0x%04X dir_flags 0x%04X", rec->nbytes, rec->nentries, (unsigned long long) rec->part_gen,
						 rec->granule_lo, rec->granule_hi, rec->page_flags, rec->dir_flags);
}
