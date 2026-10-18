/*
 * sift_visibility.c - visibility map masks and heap checks.
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/tableam.h"
#include "access/visibilitymapdefs.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "sift_kernels.h"
#include "sift_table.h"
#include "sift_visibility.h"

/* visibilitymap.c layout: 2 bits per heap page after the page header */
#define VM_MAPSIZE		(BLCKSZ - MAXALIGN(SizeOfPageHeaderData))
#define VM_BYTES_PER_SB	(SIFT_SB_PAGES / 4)

/* Blocks of the table in TID space: a sift_table hands out virtual blocks, not pages of its relation */
BlockNumber
sift_vis_nblocks(Relation heap)
{
	return sift_tbl_is_table(heap) ? (BlockNumber) sift_tbl_nvblocks(heap) : RelationGetNumberOfBlocks(heap);
}

void
sift_vis_begin(SiftVis *v, Relation heap, Snapshot snapshot)
{
	memset(v, 0, sizeof(SiftVis));
	v->heap = heap;
	v->snapshot = snapshot;
	v->use_vm = IsMVCCSnapshot(snapshot);
	v->vmbuf = InvalidBuffer;
	v->fetch = sift_table_index_fetch_begin(heap);
	v->heap_nblocks = sift_vis_nblocks(heap);
	v->vm_nblocks = smgrexists(RelationGetSmgr(heap), VISIBILITYMAP_FORKNUM) ?
		smgrnblocks(RelationGetSmgr(heap), VISIBILITYMAP_FORKNUM) : 0;
}

void
sift_vis_end(SiftVis *v)
{
	if (BufferIsValid(v->vmbuf))
		ReleaseBuffer(v->vmbuf);
	v->vmbuf = InvalidBuffer;
	table_index_fetch_end(v->fetch);
	v->fetch = NULL;
}

/* Copies len map bytes starting at byte offset off of VM page mapblk; zeros past the fork's end */
static void
vm_read(SiftVis *v, BlockNumber mapblk, uint32 off, uint32 len, uint8 *out)
{
	if (mapblk >= v->vm_nblocks)
	{
		memset(out, 0, len);
		return;
	}
	if (!BufferIsValid(v->vmbuf) || BufferGetBlockNumber(v->vmbuf) != mapblk)
	{
		if (BufferIsValid(v->vmbuf))
			ReleaseBuffer(v->vmbuf);
		v->vmbuf = ReadBufferExtended(v->heap, VISIBILITYMAP_FORKNUM, mapblk, RBM_ZERO_ON_ERROR, NULL);
	}
	memcpy(out, PageGetContents(BufferGetPage(v->vmbuf)) + off, len);
}

void
sift_vis_sb_mask(SiftVis *v, uint32 sb, uint64 *pagemask)
{
	uint8		bytes[VM_BYTES_PER_SB] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	uint64		first = (uint64) sb * VM_BYTES_PER_SB;
	uint32		off = first % VM_MAPSIZE,
				head = Min(VM_BYTES_PER_SB, VM_MAPSIZE - off);

	if (!v->use_vm)
	{
		memset(pagemask, 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
		return;
	}
	vm_read(v, first / VM_MAPSIZE, off, head, bytes);
	if (head < VM_BYTES_PER_SB)
		vm_read(v, first / VM_MAPSIZE + 1, 0, VM_BYTES_PER_SB - head, bytes + head);
	sift_kern->vm_allvisible(pagemask, bytes);
}

int
sift_vis_filter_mask(BlockNumber blk, uint64 *offmask, SiftTidTest test, void *arg)
{
	int			left = 0;

	for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
		for (uint64 m = offmask[wd]; m; m &= m - 1)
		{
			ItemPointerData tid;

			ItemPointerSet(&tid, blk, wd * 64 + __builtin_ctzll(m) + 1);
			if (test(arg, &tid))
				left++;
			else
				offmask[wd] &= ~(m & -m);
		}
	return left;
}

typedef struct HeapPage
{
	SiftVis    *v;
	Buffer		buf;
} HeapPage;

static bool
heap_tid_visible(void *arg, ItemPointer tid)
{
	HeapPage   *p = arg;
	HeapTupleData tuple;
	bool		all_dead;

	return heap_hot_search_buffer(tid, p->v->heap, p->buf, p->v->snapshot, &tuple, &all_dead, true);
}

int
sift_vis_filter_page(SiftVis *v, BlockNumber blk, uint64 *offmask)
{
	HeapPage	page = {.v = v};
	int			left;

	if (blk >= v->heap_nblocks)
		v->heap_nblocks = sift_vis_nblocks(v->heap);
	if (blk >= v->heap_nblocks)
	{
		memset(offmask, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
		return 0;
	}
	if (sift_tbl_is_table(v->heap))
		return sift_tbl_filter_page(v->heap, v->snapshot, blk, offmask);
	page.buf = ReadBuffer(v->heap, blk);
	LockBuffer(page.buf, BUFFER_LOCK_SHARE);
	v->heap_pages++;
	left = sift_vis_filter_mask(blk, offmask, heap_tid_visible, &page);
	UnlockReleaseBuffer(page.buf);
	return left;
}

bool
sift_vis_fetch(SiftVis *v, ItemPointer tid, TupleTableSlot *slot)
{
	bool		call_again = false,
				all_dead;

	v->heap_checks++;
	return table_index_fetch_tuple(v->fetch, tid, v->snapshot, slot, &call_again, &all_dead);
}
