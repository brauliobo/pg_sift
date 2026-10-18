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
#include "sift_store.h"
#include "sift_visibility.h"

#ifdef SIFT_TESTING
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "storage/lock.h"

int			sift_test_vis_pause = 0;
int			sift_test_vis_jitter_us = 0;
static int	test_masks = 0;

/* Widens the window between the liveness a scan has read and the map it reads: a test holds the lock, runs the VACUUM, and lets go */
static void
test_pause(void)
{
	if (sift_test_vis_pause > 0 && ++test_masks == sift_test_vis_pause)
	{
		LOCKTAG		tag;

		SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, 0x51F7, 1, 2);
		(void) LockAcquire(&tag, ShareLock, false, false);
		LockRelease(&tag, ShareLock, false);
	}
	if (sift_test_vis_jitter_us > 0)
		pg_usleep(pg_prng_uint64_range(&pg_global_prng_state, 0, sift_test_vis_jitter_us));
}
#define TEST_PAUSE()	test_pause()
#else
#define TEST_PAUSE()	((void) 0)
#endif

/* visibilitymap.c layout: 2 bits per heap page after the page header */
#define VM_MAPSIZE		(BLCKSZ - MAXALIGN(SizeOfPageHeaderData))
#define VM_BYTES_PER_SB	(SIFT_SB_PAGES / 4)

void
sift_vis_begin(SiftVis *v, Relation heap, Snapshot snapshot, Relation index, uint32 dir_version)
{
	memset(v, 0, sizeof(SiftVis));
#ifdef SIFT_TESTING
	test_masks = 0;
#endif
	v->heap = heap;
	v->snapshot = snapshot;
	v->index = index;
	v->dir_version = dir_version;
	v->use_vm = IsMVCCSnapshot(snapshot);
	v->vmbuf = InvalidBuffer;
	v->fetch = sift_table_index_fetch_begin(heap);
	v->heap_nblocks = RelationGetNumberOfBlocks(heap);
	v->vm_nblocks = smgrexists(RelationGetSmgr(heap), VISIBILITYMAP_FORKNUM) ?
		smgrnblocks(RelationGetSmgr(heap), VISIBILITYMAP_FORKNUM) : 0;
}

void
sift_vis_end(SiftVis *v)
{
	if (BufferIsValid(v->vmbuf))
		ReleaseBuffer(v->vmbuf);
	v->vmbuf = InvalidBuffer;
	if (BufferIsValid(v->dirbuf))
		ReleaseBuffer(v->dirbuf);
	v->dirbuf = InvalidBuffer;
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

static bool
any_page(const uint64 *pagemask)
{
	uint64		any = 0;

	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		any |= pagemask[wd];
	return any != 0;
}

/*
 * Whether the directory is the one the scan read before any liveness. The version is read after the map (the buffer lock orders the two),
 * and once it has moved it stays moved. A VACUUM clears liveness bits and bumps the version before it marks a page all-visible, so a scan
 * that finds the version unchanged after it has read an all-visible bit has read, after the directory, liveness that shows every removal
 * behind the bit; a merge or a promotion moves the version too, and the scan's older segments may then miss the removals of the segments
 * that replaced them.
 */
static bool
dir_current(SiftVis *v)
{
	if (!v->dir_moved)
	{
		if (!BufferIsValid(v->dirbuf))
			v->dirbuf = sift_dir_pin(v->index);
		v->dir_moved = sift_dir_version_peek(v->dirbuf) != v->dir_version;
	}
	return !v->dir_moved;
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
	TEST_PAUSE();
	vm_read(v, first / VM_MAPSIZE, off, head, bytes);
	if (head < VM_BYTES_PER_SB)
		vm_read(v, first / VM_MAPSIZE + 1, 0, VM_BYTES_PER_SB - head, bytes + head);
	sift_kern->vm_allvisible(pagemask, bytes);
	if (any_page(pagemask) && !dir_current(v))
		memset(pagemask, 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
}

int
sift_vis_filter_page(SiftVis *v, BlockNumber blk, uint64 *offmask)
{
	Buffer		buf;
	int			left = 0;

	if (blk >= v->heap_nblocks)
		v->heap_nblocks = RelationGetNumberOfBlocks(v->heap);
	if (blk >= v->heap_nblocks)
	{
		memset(offmask, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
		return 0;
	}
	buf = ReadBuffer(v->heap, blk);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	v->heap_pages++;
	for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
		for (uint64 m = offmask[wd]; m; m &= m - 1)
		{
			ItemPointerData tid;
			HeapTupleData tuple;
			bool		all_dead;

			ItemPointerSet(&tid, blk, wd * 64 + __builtin_ctzll(m) + 1);
			if (heap_hot_search_buffer(&tid, v->heap, buf, v->snapshot, &tuple, &all_dead, true))
				left++;
			else
				offmask[wd] &= ~(m & -m);
		}
	UnlockReleaseBuffer(buf);
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
