/*
 * core/visibility.c - visibility map masks and heap checks (DESIGN.md (e)).
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/visibilitymapdefs.h"
#include "access/transam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "bm25/kernels.h"
#include "table/table.h"
#include "core/popcount.h"
#include "core/cpu.h"
#include "core/store.h"
#include "core/visibility.h"

#ifdef SIFT_TESTING
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "storage/lock.h"

int			sift_test_vis_pause = 0;
int			sift_test_vis_jitter_us = 0;
bool		sift_test_count_gate = false;
bool		sift_test_cleanup_gate = false;
static int	test_masks = 0;

/* Waits until the test lets go of advisory lock (0x51F7, key) */
void
sift_test_wait(int key)
{
	LOCKTAG		tag;

	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, 0x51F7, key, 2);
	(void) LockAcquire(&tag, ShareLock, false, false);
	LockRelease(&tag, ShareLock, false);
}

/* Widens the window between the liveness a scan has read and the map it reads: a test holds the lock, runs the VACUUM, and lets go */
static void
test_pause(void)
{
	if (sift_test_vis_pause > 0 && ++test_masks == sift_test_vis_pause)
		sift_test_wait(1);
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

/* Blocks of the table in TID space: a sift_table hands out virtual blocks, not pages of its relation */
BlockNumber
sift_vis_nblocks(Relation heap)
{
	return sift_tbl_is_table(heap) ? (BlockNumber) sift_tbl_nvblocks(heap) : RelationGetNumberOfBlocks(heap);
}

/* The all-visible bit of each page (not its all-frozen bit) among the n words of a map page; the contents of a page are 8-aligned. No popcnt instruction: this object is baseline x86-64 (scripts/isa_check.sh) */
static uint32
count_allvisible_words(const uint64 *w, uint32 n)
{
	uint32		count = 0;

	for (uint32 i = 0; i < n; i++)
		count += sift_pc64(w[i] & UINT64CONST(0x5555555555555555));
	return count;
}

/*
 * The pages among the first nblocks that the map says are all-visible: what visibilitymap_count counts, from the bytes of the map those pages
 * use (it counts the whole 8 kB of every map page, a hundred times the work for a partition of a few pages). Not locked, as there: a bit
 * a writer sets or clears meanwhile is a page more or less.
 */
BlockNumber
sift_vis_count_allvisible(Relation heap, BlockNumber nblocks)
{
	SMgrRelation smgr = RelationGetSmgr(heap);
	uint64		left = ((uint64) nblocks + 3) / 4;
	BlockNumber vm_nblocks,
				count = 0;

	if (!smgrexists(smgr, VISIBILITYMAP_FORKNUM))
		return 0;
	vm_nblocks = smgrnblocks(smgr, VISIBILITYMAP_FORKNUM);
	for (BlockNumber mapblk = 0; left > 0 && mapblk < vm_nblocks; mapblk++)
	{
		Buffer		buf = ReadBufferExtended(heap, VISIBILITYMAP_FORKNUM, mapblk, RBM_ZERO_ON_ERROR, NULL);
		const char *map = PageGetContents(BufferGetPage(buf));
		uint32		len = Min(left, VM_MAPSIZE);
		uint32		off = len / sizeof(uint64) * sizeof(uint64);

		count += count_allvisible_words((const uint64 *) map, len / sizeof(uint64));
		if (off < len)
		{
			uint64		w = 0;

			memcpy(&w, map + off, len - off);
			count += sift_pc64(w & UINT64CONST(0x5555555555555555));
		}
		ReleaseBuffer(buf);
		left -= len;
	}
	return count;
}

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
	v->ndirty = -1;
	v->mcxt = CurrentMemoryContext;
	v->fetch = sift_table_index_fetch_begin(heap);
	v->heap_nblocks = sift_vis_nblocks(heap);
	v->vm_nblocks = smgrexists(RelationGetSmgr(heap), VISIBILITYMAP_FORKNUM) ?
		smgrnblocks(RelationGetSmgr(heap), VISIBILITYMAP_FORKNUM) : 0;
}

void
sift_vis_unpin(SiftVis *v)
{
	if (BufferIsValid(v->vmbuf))
		ReleaseBuffer(v->vmbuf);
	v->vmbuf = InvalidBuffer;
	if (BufferIsValid(v->dirbuf))
		ReleaseBuffer(v->dirbuf);
	v->dirbuf = InvalidBuffer;
	if (v->fetch)
		table_index_fetch_reset(v->fetch);
}

