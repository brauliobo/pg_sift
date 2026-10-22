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
	SIFT_INVARIANT(false, "sift: relocation references a block outside its segment");
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

/* The copy of a segment: the extents it is read from and the free runs it is written to, in the same order. */
typedef struct RelocPlan
{
	SiftExtent *source;
	SiftExtent *fresh;
	int			parts;
	int			data_at;		/* parts before it are fixed runs (an immutable segment's LIVE and header); the data runs follow */
} RelocPlan;

/*
 * Reserve the runs of the copy in the free list with the lowest-fit rule of alloc_blocks. An immutable stream puts header/LIVE last in old:
 * reserve those exact runs first, LIVE before the one-page header so that the header cannot fragment its only fitting run, then the data.
 * DATA and lane runs can split across holes; a LIVE run must remain contiguous. False if a run does not fit below the limit.
 */
static bool
relocation_reserve(FreeChain *free, const SiftSegEntry *entry, const SiftExtent *old, int n, BlockNumber limit, int capacity, RelocPlan *plan)
{
	bool immutable = entry->kind == SIFT_SEG_IMMUTABLE;
	int ndata = immutable ? n - (old[n - 1].start == entry->header_blk ? 1 : 2) : 0;

	for (int phase = 0; phase < 2; phase++)
	{
		int first = phase == 0 ? ndata : 0;
		int end = phase == 0 ? n : ndata;

		if (phase == 1)
			plan->data_at = plan->parts;
		for (int at = first; at < end; at++)
		{
			int i = phase == 0 && immutable ? n - 1 - (at - first) : at;
			SiftExtent remaining = old[i];
			bool split = phase == 1 || !immutable;

			while (remaining.nblocks > 0)
			{
				int best = relocation_pick(free, split ? 1 : remaining.nblocks, limit);
				uint32 take;

				if (best < 0 || plan->parts == capacity)
					return false;
				take = Min(remaining.nblocks, Min(free->ext[best].nblocks, limit - free->ext[best].start));
				plan->source[plan->parts] = (SiftExtent) {remaining.start, take};
				plan->fresh[plan->parts++] = (SiftExtent) {free->ext[best].start, take};
				free->ext[best].start += take;
				free->ext[best].nblocks -= take;
				remaining.start += take;
				remaining.nblocks -= take;
			}
		}
	}
	return !immutable || plan->parts - plan->data_at <= SIFT_MAX_EXTENTS;
}

/* Whether the free blocks left after the plan cover publication's worst-case free-list page count. */
static bool
relocation_leaves_spare(const FreeChain *free, int n)
{
	uint64 spare = 0;

	for (int k = 0; k < free->n; k++)
		spare += free->ext[k].nblocks;
	return spare >= (n + SIFT_FREELIST_CAP - 1) / SIFT_FREELIST_CAP;
}

/* Plan the copy of the segment (blocks in total) against the free list as it is now; false leaves nothing allocated. */
static bool
relocation_plan(Relation rel, const SiftSegEntry *entry, const SiftExtent *old, int n, uint64 blocks, BlockNumber limit, RelocPlan *plan)
{
	int capacity = entry->kind == SIFT_SEG_IMMUTABLE ? SIFT_MAX_EXTENTS + 2 : blocks;
	Buffer metabuf;
	FreeChain free;
	bool fits;

	*plan = (RelocPlan) {.source = palloc(capacity * sizeof(SiftExtent)),.fresh = palloc(capacity * sizeof(SiftExtent))};
	metabuf = lock_meta(rel, BUFFER_LOCK_SHARE);
	read_chain(rel, metabuf, &free, InvalidFullTransactionId);
	UnlockReleaseBuffer(metabuf);
	fits = relocation_reserve(&free, entry, old, n, limit, capacity, plan) && relocation_leaves_spare(&free, n);
	pfree(free.ext);
	pfree(free.page);
	if (!fits)
	{
		pfree(plan->source);
		pfree(plan->fresh);
	}
	return fits;
}

static SiftPageType
relocation_page_type(const SiftSegEntry *entry, BlockNumber blk, bool data)
{
	if (blk == entry->header_blk)
		return entry->kind == SIFT_SEG_IMMUTABLE ? SIFT_PT_SEGHEADER : SIFT_PT_WBUFHEADER;
	if (entry->kind != SIFT_SEG_IMMUTABLE)
		return SIFT_PT_WBUFDATA;
	return data ? SIFT_PT_DATA : SIFT_PT_LIVE;
}

