/*
 * core/relocate.c - space mode: moves a segment into free blocks below a limit, page by page.
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/nbtxlog.h"
#include "access/parallel.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_class.h"
#include "catalog/storage.h"
#include "storage/procarray.h"
#include "storage/sinval.h"
#include "catalog/pg_control.h"
#include "common/int.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "storage/bulk_write.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#include "col/col.h"
#include "bm25/estimate.h"
#include "core/lane.h"
#include "core/store.h"
#include "core/store_int.h"

/* Remap a block while preserving its position inside the original extent. */
BlockNumber
sift_store_relocated_block(BlockNumber blk, const SiftExtent *old, const SiftExtent *fresh, int n)
{
	if (!BlockNumberIsValid(blk))
		return blk;
	for (int i = 0; i < n; i++)
		if (blk >= old[i].start && blk - old[i].start < old[i].nblocks)
			return fresh[i].start + (blk - old[i].start);
	elog(ERROR, "sift: relocation references a block outside its segment");
	pg_unreachable();
}

/* Lowest fitting run, shared with allocation's eligibility and placement. */
static int
relocation_pick(const FreeChain *free, uint32 min, BlockNumber limit)
{
	int best = -1;

	for (int k = 0; k < free->n; k++)
		if (space_fits(&free->ext[k], min, limit) &&
			(best < 0 || free->ext[k].start < free->ext[best].start))
			best = k;
	return best;
}

/*
 * Copy stored pages without decoding, format conversion, promotion or growing stream reservations. Preflight every allocation
 * with the same lowest-fit rule as alloc_blocks. DATA and lane runs can split across holes; a LIVE run must remain contiguous.
 * Plan fixed runs first, then data in logical order, and verify the resulting stream directory fits its existing header page.
 * No accepted move, including its free-list publication, needs to extend the file. The caller atomically publishes the copy.
 */