void
sift_vis_end(SiftVis *v)
{
	sift_vis_unpin(v);
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
	if (v->index != NULL && !v->dir_moved)
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

/* The pages of the table (first heap_nblocks) that are not all-visible, once per scan, from the map bytes */
static void
dirty_scan(SiftVis *v)
{
	uint64		nb = v->heap_nblocks,
				bytes = (nb + 3) / 4;

	v->ndirty = 0;
	v->dirty = MemoryContextAlloc(v->mcxt, SIFT_VIS_DIRTY_MAX * sizeof(BlockNumber));
	if (!v->use_vm || nb == 0)
	{
		v->ndirty = -2;
		return;
	}
	for (uint64 mapblk = 0; mapblk * VM_MAPSIZE < bytes; mapblk++)
	{
		const uint8 *map = NULL;
		Buffer		buf = InvalidBuffer;
		uint64		len = Min(bytes - mapblk * VM_MAPSIZE, VM_MAPSIZE);

		if (mapblk < v->vm_nblocks)
		{
			buf = ReadBufferExtended(v->heap, VISIBILITYMAP_FORKNUM, (BlockNumber) mapblk, RBM_ZERO_ON_ERROR, NULL);
			map = (const uint8 *) PageGetContents(BufferGetPage(buf));
		}
		for (uint64 i = 0; i < len; i++)
		{
			uint8		b = map ? map[i] : 0;

			if ((b & 0x55) == 0x55)
				continue;
			for (int k = 0; k < 4; k++)
			{
				uint64		x = (mapblk * VM_MAPSIZE + i) * 4 + k;

				if (x >= nb || ((b >> (2 * k)) & 1))
					continue;
				if (v->ndirty == SIFT_VIS_DIRTY_MAX)
				{
					if (BufferIsValid(buf))
						ReleaseBuffer(buf);
					v->ndirty = -2;
					return;
				}
				v->dirty[v->ndirty++] = (BlockNumber) x;
			}
		}
		if (BufferIsValid(buf))
			ReleaseBuffer(buf);
	}
}

bool
sift_vis_range_allvisible(SiftVis *v, BlockNumber lo, BlockNumber hi)
{
	uint32		a = 0,
				b;

	if (v->ndirty == -1)
		dirty_scan(v);
	if (v->ndirty < 0)
		return false;
	b = v->ndirty;
	while (a < b)				/* the first dirty page >= lo */
	{
		uint32		mid = (a + b) / 2;

		if (v->dirty[mid] < lo)
			a = mid + 1;
		else
			b = mid;
	}
	return (a == (uint32) v->ndirty || v->dirty[a] > hi) && dir_current(v);
}

/* The all-visible bit of each of the n blocks (ascending, as a key-cut unit lists its pages) in bits, one bit per block; the same directory rule as sift_vis_sb_mask */
void
sift_vis_blocks_allvisible(SiftVis *v, const BlockNumber *blocks, uint32 n, uint64 *bits)
{
	uint64		any = 0;
	BlockNumber curmap = InvalidBlockNumber;
	const uint8 *map = NULL;

	memset(bits, 0, ((n + 63) / 64) * sizeof(uint64));
	if (!v->use_vm)
		return;
	TEST_PAUSE();
	for (uint32 i = 0; i < n; i++)
	{
		uint64		byteno = blocks[i] / 4;
		BlockNumber mapblk = byteno / VM_MAPSIZE;

		if (mapblk != curmap)
		{
			curmap = mapblk;
			map = NULL;
			if (mapblk < v->vm_nblocks)
			{
				if (!BufferIsValid(v->vmbuf) || BufferGetBlockNumber(v->vmbuf) != mapblk)
				{
					if (BufferIsValid(v->vmbuf))
						ReleaseBuffer(v->vmbuf);
					v->vmbuf = ReadBufferExtended(v->heap, VISIBILITYMAP_FORKNUM, mapblk, RBM_ZERO_ON_ERROR, NULL);
				}
				map = (const uint8 *) PageGetContents(BufferGetPage(v->vmbuf));
			}
		}
		if (map != NULL && (map[byteno % VM_MAPSIZE] >> ((blocks[i] % 4) * 2)) & 1)
			bits[i / 64] |= UINT64CONST(1) << (i % 64);
	}
	for (uint32 w = 0; w < (n + 63) / 64; w++)
		any |= bits[w];
	if (any && !dir_current(v))
		memset(bits, 0, ((n + 63) / 64) * sizeof(uint64));
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
	Page		page;
	OffsetNumber maxoff;
	bool		quick;			/* a tuple whose inserter is older than every transaction of the snapshot needs no visibility function */
} HeapPage;

/*
 * The tuple an index entry names is plain (no HOT chain to follow) and visible to the snapshot when its inserter committed (the hint
 * bit) before the oldest transaction the snapshot knows, or froze it, and nobody deleted or locked it (xmax invalid): the first two
 * tests of HeapTupleSatisfiesMVCC, without the chain walk, the predicate lock and the conflict check heap_hot_search_buffer adds. Every
 * other tuple goes the full way. A serializable transaction takes the full way too: the lock and the check are its.
 */
static bool
quick_visible(const HeapPage *p, ItemPointer tid)
{
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);
	ItemId		lp;
	HeapTupleHeader th;

	if (!p->quick || off < FirstOffsetNumber || off > p->maxoff)
		return false;
	lp = PageGetItemId(p->page, off);
	if (!ItemIdIsNormal(lp))
		return false;
	th = (HeapTupleHeader) PageGetItem(p->page, lp);
	if (!(th->t_infomask & HEAP_XMAX_INVALID) || (th->t_infomask & HEAP_XMIN_COMMITTED) == 0)
		return false;
	return HeapTupleHeaderXminFrozen(th) || TransactionIdPrecedes(HeapTupleHeaderGetRawXmin(th), p->v->snapshot->xmin);
}

