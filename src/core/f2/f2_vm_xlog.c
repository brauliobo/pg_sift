/*
 * core/f2/f2_vm_xlog.c - the changes of the visibility map of a sift_table (core/f2/f2_vm_xlog.h): the page edits shared by the writer and the redo, the records, the replay.
 *
 * Both records register the page of the map as block 0 without the hole of a standard page: the bits sit where a standard page has its hole.
 */
#include "postgres.h"

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/standby.h"
#include "utils/rel.h"

#include "core/f2/f2_vm_xlog.h"
#include "core/host.h"

static bool rmgr_registered;

bool
sift_vm_rmgr_active(void)
{
	return rmgr_registered;
}

static uint8 *
map_of(Page page)
{
	return (uint8 *) PageGetContents(page);
}

uint64
sift_vm_page_word(Page page, int word)
{
	const uint8 *map = map_of(page) + (Size) word * sizeof(uint64);
	uint64		w = 0;

	for (int b = 0; b < (int) sizeof(uint64); b++)
		w |= (uint64) map[b] << (b * 8);
	return w;
}

/* No bit is set: the page is what the redo builds from nothing (PageInit, then the bits of the record) */
static bool
page_blank(Page page)
{
	for (int w = 0; w < SIFT_VM_WORDS_PER_PAGE; w++)
		if (sift_vm_page_word(page, w) != 0)
			return false;
	return true;
}

static void
page_or(Page page, const uint16 *word_idx, const uint64 *bit_or, int nwords)
{
	uint8	   *map = map_of(page);

	for (int i = 0; i < nwords; i++)
	{
		Assert(word_idx[i] < SIFT_VM_WORDS_PER_PAGE);
		for (int b = 0; b < (int) sizeof(uint64); b++)
			map[word_idx[i] * sizeof(uint64) + b] |= (uint8) (bit_or[i] >> (b * 8));
	}
}

static uint8
page_bits(Page page, BlockNumber vblock)
{
	uint64		in_page = vblock % SIFT_VM_BLOCKS_PER_PAGE;

	return (map_of(page)[in_page / SIFT_VM_BLOCKS_PER_BYTE] >> ((in_page % SIFT_VM_BLOCKS_PER_BYTE) * BITS_PER_HEAPBLOCK)) & VISIBILITYMAP_VALID_BITS;
}

static void
page_clear(Page page, BlockNumber vblock, uint8 flags)
{
	uint64		in_page = vblock % SIFT_VM_BLOCKS_PER_PAGE;

	map_of(page)[in_page / SIFT_VM_BLOCKS_PER_BYTE] &= ~(uint8) (flags << ((in_page % SIFT_VM_BLOCKS_PER_BYTE) * BITS_PER_HEAPBLOCK));
}

static XLogRecPtr
log_set(Relation rel, Buffer vmbuf, uint8 flags, TransactionId horizon, const uint16 *word_idx, const uint64 *bit_or, int nwords, bool init)
{
	SiftXlVmSet rec = {.locator = rel->rd_locator,.vm_blkno = BufferGetBlockNumber(vmbuf),.snapshotConflictHorizon = horizon,.nwords = nwords,.flags = flags};

	XLogBeginInsert();
	XLogRegisterData((char *) &rec, sizeof(rec));
	XLogRegisterData((char *) word_idx, nwords * sizeof(uint16));
	XLogRegisterData((char *) bit_or, nwords * sizeof(uint64));
	XLogRegisterBuffer(0, vmbuf, init ? REGBUF_WILL_INIT : 0);
	return XLogInsert(SIFT_RMGR_ID, SIFT_XLOG_VM_SET);
}

bool
sift_vm_apply_set(Relation rel, Buffer vmbuf, uint8 flags, TransactionId horizon, const uint16 *word_idx, const uint64 *bit_or, int nwords)
{
	Page		page;
	uint16		idx[SIFT_VM_WORDS_PER_PAGE];
	uint64		bits[SIFT_VM_WORDS_PER_PAGE];
	int			n = 0;
	bool		init;

	SIFT_INVARIANT(nwords <= lengthof(idx), "a page of the map has no more words than its changes name");
	LockBuffer(vmbuf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(vmbuf);
	init = page_blank(page);
	for (int i = 0; i < nwords; i++)
		if ((sift_vm_page_word(page, word_idx[i]) | bit_or[i]) != sift_vm_page_word(page, word_idx[i]))
		{
			idx[n] = word_idx[i];
			bits[n++] = bit_or[i];
		}
	if (n > 0)
	{
		START_CRIT_SECTION();
		if (init)
			PageInit(page, BLCKSZ, 0);
		page_or(page, idx, bits, n);
		MarkBufferDirty(vmbuf);
		if (RelationNeedsWAL(rel))
			PageSetLSN(page, log_set(rel, vmbuf, flags, horizon, idx, bits, n, init));
		END_CRIT_SECTION();
	}
	LockBuffer(vmbuf, BUFFER_LOCK_UNLOCK);
	return n > 0;
}

bool
sift_vm_apply_clear(Relation rel, Buffer vmbuf, BlockNumber vblock)
{
	Page		page;
	uint8		bits;

	LockBuffer(vmbuf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(vmbuf);
	bits = PageIsNew(page) ? 0 : page_bits(page, vblock);
	if (bits != 0)
	{
		SiftXlVmClear rec = {.locator = rel->rd_locator,.vblock = vblock,.flags = VISIBILITYMAP_VALID_BITS};

		START_CRIT_SECTION();
		page_clear(page, vblock, VISIBILITYMAP_VALID_BITS);
		MarkBufferDirty(vmbuf);
		if (RelationNeedsWAL(rel))
		{
			XLogBeginInsert();
			XLogRegisterData((char *) &rec, sizeof(rec));
			XLogRegisterBuffer(0, vmbuf, 0);
			PageSetLSN(page, XLogInsert(SIFT_RMGR_ID, SIFT_XLOG_VM_CLEAR));
		}
		END_CRIT_SECTION();
	}
	LockBuffer(vmbuf, BUFFER_LOCK_UNLOCK);
	return bits != 0;
}

/* The record builds the page it names (the first bits of a page of the map): redo zeroes it and sets the bits, with no image of the page */
static bool
builds_page(XLogReaderState *r)
{
	return (XLogRecGetBlock(r, 0)->flags & BKPBLOCK_WILL_INIT) != 0;
}

/* The data of the record, which holds at least len bytes */
static const void *
record_data(XLogReaderState *r, Size len)
{
	if (XLogRecGetDataLen(r) < len)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("sift visibility map record of %u bytes where %zu are needed", XLogRecGetDataLen(r), len)));
	return XLogRecGetData(r);
}