/* Give a copied page its new generation and point the block numbers it holds at the fresh runs. */
static void
relocation_remap(Page page, SiftPageType type, uint64 gen, const RelocPlan *plan)
{
	SiftPageGetOpaque(page)->gen = gen;
	if (type == SIFT_PT_SEGHEADER)
	{
		SiftSegHeaderData *h = (SiftSegHeaderData *) SiftPageGetData(page);

		h->gen = gen;
		h->nextents = plan->parts - plan->data_at;
		memcpy(h->extents, plan->fresh + plan->data_at, h->nextents * sizeof(SiftExtent));
		SiftPageSetUsed(page, offsetof(SiftSegHeaderData, extents) + h->nextents * sizeof(SiftExtent));
		if (h->live_npages > 0)
			h->live_first = sift_store_relocated_block(h->live_first, plan->source, plan->fresh, plan->parts);
	}
	else if (type == SIFT_PT_WBUFHEADER)
	{
		SiftWbufHeaderData *h = SiftLaneHdr(page);

		h->gen = gen;
		for (int k = 0; k < SIFT_LANES; k++)
		{
			h->lanes[k].head = sift_store_relocated_block(h->lanes[k].head, plan->source, plan->fresh, plan->parts);
			h->lanes[k].tail = sift_store_relocated_block(h->lanes[k].tail, plan->source, plan->fresh, plan->parts);
		}
	}
	else if (type == SIFT_PT_WBUFDATA)
		SiftLaneHead(page)->next = sift_store_relocated_block(SiftLaneHead(page)->next, plan->source, plan->fresh, plan->parts);
}

/*
 * Copy stored pages without decoding, format conversion, promotion or growing stream reservations. Preflight every allocation
 * with the same lowest-fit rule as alloc_blocks, then verify the resulting stream directory fits its existing header page.
 * No accepted move, including its free-list publication, needs to extend the file. The caller atomically publishes the copy.
 */
uint64
sift_store_relocate(Relation rel, SiftWriteTarget *wt, const SiftSegEntry *entry, const SiftExtent *old, int n,
					BlockNumber limit, uint64 budget, SiftSegEntry *entry_new, SiftRelocatePage relocate_page)
{
	RelocPlan plan;
	uint64 blocks = 0;
	uint64 gen;
	PGIOAlignedBlock copy;

	Assert(space_exclusive);
	for (int i = 0; i < n; i++)
		blocks += old[i].nblocks;
	if (blocks > budget / BLCKSZ || !relocation_plan(rel, entry, old, n, blocks, limit, &plan))
		return 0;
	gen = sift_store_new_gen(rel);
	space_limit = limit;
	for (int i = 0; i < plan.parts; i++)
		SIFT_INVARIANT(sift_wt_reserve(wt, plan.source[i].nblocks) == plan.fresh[i].start,
					   "sift: relocation allocation differs from its plan");
	space_limit = InvalidBlockNumber;
	for (int i = 0; i < plan.parts; i++)
		for (uint32 j = 0; j < plan.source[i].nblocks; j++)
		{
			BlockNumber blk = plan.source[i].start + j;
			Buffer buf = read_locked(rel, blk, BUFFER_LOCK_SHARE);
			Page page = copy.data;
			SiftPageType type = relocation_page_type(entry, blk, i >= plan.data_at);

			sift_store_check_page(rel, buf, type, entry->gen);
			memcpy(page, BufferGetPage(buf), BLCKSZ);
			UnlockReleaseBuffer(buf);
			relocation_remap(page, type, gen, &plan);
			if (relocate_page && (entry->flags & SIFT_TBL_SEG_WBUF))
				relocate_page(page, plan.source, plan.fresh, plan.parts);
			sift_wt_put_page(wt, plan.fresh[i].start + j, page);
			CHECK_FOR_INTERRUPTS();
		}
	sift_wt_finish(wt);
	*entry_new = *entry;
	entry_new->gen = gen;
	entry_new->header_blk = sift_store_relocated_block(entry->header_blk, plan.source, plan.fresh, plan.parts);
	pfree(plan.source);
	pfree(plan.fresh);
	space_written += blocks;
	return blocks * BLCKSZ;
}
