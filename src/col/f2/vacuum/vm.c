/*
 * col/f2/vacuum/vm.c - the dirty set of a heap's visibility map (col/f2/vacuum/vm.h).
 */
#include "postgres.h"

#include "col/f2/vacuum/vm.h"

void
sift_vm_dirty_init(SiftVmDirtySet *ds, BlockNumber nheap_blocks)
{
	*ds = (SiftVmDirtySet) {.nheap_blocks = nheap_blocks};
}

void
sift_vm_dirty_all(SiftVmDirtySet *ds)
{
	*ds = (SiftVmDirtySet) {.nheap_blocks = 0,.all_dirty = true};
}

/* The set that was built is let go of for the one that skips nothing */
static void
give_up(SiftVmDirtySet *ds)
{
	if (ds->intervals != NULL)
		pfree(ds->intervals);
	sift_vm_dirty_all(ds);
}

static void
add_run(SiftVmDirtySet *ds, BlockNumber lo, BlockNumber hi)
{
	if (ds->all_dirty)
		return;
	if (ds->nintervals > 0 && ds->intervals[ds->nintervals - 1].hi + 1 == lo)
		ds->intervals[ds->nintervals - 1].hi = hi;
	else
	{
		if (ds->nintervals == SIFT_VM_DIRTY_MAX_INTERVALS)
		{
			give_up(ds);
			return;
		}
		if (ds->nintervals == ds->cap)
		{
			ds->cap = ds->cap > 0 ? ds->cap * 2 : 64;
			ds->intervals = ds->intervals ? repalloc(ds->intervals, ds->cap * sizeof(SiftBlkInterval)) : palloc(ds->cap * sizeof(SiftBlkInterval));
		}
		ds->intervals[ds->nintervals++] = (SiftBlkInterval) {lo, hi};
	}
	ds->dirty_blocks += (uint64) hi - lo + 1;
}

void
sift_vm_dirty_scan_words(SiftVmDirtySet *ds, BlockNumber base, const uint64 *words, int nwords)
{
	for (int w = 0; w < nwords; w++)
	{
		uint64		clear = ~words[w] & SIFT_VM_ALL_VISIBLE_BITS;
		const uint64 b0 = (uint64) base + (uint64) w * SIFT_VM_BLOCKS_PER_WORD;

		for (; clear != 0; clear &= clear - 1)
		{
			const uint64 b = b0 + (uint64) (__builtin_ctzll(clear) / 2);

			if (b >= ds->nheap_blocks)
				return;
			add_run(ds, (BlockNumber) b, (BlockNumber) b);
		}
	}
}

void
sift_vm_dirty_finish(SiftVmDirtySet *ds)
{
	if (ds->dirty_blocks * 2 > ds->nheap_blocks)
		give_up(ds);
}

void
sift_vm_dirty_free(SiftVmDirtySet *ds)
{
	if (ds->intervals != NULL)
		pfree(ds->intervals);
	*ds = (SiftVmDirtySet) {0};
}

/* The first interval whose last block is at or past lo */
static int
first_reaching(const SiftVmDirtySet *ds, BlockNumber lo)
{
	int			a = 0,
				b = ds->nintervals;

	while (a < b)
	{
		int			mid = a + (b - a) / 2;

		if (ds->intervals[mid].hi < lo)
			a = mid + 1;
		else
			b = mid;
	}
	return a;
}

bool
sift_vm_dirty_overlaps(const SiftVmDirtySet *ds, BlockNumber lo, BlockNumber hi)
{
	int			i;

	if (ds->all_dirty || hi >= ds->nheap_blocks)
		return true;
	i = first_reaching(ds, lo);
	return i < ds->nintervals && ds->intervals[i].lo <= hi;
}

bool
sift_vm_dirty_overlaps_bloom(const SiftVmDirtySet *ds, BlockNumber lo, BlockNumber hi, uint64 bloom)
{
	int			tested = 0;

	if (ds->all_dirty || hi >= ds->nheap_blocks)
		return true;
	for (int i = first_reaching(ds, lo); i < ds->nintervals && ds->intervals[i].lo <= hi; i++)
	{
		BlockNumber from = Max(lo, ds->intervals[i].lo),
					to = Min(hi, ds->intervals[i].hi);

		if ((uint64) to - from + 1 + tested > SIFT_VM_DIRTY_MAX_BLOOM_BLOCKS)
			return true;
		for (BlockNumber b = from;; b++)
		{
			if ((bloom & sift_vm_blk_bit(b)) != 0)
				return true;
			tested++;
			if (b == to)
				break;
		}
	}
	return false;
}

#ifndef SIFT_S49_STANDALONE
#include "access/heapam.h"
#include "access/visibilitymap.h"
#include "catalog/pg_am_d.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"

/* 2 bits for each block in the map page: its payload is what the page has after its header */
#define VM_MAPSIZE				(BLCKSZ - MAXALIGN(SizeOfPageHeaderData))
#define VM_BLOCKS_PER_PAGE		((BlockNumber) VM_MAPSIZE * 4)

void
sift_vm_dirty_build(SiftVmDirtySet *ds, Relation heap, bool validating)
{
	BlockNumber nblocks;
	BlockNumber nvm;
	uint64		words[BLCKSZ / sizeof(uint64)];

	if (heap == NULL || heap->rd_rel->relam != HEAP_TABLE_AM_OID || validating)
	{
		sift_vm_dirty_all(ds);
		return;
	}
	nblocks = RelationGetNumberOfBlocks(heap);
	sift_vm_dirty_init(ds, nblocks);
	nvm = smgrexists(RelationGetSmgr(heap), VISIBILITYMAP_FORKNUM) ? smgrnblocks(RelationGetSmgr(heap), VISIBILITYMAP_FORKNUM) : 0;
	for (BlockNumber m = 0; (uint64) m * VM_BLOCKS_PER_PAGE < nblocks; m++)
	{
		BlockNumber base = m * VM_BLOCKS_PER_PAGE;
		int			nwords = VM_MAPSIZE / sizeof(uint64);

		if (m >= nvm)
		{
			add_run(ds, base, nblocks - 1);
			break;
		}
		{
			Buffer		buf = ReadBufferExtended(heap, VISIBILITYMAP_FORKNUM, m, RBM_ZERO_ON_ERROR, NULL);

			LockBuffer(buf, BUFFER_LOCK_SHARE);
			memset(words, 0, sizeof(words));
			memcpy(words, PageGetContents(BufferGetPage(buf)), VM_MAPSIZE);
			UnlockReleaseBuffer(buf);
		}
		sift_vm_dirty_scan_words(ds, base, words, nwords);
		if (ds->all_dirty)
			return;
	}
	sift_vm_dirty_finish(ds);
}
#endif