static void
redo_set(XLogReaderState *r)
{
	const SiftXlVmSet *rec = record_data(r, sizeof(SiftXlVmSet));
	const uint16 *word_idx;
	const char *packed;
	Buffer		buf;

	if (rec->nwords > SIFT_VM_WORDS_PER_PAGE)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("sift visibility map record with %u words", rec->nwords)));
	(void) record_data(r, sizeof(SiftXlVmSet) + (Size) rec->nwords * (sizeof(uint16) + sizeof(uint64)));
	word_idx = (const uint16 *) (rec + 1);
	packed = (const char *) (word_idx + rec->nwords);

	/* the bits tell a query that a row is there for it without asking the table: one whose snapshot is not past every transaction they stand for must go first */
	if (InHotStandby && TransactionIdIsNormal(rec->snapshotConflictHorizon))
		ResolveRecoveryConflictWithSnapshot(rec->snapshotConflictHorizon, false, rec->locator);
	if (XLogReadBufferForRedoExtended(r, 0, builds_page(r) ? RBM_ZERO_AND_LOCK : RBM_ZERO_ON_ERROR, false, &buf) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);
		uint64		bits[SIFT_VM_WORDS_PER_PAGE];

		for (int i = 0; i < rec->nwords; i++)
		{
			if (word_idx[i] >= SIFT_VM_WORDS_PER_PAGE)
				elog(PANIC, "sift visibility map record with word %u", word_idx[i]);
			memcpy(&bits[i], packed + i * sizeof(uint64), sizeof(uint64));
		}
		if (PageIsNew(page))
			PageInit(page, BLCKSZ, 0);
		page_or(page, word_idx, bits, rec->nwords);
		PageSetLSN(page, r->EndRecPtr);
		MarkBufferDirty(buf);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static void
redo_clear(XLogReaderState *r)
{
	const SiftXlVmClear *rec = record_data(r, sizeof(SiftXlVmClear));
	Buffer		buf;

	if (XLogReadBufferForRedoExtended(r, 0, RBM_ZERO_ON_ERROR, false, &buf) == BLK_NEEDS_REDO)
	{
		Page		page = BufferGetPage(buf);

		if (PageIsNew(page))
			PageInit(page, BLCKSZ, 0);
		page_clear(page, rec->vblock, rec->flags);
		PageSetLSN(page, r->EndRecPtr);
		MarkBufferDirty(buf);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
}

static void
vm_redo(XLogReaderState *r)
{
	switch (XLogRecGetInfo(r) & ~XLR_INFO_MASK)
	{
		case SIFT_XLOG_VM_SET:
			redo_set(r);
			break;
		case SIFT_XLOG_VM_CLEAR:
			redo_clear(r);
			break;
		default:
			elog(PANIC, "sift_vm_redo: unknown op code %u", XLogRecGetInfo(r) & ~XLR_INFO_MASK);
	}
}

#ifdef SIFT_TESTING
void
sift_vm_redo_test(XLogReaderState *r)
{
	vm_redo(r);
}
#endif

static void
vm_desc(StringInfo buf, XLogReaderState *r)
{
	const char *rec = XLogRecGetData(r);
	const bool	set = (XLogRecGetInfo(r) & ~XLR_INFO_MASK) == SIFT_XLOG_VM_SET;

	if (XLogRecGetDataLen(r) < (set ? sizeof(SiftXlVmSet) : sizeof(SiftXlVmClear)))
		appendStringInfo(buf, "short record of %u bytes", XLogRecGetDataLen(r));
	else if (set)
	{
		const SiftXlVmSet *s = (const SiftXlVmSet *) rec;

		appendStringInfo(buf, "rel %u/%u/%u page %u words %u horizon %u flags 0x%02X", s->locator.spcOid, s->locator.dbOid, s->locator.relNumber, s->vm_blkno, s->nwords,
						 s->snapshotConflictHorizon, s->flags);
	}
	else
	{
		const SiftXlVmClear *c = (const SiftXlVmClear *) rec;

		appendStringInfo(buf, "rel %u/%u/%u vblock %u flags 0x%02X", c->locator.spcOid, c->locator.dbOid, c->locator.relNumber, c->vblock, c->flags);
	}
}

static const char *
vm_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case SIFT_XLOG_VM_SET:
			return "VM_SET";
		case SIFT_XLOG_VM_CLEAR:
			return "VM_CLEAR";
	}
	return NULL;
}

void
sift_vm_xlog_init(void)
{
	static const RmgrData rmgr = {.rm_name = SIFT_RMGR_NAME,.rm_redo = vm_redo,.rm_desc = vm_desc,.rm_identify = vm_identify};

	if (!process_shared_preload_libraries_in_progress)
		return;
	RegisterCustomRmgr(SIFT_RMGR_ID, &rmgr);
	rmgr_registered = true;
}
