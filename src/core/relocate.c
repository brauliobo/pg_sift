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

/* the shortest free run a piece of a data run is moved into (fewer, longer extents in the header) */
#define SPACE_MIN_RUN_BLOCKS	16

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

/* A run of a segment the shift moves, and the free run it goes to */
typedef struct ShiftPart
{
	SiftExtent	src;
	BlockNumber dst;
	int			ext;			/* the extent of the header it is the end of, or -1 for the liveness run */
} ShiftPart;

typedef struct ShiftPiece
{
	SiftExtent	run;
	int			ext;
} ShiftPiece;

static int
cmp_piece_end_desc(const void *a, const void *b)
{
	const ShiftPiece *x = a;
	const ShiftPiece *y = b;

	return pg_cmp_u64((uint64) y->run.start + y->run.nblocks, (uint64) x->run.start + x->run.nblocks);
}

static int
cmp_part_src(const void *a, const void *b)
{
	return pg_cmp_u32(((const ShiftPart *) a)->src.start, ((const ShiftPart *) b)->src.start);
}

/*
 * Moves the runs of an immutable segment that lie highest in the file (up to budget) into the lowest free runs below each of them,
 * and writes the header page again, at the lowest free block, with the extents they now are: a segment far larger than the budget
 * walks down the file a round at a time, and the file can be cut where it ended. The pages are copied as they are (the segment keeps its
 * generation), so a data run moves in pieces and the header lists more extents; the liveness run moves whole or not at all. Like
 * sift_store_relocate, nothing extends the file, and the caller publishes the new entry and the released runs together (sift_wt_publish).
 * Returns the bytes copied, zero when no free run lies below a run of the segment, with nothing reserved.
 */