uint64
sift_store_relocate(Relation rel, SiftWriteTarget *wt, const SiftSegEntry *entry, const SiftExtent *old, int n,
					BlockNumber limit, uint64 budget, SiftSegEntry *entry_new, SiftRelocatePage relocate_page)
{
	Buffer metabuf;
	FreeChain free;
	SiftExtent *source;
	SiftExtent *fresh;
	int parts = 0;
	int data_at = 0;
	int ndata = entry->kind == SIFT_SEG_IMMUTABLE ? n - (old[n - 1].start == entry->header_blk ? 1 : 2) : 0;
	uint64 blocks = 0;
	int capacity;
	uint64 gen;
	PGIOAlignedBlock copy;

	Assert(space_exclusive);
	for (int i = 0; i < n; i++)
		blocks += old[i].nblocks;
	if (blocks > budget / BLCKSZ)
		return 0;
	capacity = entry->kind == SIFT_SEG_IMMUTABLE ? SIFT_MAX_EXTENTS + 2 : blocks;
	source = palloc(capacity * sizeof(SiftExtent));
	fresh = palloc(capacity * sizeof(SiftExtent));
	metabuf = lock_meta(rel, BUFFER_LOCK_SHARE);
	read_chain(rel, metabuf, &free, InvalidFullTransactionId);
	UnlockReleaseBuffer(metabuf);
	/* Immutable streams put header/LIVE last in old; reserve those exact runs before filling the small holes with data. */
	for (int phase = 0; phase < 2; phase++)
	{
		int first = phase == 0 ? ndata : 0;
		int end = phase == 0 ? n : ndata;

		if (phase == 1)
			data_at = parts;
		for (int at = first; at < end; at++)
		{
			/* LIVE precedes the one-page header reservation, so that the header cannot fragment its only fitting run. */
			int i = phase == 0 && entry->kind == SIFT_SEG_IMMUTABLE ? n - 1 - (at - first) : at;
			SiftExtent remaining = old[i];
			bool split = phase == 1 || entry->kind != SIFT_SEG_IMMUTABLE;

			while (remaining.nblocks > 0)
			{
				int best = relocation_pick(&free, split ? 1 : remaining.nblocks, limit);
				uint32 take;

				if (best < 0 || parts == capacity)
					goto skip;
				take = Min(remaining.nblocks, Min(free.ext[best].nblocks, limit - free.ext[best].start));
				source[parts] = (SiftExtent) {remaining.start, take};
				fresh[parts++] = (SiftExtent) {free.ext[best].start, take};
				free.ext[best].start += take;
				free.ext[best].nblocks -= take;
				remaining.start += take;
				remaining.nblocks -= take;
			}
		}
	}
	if (entry->kind == SIFT_SEG_IMMUTABLE && parts - data_at > SIFT_MAX_EXTENTS)
		goto skip;
	/* Leave existing free blocks for publication's worst-case free-list page count, too. */
	{
		uint64 spare = 0;

		for (int k = 0; k < free.n; k++)
			spare += free.ext[k].nblocks;
		if (spare < (n + SIFT_FREELIST_CAP - 1) / SIFT_FREELIST_CAP)
			goto skip;
	}
	pfree(free.ext);
	pfree(free.page);
	gen = sift_store_new_gen(rel);
	space_limit = limit;
	for (int i = 0; i < parts; i++)
	{
		BlockNumber reserved = sift_wt_reserve(wt, source[i].nblocks);

		if (reserved != fresh[i].start)
			elog(ERROR, "sift: relocation allocation differs from its plan");
	}
	space_limit = InvalidBlockNumber;
	for (int i = 0; i < parts; i++)
		for (uint32 j = 0; j < source[i].nblocks; j++)
		{
			BlockNumber blk = source[i].start + j;
			Buffer buf = read_locked(rel, blk, BUFFER_LOCK_SHARE);
			Page page = copy.data;
			SiftPageType type = blk == entry->header_blk ?
				(entry->kind == SIFT_SEG_IMMUTABLE ? SIFT_PT_SEGHEADER : SIFT_PT_WBUFHEADER) :
				(entry->kind == SIFT_SEG_IMMUTABLE ? (i < data_at ? SIFT_PT_LIVE : SIFT_PT_DATA) : SIFT_PT_WBUFDATA);

			sift_store_check_page(rel, buf, type, entry->gen);
			memcpy(page, BufferGetPage(buf), BLCKSZ);
			UnlockReleaseBuffer(buf);
			SiftPageGetOpaque(page)->gen = gen;
			if (type == SIFT_PT_SEGHEADER)
			{
				SiftSegHeaderData *h = (SiftSegHeaderData *) SiftPageGetData(page);

				h->gen = gen;
				h->nextents = parts - data_at;
				memcpy(h->extents, fresh + data_at, h->nextents * sizeof(SiftExtent));
				SiftPageSetUsed(page, offsetof(SiftSegHeaderData, extents) + h->nextents * sizeof(SiftExtent));
				if (h->live_npages > 0)
					h->live_first = sift_store_relocated_block(h->live_first, source, fresh, parts);
			}
			else if (type == SIFT_PT_WBUFHEADER)
			{
				SiftWbufHeaderData *h = SiftLaneHdr(page);

				h->gen = gen;
				for (int k = 0; k < SIFT_LANES; k++)
				{
					h->lanes[k].head = sift_store_relocated_block(h->lanes[k].head, source, fresh, parts);
					h->lanes[k].tail = sift_store_relocated_block(h->lanes[k].tail, source, fresh, parts);
				}
			}
			else if (type == SIFT_PT_WBUFDATA)
				SiftLaneHead(page)->next = sift_store_relocated_block(SiftLaneHead(page)->next, source, fresh, parts);
			if (relocate_page && (entry->flags & SIFT_TBL_SEG_WBUF))
				relocate_page(page, source, fresh, parts);
			sift_wt_put_page(wt, fresh[i].start + j, page);
			CHECK_FOR_INTERRUPTS();
		}
	sift_wt_finish(wt);
	*entry_new = *entry;
	entry_new->gen = gen;
	entry_new->header_blk = sift_store_relocated_block(entry->header_blk, source, fresh, parts);
	pfree(source);
	pfree(fresh);
	space_written += blocks;
	return blocks * BLCKSZ;
skip:
	pfree(free.ext);
	pfree(free.page);
	pfree(source);
	pfree(fresh);
	return 0;
}
