/*
 * core/visibility.c - visibility map masks and heap checks.
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/read_stream.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "bm25/kernels.h"
#include "table/dispatch.h"
#include "table/v1/table_v1.h"
#include "core/popcount.h"
#include "core/cmp.h"
#include "core/cpu.h"
#include "core/extlock.h"
#include "core/store.h"
#include "core/visibility.h"
#include "core/visibility_run.h"

#ifdef SIFT_TESTING
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "storage/lock.h"

int			sift_test_vis_pause = 0;
int			sift_test_vis_jitter_us = 0;
int			sift_test_bulkdelete_gate = 0;
bool		sift_test_count_gate = false;
bool		sift_test_cleanup_gate = false;
bool		sift_test_stay_gate = false;
bool		sift_test_shrink_gate = false;
bool		sift_test_merge_gate = false;
bool		sift_test_worker_gate = false;
bool		sift_test_release_gate = false;
bool		sift_test_export_gate = false;
bool		sift_test_drop_gate = false;
bool		sift_test_build_gate = false;
bool		sift_test_alter_gate = false;
bool		sift_test_step_gate = false;
bool		sift_test_repair_gate = false;
bool		sift_test_check_gate = false;
bool		sift_test_par_gate = false;
int			sift_test_mutant = 0;
bool		sift_test_rebuild_gate = false;
bool		sift_test_intern_gate = false;
bool		sift_test_par_stats = false;
static int	test_masks = 0;

/* Waits until the test lets go of advisory lock (0x51F7, key) */
void
sift_test_wait(int key)
{
	LOCKTAG		tag;

	SIFT_INVARIANT(!sift_extension_held(), "sift: a test gate waits for a heavyweight lock under the relation extension lock");
	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, 0x51F7, key, 2);
	(void) LockAcquire(&tag, ShareLock, false, false);
	LockRelease(&tag, ShareLock, false);
}

void
sift_test_wait_inval(int key)
{
	sift_test_wait(key);
	AcceptInvalidationMessages();
}