static bool
heap_tid_visible(void *arg, ItemPointer tid)
{
	HeapPage   *p = arg;
	HeapTupleData tuple;
	bool		all_dead;

	return quick_visible(p, tid) || heap_hot_search_buffer(tid, p->v->heap, p->buf, p->v->snapshot, &tuple, &all_dead, true);
}

/*
 * A page the scan asks about again (units cut by a key, not by the heap, take a few rows of each page): the second visit computes the
 * visibility of every row of the page once and the later ones are a mask. A snapshot does not change its mind about a tuple, so the mask
 * holds for the rest of the scan. Entries are capped; a page past the cap is read every time, as before.
 */
#define SIFT_VIS_PAGE_CACHE_MAX	(1 << 20)

typedef struct PageVis
{
	BlockNumber blk;
	bool		full;			/* vis holds every row of the page; else the page was visited once */
	uint64		vis[SIFT_OFFMASK_WORDS];
} PageVis;

static PageVis *
page_cache_entry(SiftVis *v, BlockNumber blk, bool *found)
{
	if (v->pagevis == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(BlockNumber), .entrysize = sizeof(PageVis), .hcxt = v->mcxt};

		v->pagevis = hash_create("sift page visibility", 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	if (hash_get_num_entries(v->pagevis) >= SIFT_VIS_PAGE_CACHE_MAX)
		return hash_search(v->pagevis, &blk, HASH_FIND, found);
	return hash_search(v->pagevis, &blk, HASH_ENTER, found);
}

int
sift_vis_filter_page(SiftVis *v, BlockNumber blk, uint64 *offmask)
{
	HeapPage	page = {.v = v};
	PageVis    *e = NULL;
	bool		found = false,
				full = false;
	int			left = 0;

	if (blk >= v->heap_nblocks)
		v->heap_nblocks = sift_vis_nblocks(v->heap);
	if (blk >= v->heap_nblocks)
	{
		memset(offmask, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
		return 0;
	}
	if (sift_tbl_is_table(v->heap))
		return sift_tbl_filter_page(v->heap, v->snapshot, blk, offmask);
	if (sift_vis_page_cache)
	{
		e = page_cache_entry(v, blk, &found);
		if (e != NULL && !found)
			e->full = false;
		else if (e != NULL && e->full)
		{
			for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
			{
				offmask[wd] &= e->vis[wd];
				left += sift_pc64(offmask[wd]);
			}
			return left;
		}
		full = e != NULL && found;
	}
	page.buf = ReadBuffer(v->heap, blk);
	LockBuffer(page.buf, BUFFER_LOCK_SHARE);
	page.page = BufferGetPage(page.buf);
	page.maxoff = PageGetMaxOffsetNumber(page.page);
	page.quick = IsMVCCSnapshot(v->snapshot) && !IsolationIsSerializable();
	v->heap_pages++;
	if (full)
	{
		memset(e->vis, 0, sizeof(e->vis));
		for (OffsetNumber off = FirstOffsetNumber; off <= page.maxoff && off <= SIFT_OFFMASK_WORDS * 64; off++)
		{
			ItemPointerData tid;

			ItemPointerSet(&tid, blk, off);
			if (heap_tid_visible(&page, &tid))
				e->vis[(off - 1) / 64] |= UINT64CONST(1) << ((off - 1) % 64);
		}
		e->full = true;
		for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
		{
			offmask[wd] &= e->vis[wd];
			left += sift_pc64(offmask[wd]);
		}
	}
	else
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