uint64
sift_store_shift(Relation rel, SiftWriteTarget *wt, const SiftSegEntry *entry, uint64 budget, SiftSegEntry *entry_new,
				 SiftExtent **release, int *nrelease)
{
	PGIOAlignedBlock hdrcopy;
	PGIOAlignedBlock copy;
	SiftSegHeaderData *h = (SiftSegHeaderData *) SiftPageGetData(hdrcopy.data);
	Buffer		buf = read_locked(rel, entry->header_blk, BUFFER_LOCK_SHARE);
	Buffer		metabuf;
	FreeChain	free;
	ShiftPiece *piece;
	ShiftPart  *part;
	SiftExtent *list;
	SiftExtent *rel_ext;
	uint64		moved = 0;
	uint64		maxblocks = budget / BLCKSZ > 0 ? budget / BLCKSZ - 1 : 0;	/* the header page is copied too */
	int			npiece = 0;
	int			nparts = 0;
	int			ndata = 0;
	int			nlist = 0;
	BlockNumber newhdr = InvalidBlockNumber;
	int			hdrbest;
	uint64		gen = entry->gen;

	Assert(space_exclusive);
	sift_store_check_page(rel, buf, SIFT_PT_SEGHEADER, gen);
	memcpy(hdrcopy.data, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	if (h->nextents > SIFT_MAX_EXTENTS)
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED), errmsg("index \"%s\": segment header block %u lists %u extents", RelationGetRelationName(rel), entry->header_blk, h->nextents)));
	piece = palloc((h->nextents + 1) * sizeof(ShiftPiece));
	part = palloc((SIFT_MAX_EXTENTS + 2) * sizeof(ShiftPart));
	for (int i = 0; i < (int) h->nextents; i++)
		piece[npiece++] = (ShiftPiece) {h->extents[i], i};
	if (h->live_npages > 0)
		piece[npiece++] = (ShiftPiece) {{h->live_first, h->live_npages}, -1};
	qsort(piece, npiece, sizeof(ShiftPiece), cmp_piece_end_desc);

	metabuf = lock_meta(rel, BUFFER_LOCK_SHARE);
	read_chain(rel, metabuf, &free, InvalidFullTransactionId);
	UnlockReleaseBuffer(metabuf);
	for (int p = 0; p < npiece && moved < maxblocks; p++)
	{
		const SiftExtent *run = &piece[p].run;
		BlockNumber top = run->start + run->nblocks;
		uint32		left = run->nblocks;

		if (piece[p].ext < 0)
		{
			int			best = left <= maxblocks - moved ? relocation_pick(&free, left, run->start) : -1;

			if (best < 0)
				continue;
			part[nparts++] = (ShiftPart) {{run->start, left}, free.ext[best].start, -1};
			free.ext[best].start += left;
			free.ext[best].nblocks -= left;
			moved += left;
			continue;
		}
		while (left > 0 && moved < maxblocks && h->nextents + ndata < SIFT_MAX_EXTENTS)
		{
			uint32		want = Min(left, maxblocks - moved);
			int			best = relocation_pick(&free, Min(want, SPACE_MIN_RUN_BLOCKS), run->start);
			uint32		take;

			if (best < 0)
				break;
			take = Min(want, Min(free.ext[best].nblocks, run->start - free.ext[best].start));
			top -= take;
			part[nparts++] = (ShiftPart) {{top, take}, free.ext[best].start, piece[p].ext};
			free.ext[best].start += take;
			free.ext[best].nblocks -= take;
			left -= take;
			moved += take;
			ndata++;
		}
	}
	hdrbest = nparts > 0 ? relocation_pick(&free, 1, RelationGetNumberOfBlocks(rel)) : -1;
	if (hdrbest < 0 || !relocation_leaves_spare(&free, nparts + 1))
	{
		pfree(piece);
		pfree(part);
		pfree(free.ext);
		pfree(free.page);
		return 0;
	}
	newhdr = free.ext[hdrbest].start;
	pfree(free.ext);
	pfree(free.page);
	pfree(piece);

	/* the same lowest-fit picks, now taken from the free list: the order is the order of the plan */
	for (int i = 0; i < nparts; i++)
	{
		space_limit = part[i].ext < 0 ? part[i].src.start :
			h->extents[part[i].ext].start;
		SIFT_INVARIANT(sift_wt_reserve(wt, part[i].src.nblocks) == part[i].dst, "sift: shift allocation differs from its plan");
	}
	space_limit = RelationGetNumberOfBlocks(rel);
	SIFT_INVARIANT(sift_wt_reserve(wt, 1) == newhdr, "sift: shift allocation differs from its plan");
	space_limit = InvalidBlockNumber;
	for (int i = 0; i < nparts; i++)
		for (uint32 j = 0; j < part[i].src.nblocks; j++)
		{
			Buffer		src = read_locked(rel, part[i].src.start + j, BUFFER_LOCK_SHARE);

			sift_store_check_page(rel, src, part[i].ext < 0 ? SIFT_PT_LIVE : SIFT_PT_DATA, gen);
			memcpy(copy.data, BufferGetPage(src), BLCKSZ);
			UnlockReleaseBuffer(src);
			sift_wt_put_page(wt, part[i].dst + j, copy.data);
			CHECK_FOR_INTERRUPTS();
		}

	/* the extents the header lists now, in the order of the stream: what stayed of each, then the pieces that left it, lowest first */
	qsort(part, nparts, sizeof(ShiftPart), cmp_part_src);
	list = palloc((h->nextents + ndata) * sizeof(SiftExtent));
	for (int e = 0; e < (int) h->nextents; e++)
	{
		SiftExtent	old = h->extents[e];
		uint32		stays = old.nblocks;

		for (int i = 0; i < nparts; i++)
			if (part[i].ext == e)
				stays -= part[i].src.nblocks;
		if (stays > 0)
			list[nlist++] = (SiftExtent) {old.start, stays};
		for (int i = 0; i < nparts; i++)
			if (part[i].ext == e)
			{
				if (nlist > 0 && list[nlist - 1].start + list[nlist - 1].nblocks == part[i].dst)
					list[nlist - 1].nblocks += part[i].src.nblocks;
				else
					list[nlist++] = (SiftExtent) {part[i].dst, part[i].src.nblocks};
			}
	}
	for (int i = 0; i < nparts; i++)
		if (part[i].ext < 0)
			h->live_first = part[i].dst;
	h->nextents = nlist;
	memcpy(h->extents, list, nlist * sizeof(SiftExtent));
	SiftPageSetUsed(hdrcopy.data, offsetof(SiftSegHeaderData, extents) + nlist * sizeof(SiftExtent));
	sift_wt_put_page(wt, newhdr, hdrcopy.data);
	sift_wt_finish(wt);

	rel_ext = palloc((nparts + 1) * sizeof(SiftExtent));
	for (int i = 0; i < nparts; i++)
		rel_ext[i] = part[i].src;
	rel_ext[nparts] = (SiftExtent) {entry->header_blk, 1};
	*release = rel_ext;
	*nrelease = nparts + 1;
	*entry_new = *entry;
	entry_new->header_blk = newhdr;
	pfree(list);
	pfree(part);
	space_written += moved + 1;
	return (moved + 1) * BLCKSZ;
}