/* Waits until the test lets go of the advisory lock of this number (the one-argument int8 form; 0 or less: no gate) */
void
sift_test_hold(int64 key)
{
	if (key > 0)
	{
		DirectFunctionCall1(pg_advisory_lock_int8, Int64GetDatum(key));
		DirectFunctionCall1(pg_advisory_unlock_int8, Int64GetDatum(key));
	}
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
#define VM_BYTE_VISIBLE	((uint8) 0x55)
#define VM_BYTES_PER_SB	(SIFT_SB_PAGES / 4)

/* Blocks of the table in TID space: a sift_table hands out virtual blocks, not pages of its relation */
BlockNumber
sift_vis_nblocks(Relation heap)
{
	return sift_tbl_is_table(heap) ? (BlockNumber) sift_tbl_ops(heap)->nvblocks(heap) : RelationGetNumberOfBlocks(heap);
}

static const uint64 vm_zero_words[VM_MAPSIZE / sizeof(uint64)];

/* What the last counts of a heap were while a planner run lasts (sift_vis_scope): the stamp of an index and the statistics it is read for count the same map */
#define VM_MEMO_MAX 4

static struct
{
	Oid			relid;
	BlockNumber nblocks,
				visible,
				clean;
}			vm_memo[VM_MEMO_MAX];
static int	vm_memo_n;
static bool vm_memo_on;

void
sift_vis_scope(bool on)
{
	vm_memo_on = on;
	vm_memo_n = 0;
}

static void
vm_run_soft(VmPass *p, const uint64 *w, uint64 n)
{
	vm_run_impl(p, w, n, false);
}

static void (*vm_run) (VmPass *p, const uint64 *w, uint64 n) = vm_run_soft;

static void
vm_rebind(const SiftCpu *cpu)
{
#if defined(__x86_64__) || defined(SIFT_HAVE_NEON)
	vm_run = cpu->active > SIFT_SIMD_SCALAR && (cpu->flags & (SIFT_CPU_POPCNT | SIFT_CPU_NEON)) ? sift_vm_run_hw : vm_run_soft;
#else
	vm_run = vm_run_soft;
#endif
}

void
sift_vis_init(void)
{
	sift_cpu_register(vm_rebind);
}

/*
 * The pages among the first nblocks that the map says are all-visible (what visibilitymap_count counts, from the bytes of the map those pages use: it counts the whole
 * 8 kB of every map page, a hundred times the work for a partition of a few pages), and the superblocks among them (SIFT_SB_PAGES pages: a unit's rows are in one) whose
 * pages are all all-visible, the units whose pre-aggregates may answer; the last, partial one counts the pages it has. One pass over the map reads both. Not locked,
 * as in visibilitymap_count: a bit a writer sets or clears meanwhile is a page more or less. clean may be NULL. A superblock is 8 words of the map's bytes, which a map page (1021 words)
 * does not hold a whole number of.
 */
void
sift_vis_count(Relation heap, BlockNumber nblocks, BlockNumber *visible, BlockNumber *clean)
{
	SMgrRelation smgr = RelationGetSmgr(heap);
	uint64		nbytes = ((uint64) nblocks + 3) / 4,
				nw = (nbytes + 7) / 8,
				gw = 0;
	VmPass		p = {VM_VISIBLE_BITS, 0, 0, 0};
	BlockNumber vm_nblocks = 0;

	for (int i = 0; vm_memo_on && i < vm_memo_n; i++)
		if (vm_memo[i].relid == RelationGetRelid(heap) && vm_memo[i].nblocks == nblocks)
		{
			*visible = vm_memo[i].visible;
			if (clean)
				*clean = vm_memo[i].clean;
			return;
		}
	if (smgrexists(smgr, VISIBILITYMAP_FORKNUM))
		vm_nblocks = smgrnblocks(smgr, VISIBILITYMAP_FORKNUM);
	for (BlockNumber mapblk = 0; gw < nw; mapblk++)
	{
		Buffer		buf = InvalidBuffer;
		const uint64 *w = vm_zero_words;
		uint64		n = Min(VM_MAPSIZE / sizeof(uint64), nw - gw);

		if (mapblk < vm_nblocks)
		{
			buf = ReadBufferExtended(heap, VISIBILITYMAP_FORKNUM, mapblk, RBM_ZERO_ON_ERROR, NULL);
			w = (const uint64 *) PageGetContents(BufferGetPage(buf));
		}
		gw += n;
		if (gw == nw)
		{
			/* the last word holds the bytes of the last pages only, and the pages of its last byte that exist: the rest of the word counts as all-visible for a superblock and as zero for the pages */
			uint8		pages[sizeof(uint64)] = {0};
			uint64		tail = 0,
						real = 0;

			memcpy(&tail, (const char *) &w[n - 1], nbytes - (nw - 1) * sizeof(uint64));
			for (uint32 pg = 0; pg < nblocks - (nw - 1) * 32; pg++)
				pages[pg / 4] |= 1 << (pg % 4 * 2);
			memcpy(&real, pages, sizeof(real));
			vm_run(&p, w, n - 1);
			tail &= VM_VISIBLE_BITS;
			p.visible += sift_pc64(tail);
			p.acc &= tail | (VM_VISIBLE_BITS & ~real);
			p.k++;
			if (p.k == 8)
			{
				p.clean += p.acc == VM_VISIBLE_BITS;
				p.k = 0;
			}
		}
		else
			vm_run(&p, w, n);
		if (BufferIsValid(buf))
			ReleaseBuffer(buf);
	}
	if (p.k > 0)
		p.clean += p.acc == VM_VISIBLE_BITS;
	if (vm_memo_on && vm_memo_n < VM_MEMO_MAX)
		vm_memo[vm_memo_n++] = (typeof(vm_memo[0])) {RelationGetRelid(heap), nblocks, p.visible, p.clean};
	*visible = p.visible;
	if (clean)
		*clean = p.clean;
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
	/* the kernels read 16 bytes for each mask word, which a superblock of fewer than 64 pages (the 8-page test flavour) does not have: zeros */
	uint8		bytes[SIFT_PAGEMASK_WORDS * 16] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	uint64		first = (uint64) sb * VM_BYTES_PER_SB;
	uint32		off = first % VM_MAPSIZE,
				head = Min(VM_BYTES_PER_SB, VM_MAPSIZE - off);

	if (!v->use_vm)
	{
		memset(pagemask, 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
		return;
	}
	TEST_PAUSE();
	memset(bytes + VM_BYTES_PER_SB, 0, sizeof(bytes) - VM_BYTES_PER_SB);
	vm_read(v, first / VM_MAPSIZE, off, head, bytes);
	if (head < VM_BYTES_PER_SB)
		vm_read(v, first / VM_MAPSIZE + 1, 0, VM_BYTES_PER_SB - head, bytes + head);
	sift_kern->vm_allvisible(pagemask, bytes);
	if (any_page(pagemask) && !dir_current(v))
		memset(pagemask, 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
}

/* The map page mapblk through the one buffer the scan keeps pinned (v->vmbuf); NULL past the end of the fork */
static const uint8 *
vm_map(SiftVis *v, BlockNumber mapblk)
{
	if (mapblk >= v->vm_nblocks)
		return NULL;
	if (!BufferIsValid(v->vmbuf) || BufferGetBlockNumber(v->vmbuf) != mapblk)
	{
		if (BufferIsValid(v->vmbuf))
			ReleaseBuffer(v->vmbuf);
		v->vmbuf = ReadBufferExtended(v->heap, VISIBILITYMAP_FORKNUM, mapblk, RBM_ZERO_ON_ERROR, NULL);
	}
	return (const uint8 *) PageGetContents(BufferGetPage(v->vmbuf));
}

/* The all-visible bits of the table pages [b0, b1) of a map page (numbered from its first page), all set? The first and the last byte are cut to their pages, the middle is read 32 pages at a time */
static bool
vm_slice_allvisible(const uint8 *map, uint64 b0, uint64 b1)
{
	uint64		i = b0 / 4,
				end = (b1 + 3) / 4;

	while (i < end)
	{
		uint8		mask = VM_BYTE_VISIBLE;

		if (i == b0 / 4)
			mask &= (uint8) (VM_BYTE_VISIBLE << (b0 % 4 * 2));
		if (i + 1 == end && b1 % 4 != 0)
			mask &= (uint8) (VM_BYTE_VISIBLE >> ((4 - b1 % 4) * 2));
		if (mask == VM_BYTE_VISIBLE && i + 8 <= b1 / 4)
		{
			uint64		w;

			memcpy(&w, map + i, sizeof(w));
			if ((w & VM_VISIBLE_BITS) != VM_VISIBLE_BITS)
				return false;
			i += 8;
			continue;
		}
		if ((map[i] & mask) != mask)
			return false;
		i++;
	}
	return true;
}

/*
 * Is every page in [lo, hi] all-visible? Only the bytes of the map those pages use are read, in at most SIFT_VIS_RANGE_MAP_PAGES map pages (each covers 255 MB of a heap): a wider range says no,
 * and the caller asks the pages it meets one by one (sift_vis_blocks_allvisible). Nothing is kept past the call, so a bit that changes is read as it is now.
 */
bool
sift_vis_range_allvisible(SiftVis *v, BlockNumber lo, BlockNumber hi)
{
	uint64		pages_per_map = (uint64) VM_MAPSIZE * 4,
				from = lo,
				to;

	if (!v->use_vm || v->heap_nblocks == 0)
		return false;
	to = Min((uint64) hi + 1, (uint64) v->heap_nblocks);
	if (from >= to)
		return dir_current(v);
	if ((to - 1) / pages_per_map - from / pages_per_map >= SIFT_VIS_RANGE_MAP_PAGES)
		return false;
	while (from < to)
	{
		uint64		mapblk = from / pages_per_map,
					upto = Min(to, (mapblk + 1) * pages_per_map);
		const uint8 *map = vm_map(v, (BlockNumber) mapblk);

		if (map == NULL || !vm_slice_allvisible(map, from - mapblk * pages_per_map, upto - mapblk * pages_per_map))
			return false;
		from = upto;
	}
	return dir_current(v);
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
	/* the rows come by address, so a page's rows are a run, which has one answer */
	for (uint32 i = 0, j; i < n; i = j)
	{
		uint64		byteno = blocks[i] / 4;
		BlockNumber mapblk = byteno / VM_MAPSIZE;

		for (j = i + 1; j < n && blocks[j] == blocks[i]; j++)
			;
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
			for (uint32 r = i; r < j;)
			{
				uint32		upto = Min(j, (r | 63) + 1);

				bits[r / 64] |= (upto - r == 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << (upto - r)) - 1) << (r % 64);
				r = upto;
			}
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
static pg_attribute_always_inline bool
quick_visible(const HeapPage *p, OffsetNumber off)
{
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
	/* a committed xmin that is not frozen is a normal xid: the compare of TransactionIdPrecedes, without its call (it is a call a row) */
	return HeapTupleHeaderXminFrozen(th) || (int32) (HeapTupleHeaderGetRawXmin(th) - p->v->snapshot->xmin) < 0;
}

/* The TID is built for the full way only: a call through a TID test, per row, costs more than the test */
static pg_attribute_always_inline bool
row_visible(const HeapPage *p, BlockNumber blk, OffsetNumber off)
{
	ItemPointerData tid;
	HeapTupleData tuple;
	bool		all_dead;

	if (quick_visible(p, off))
		return true;
	ItemPointerSet(&tid, blk, off);
	return heap_hot_search_buffer(&tid, p->v->heap, p->buf, p->v->snapshot, &tuple, &all_dead, true);
}

/*
 * A page the scan asks about again (units cut by a key, not by the heap, take a few rows of each page): the second visit computes the
 * visibility of every row of the page once and the later ones are a mask. A snapshot does not change its mind about a tuple, so the mask
 * holds for the rest of the scan. Entries are capped (4,096 pages, about 230 kB a scan) and never evicted: the read stream skips the pages the cache holds whole, so an entry it saw has to be there when the page is filtered. A page past the cap is read every time.
 */
#define SIFT_VIS_PAGE_CACHE_MAX	4096

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

bool
sift_vis_has_chains(Relation heap)
{
	return sift_tbl_is_table(heap) && sift_tbl_has_chains(heap);
}

void
sift_vis_chain_members(Relation heap, uint32 *pos, BlockNumber blk, const uint64 *offmask, SiftVisKeyFn fn, void *arg)
{
	sift_tbl_chain_members(heap, pos, blk, offmask, fn, arg);
}

/* Whether sift_vis_filter_page reads the page: it is in the heap and its visibility is not cached whole */
static bool
page_read_wanted(SiftVis *v, BlockNumber blk)
{
	PageVis    *e = v->pagevis ? hash_search(v->pagevis, &blk, HASH_FIND, NULL) : NULL;

	if (blk >= v->heap_nblocks)
		v->heap_nblocks = sift_vis_nblocks(v->heap);
	return blk < v->heap_nblocks && (e == NULL || !e->full);
}

static BlockNumber
stream_next(ReadStream *stream, void *arg, void *per_buffer)
{
	SiftVis    *v = arg;

	for (BlockNumber blk; (blk = v->next(v->next_arg)) != InvalidBlockNumber;)
		if (page_read_wanted(v, blk))
			return blk;
	return InvalidBlockNumber;
}

void
sift_vis_stream_begin(SiftVis *v, SiftVisNext next, void *arg, bool whole)
{
	if (v->stream != NULL || sift_tbl_is_table(v->heap))
		return;
	v->next = next;
	v->next_arg = arg;
	v->whole = whole;
	v->stream = read_stream_begin_relation(READ_STREAM_DEFAULT, NULL, v->heap, MAIN_FORKNUM, stream_next, v, 0);
}

void
sift_vis_stream_end(SiftVis *v)
{
	if (v->stream == NULL)
		return;
	read_stream_end(v->stream);
	v->stream = NULL;
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
		return ((SiftTblFetchHeader *) v->fetch)->ops->filter_page(v->fetch, v->snapshot, blk, offmask);
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
		full = e != NULL && (found || v->whole);
	}
	page.buf = v->stream ? read_stream_next_buffer(v->stream, NULL) : ReadBuffer(v->heap, blk);
	SIFT_INVARIANT(BufferGetBlockNumber(page.buf) == blk, "sift: the heap page stream is out of step (block %u, expected %u)", BufferGetBlockNumber(page.buf), blk);
	LockBuffer(page.buf, BUFFER_LOCK_SHARE);
	page.page = BufferGetPage(page.buf);
	page.maxoff = PageGetMaxOffsetNumber(page.page);
	page.quick = IsMVCCSnapshot(v->snapshot) && !IsolationIsSerializable();
	v->heap_pages++;
	if (full)
	{
		memset(e->vis, 0, sizeof(e->vis));
		for (OffsetNumber off = FirstOffsetNumber; off <= page.maxoff && off <= SIFT_OFFMASK_WORDS * 64; off++)
			if (row_visible(&page, blk, off))
				e->vis[(off - 1) / 64] |= UINT64CONST(1) << ((off - 1) % 64);
		e->full = true;
		for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
		{
			offmask[wd] &= e->vis[wd];
			left += sift_pc64(offmask[wd]);
		}
	}
	else
		for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
			for (uint64 m = offmask[wd]; m; m &= m - 1)
			{
				if (row_visible(&page, blk, wd * 64 + __builtin_ctzll(m) + 1))
					left++;
				else
					offmask[wd] &= ~(m & -m);
			}
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
