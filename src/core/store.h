/*
 * core/store.h - page IO, metapage/directory access, extent allocation,
 * segment byte streams and WAL helpers.
 *
 * Only this module reads or writes the metapage, the directory and the free
 * list. Crash-safety protocol (recovery relies on these, in WAL order):
 *
 * P1 Reachability. A page becomes reachable only from the directory, the free
 *    list or the metapage. Its content is WAL-logged (full image) before the
 *    record that makes it reachable, so replay never exposes an unlogged page.
 * P2 Atomic switch. Segment add/drop/replace, metapage totals and the freeing
 *    of dropped extents are one generic WAL record: metapage, directory, at
 *    most one free-list page, at most one caller page (registered in that
 *    order, which is also the buffer lock order on a standby). Releases that
 *    do not fit an existing free-list page go to new free-list pages that are
 *    logged first and linked by that same record.
 * P3 Allocation. Taking blocks off the free list, or raising high_water, is
 *    logged before the blocks are written. Extending the relation logs a full
 *    image of the new last block in the same record, so after replay the
 *    physical size is >= high_water. A crash between allocation and
 *    publication leaves orphans (allocated, unreferenced), never two owners;
 *    sift_store_orphans finds them for the sweep.
 * P4 Reuse. Released extents carry safexid = the releasing transaction's
 *    xid, assigned before the directory lock, so every snapshot that could
 *    hold the old directory has xmin <= safexid. They are reused once safexid
 *    precedes the index's GlobalVis horizon, i.e. as soon as the releasing
 *    transaction and those readers are gone. (In parallel mode, which cannot
 *    assign an xid, safexid is the next xid read after the lock: as safe,
 *    reusable once a later xid completes. A lazy VACUUM is left out of the
 *    snapshots and of the horizon, its xid included, so a reader that began
 *    after that xid was assigned could see it as passed: a VACUUM releases
 *    with the next xid too, and its truncation, which excludes every reader
 *    (P7), counts every released extent as reusable.) New content enters a reused
 *    block's buffer only inside the critical section that logs it, so
 *    WAL-before-data orders it after the release record.
 * P5 Standby. Generic WAL raises no recovery conflicts. Readers check page
 *    type/gen; a mismatch for a generation the replayed directory no longer
 *    lists raises ERRCODE_T_R_SERIALIZATION_FAILURE (retry), anything else is
 *    ERRCODE_INDEX_CORRUPTED.
 * P6 Generations. sift_store_new_gen logs the reservation before any page
 *    carries the gen, so durable pages never share a gen across segments.
 * P7 Truncation. The reusable free run at the end of the relation is given back
 *    in two steps, in this order: one record writes the free list without it
 *    and lowers high_water to its start (P2), then RelationTruncate cuts the
 *    file. A crash between them leaves a file longer than high_water, which
 *    allocation extends over and the next truncation cuts; the reverse order
 *    could leave the free list past the end. Only blocks that are reusable (P4)
 *    go, and the caller holds AccessExclusiveLock on the index (conditionally,
 *    like VACUUM's truncation of a heap): no backend then has a scan, a
 *    directory copy or an open write target, a standby replays the lock before
 *    the truncation and cancels what holds the index there, and the relcache
 *    entries (the write buffer appenders' cached lane tails) are invalidated before
 *    the lock is released.
 *
 * Store records are not transactional and nothing here flushes WAL: a change
 * is durable once WAL is flushed past it, by the commit of a transaction that
 * has an xid and wrote WAL (a releasing sift_dir_update qualifies), the WAL
 * writer or a checkpoint. A crash may lose recent changes (whose blocks then
 * become orphans) but never exposes a partial one. Index content stays
 * durable through the commits of the inserting transactions.
 *
 * Readers must take a registered snapshot (MyProc->xmin) before sift_dir_read
 * and keep it for as long as they use the directory copy: that is what P4
 * waits for. Unlogged and temporary indexes skip WAL; under wal_level=minimal
 * a relfilenode created in the current transaction skips WAL and is synced at
 * commit.
 */
#ifndef SIFT_STORE_H
#define SIFT_STORE_H

#include "core/host.h"

#include "core/page.h"

/* ERRCODE_INDEX_CORRUPTED: a page, record or directory of the index that the format does not allow (index: the Relation) */
#define SIFT_CORRUPT(index, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift index \"%s\" is corrupted", RelationGetRelationName(index)), \
			 errdetail(__VA_ARGS__)))

/* The same for one segment of the index (gen: its generation) */
#define SIFT_SEG_CORRUPT(index, gen, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift index \"%s\" segment %llu is corrupted", RelationGetRelationName(index), (unsigned long long) (gen)), \
			 errdetail(__VA_ARGS__)))

#ifdef SIFT_TESTING
/*
 * Testing knobs (sift.test_*, 0 = off): pages a SHARED write target writes before it fails, extents a
 * segment stream may use, a block allocated by another backend after every run a SHARED stream takes
 */
extern PGDLLIMPORT int sift_test_fail_pages;
extern PGDLLIMPORT int sift_test_max_extents;
extern PGDLLIMPORT bool sift_test_run_gap;
extern void sift_store_test_init(void);
#endif

/* sift.standby_conflicts: reuse of released blocks is logged as a recovery conflict (log_reuse in core/freelist.c) */
extern PGDLLIMPORT bool sift_standby_conflicts;

/* Page helpers: zeroes the page, standard header, special type/gen */
extern void sift_page_init(Page page, SiftPageType type, uint64 gen);

/*
 * ERROR (ERRCODE_INDEX_CORRUPTED, or serialization failure on a standby) on
 * mismatch. buf must be locked and be the only buffer lock the caller holds:
 * on a standby the mismatch path unlocks it to consult the directory.
 */
extern void sift_store_check_page(Relation index, Buffer buf, SiftPageType type, uint64 gen);

/* Directory snapshot: plain copy of metapage + directory (invariant I3) */
typedef struct SiftDirSnapshot
{
	SiftMetaPageData meta;
	uint32		version;
	int			nsegs;
	SiftSegEntry segs[SIFT_MAX_SEGMENTS];
} SiftDirSnapshot;

/* The caller holds no buffer lock: the metapage comes first in lock order */
extern void sift_dir_read(Relation index, SiftDirSnapshot *snap);

/*
 * The directory's version through a pinned directory buffer, without its content lock: for a reader that compares it with a version it
 * read before (sift_vis_sb_mask), once per superblock. The version is one aligned word that a directory update stores under the exclusive
 * lock, and the read follows the caller's earlier read with a read barrier, so a store that preceded what the caller saw is seen.
 */
extern Buffer sift_dir_pin(Relation index);
extern uint32 sift_dir_version_peek(Buffer dirbuf);

/* The directory's version alone, read under its share lock: bumped by every directory update */
extern uint32 sift_dir_version(Relation index);

/*
 * Atomic directory update: locks metapage and directory exclusively, lets fn
 * edit working copies (fn must not ereport or allocate), records extents
 * released by the change in the free list with safexid = the caller's top
 * transaction id (assigned if needed, P4), and logs everything with one generic WAL
 * record (metapage, directory, free list page, at most one extra buffer
 * already exclusively locked by the caller, whose working copy fn edits; NULL
 * when there is none). fn may reserve generations (meta->next_gen++) and edit
 * totals; it must not touch high_water or freelist_head. Bumps the directory
 * version. Released blocks must be allocated and not already free.
 */
typedef void (*SiftDirMutator) (SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra,
								void *arg);

typedef struct SiftDirChange
{
	SiftDirMutator fn;			/* NULL: only release */
	void	   *arg;
	const SiftExtent *release;
	int			nrelease;
	Buffer		extra;			/* InvalidBuffer if none */
	bool		immediate;		/* no snapshot can hold the released extents (a relation still being written by its creator): reusable at once */
} SiftDirChange;

typedef struct SiftWriteTarget SiftWriteTarget;

extern void sift_dir_update(Relation index, const SiftDirChange *chg);
/* Publish a completed SHARED write target; subsequent abort frees only unpublished allocations. */
extern void sift_wt_publish(SiftWriteTarget *wt, const SiftDirChange *chg);

/* Reserves a fresh segment generation (logged, P6) */
extern uint64 sift_store_new_gen(Relation index);

/* Is xid older than every snapshot (the heap's GlobalVis horizon)? */
extern bool sift_store_removable(Relation index, FullTransactionId xid);

/*
 * While on, every extent released is reusable at once and every free record counts as reusable: for a caller that holds AccessExclusiveLock on
 * the relation (VACUUM's space step, sift_maint_space), which no scan, write buffer appender or other step can hold at the same time (P7).
 * The caller turns it off again before it releases the lock, also on error.
 */
extern void sift_store_space_exclusive(Relation index, bool on);

#ifdef SIFT_TESTING
extern PGDLLIMPORT int sift_test_space_crash;
extern PGDLLIMPORT bool sift_test_space_error;
extern void sift_store_space_test_point(int phase);
#define SIFT_SPACE_TEST_POINT(phase) sift_store_space_test_point(phase)
extern PGDLLIMPORT int sift_test_crash_dir;
extern PGDLLIMPORT bool sift_test_crash_worker;
extern PGDLLIMPORT int sift_test_dir_updates;
extern void sift_store_dir_test_point(bool after);
#define SIFT_DIR_TEST_POINT(after) sift_store_dir_test_point(after)
#else
#define SIFT_SPACE_TEST_POINT(phase) ((void) 0)
#define SIFT_DIR_TEST_POINT(after) ((void) 0)
#endif
extern uint64 sift_store_space_written(void);

/* A host's extra physical links in a copied lane page. Edits only this page; no allocation or IO. */
typedef void (*SiftRelocatePage) (Page page, const SiftExtent *old, const SiftExtent *fresh, int n);
extern BlockNumber sift_store_relocated_block(BlockNumber blk, const SiftExtent *old, const SiftExtent *fresh, int n);

/* Copy through a tracked SHARED target into exact free runs below limit, within budget; zero if they do not fit. Publish with sift_wt_publish. */
extern uint64 sift_store_relocate(Relation rel, SiftWriteTarget *wt, const SiftSegEntry *entry, const SiftExtent *old, int n,
								 BlockNumber limit, uint64 budget, SiftSegEntry *fresh, SiftRelocatePage relocate_page);

/* The oldest xid a snapshot may still hold, computed now: records released before it are reusable */
extern FullTransactionId sift_store_horizon(Relation index);
extern uint64 sift_store_held_blocks(Relation index);

/*
 * The free-list pages in chain order, each share-locked and validated while visit runs on it (blk is its block);
 * visit returns false to stop. metabuf is locked: it guards the list.
 */
typedef bool (*SiftFreeListVisit) (BlockNumber blk, const SiftFreeListData *fl, void *arg);
extern void sift_freelist_walk(Relation index, Buffer metabuf, SiftFreeListVisit visit, void *arg);

typedef struct SiftFreeStats
{
	int64		pages;			/* pages of the list itself */
	int64		records;
	int64		blocks;
	int64		reusable;		/* blocks whose releasing transactions are older than every snapshot */
} SiftFreeStats;

extern SiftFreeStats sift_store_free_stats(Relation index);
extern int64 sift_store_free_below(Relation index, BlockNumber limit, uint32 min_run);

/*
 * Initializes metapage + empty directory (ambuild, ambuildempty) via a BULK
 * or INIT write target; the pages are written by sift_wt_finish with the
 * final high-water mark. A BULK build numbers its segments from gen 1 and
 * sets meta->next_gen when it publishes them.
 */
extern void sift_store_init_meta(SiftWriteTarget *wt, const SiftOptions *opts, uint32 analyzer_hash);
/* The same with the scoring constants and the metapage flags given (a column index: k1 = b = 0, schema hash, SIFT_COL_META_FLAG) */
extern void sift_store_init_meta_ex(SiftWriteTarget *wt, float4 k1, float4 b, uint32 analyzer_hash, uint32 flags);

/*
 * Allocation. Extents are runs of contiguous blocks; reused from the free
 * list when safexid is past the horizon (first fit), else by extending the
 * relation. With bufs, the nblocks (<= SIFT_ALLOC_MAX_LOCKED) returned
 * buffers are pinned, exclusively locked and initialized with type/gen, and
 * that initialization is already WAL-logged: callers log their own changes
 * as generic WAL deltas. With bufs = NULL (maintenance lock holders only) the
 * blocks are untouched; each must be initialized and logged in one critical
 * section before it is published.
 */
#define SIFT_ALLOC_MAX_LOCKED	32

extern Buffer sift_alloc_page(Relation index, SiftPageType type, uint64 gen);
extern BlockNumber sift_alloc_extent(Relation index, uint32 nblocks, SiftPageType type,
									 uint64 gen, Buffer *bufs);

/*
 * Clears the safexid of free-list records past the horizon and coalesces
 * adjacent reusable records; returns the number of records made reusable.
 * Allocation also checks the horizon itself, so this only speeds it up.
 */
extern int	sift_recycle(Relation index);

/*
 * Blocks at the end of the file that sift_store_truncate would give back (an estimate: the free
 * list can change before the caller has the lock).
 */
extern BlockNumber sift_store_tail_blocks(Relation index);

/*
 * Gives back the blocks at the end of the relation that are free (and reusable) and the file past
 * high_water (P7); returns how many. The caller holds AccessExclusiveLock on the index and the
 * maintenance lock exclusively. Frees the list's own pages as well when they are in the way.
 */
extern BlockNumber sift_store_truncate(Relation index);

/*
 * Write targets (allocate in CurrentMemoryContext):
 *   BULK   ambuild into a new relfilenode: smgr_bulk_*, single writer, WAL
 *          skipped when not needed; blocks 0-1 are written at finish.
 *   INIT   ambuildempty: BULK on the init fork, always WAL-logged.
 *   SHARED promotion/merge: shared buffers, each page logged as a full image
 *          when written; unused allocated blocks go back to the free list at
 *          finish.
 * Pages written through a target are reachable only after the caller
 * publishes them with sift_dir_update() after sift_wt_finish().
 */
typedef enum SiftWriteMode
{
	SIFT_WRITE_BULK,
	SIFT_WRITE_SHARED,
	SIFT_WRITE_INIT
} SiftWriteMode;

extern SiftWriteTarget *sift_wt_begin(Relation index, SiftWriteMode mode);
extern void sift_wt_finish(SiftWriteTarget *wt);	/* flush + WAL-log all pages */
#ifdef SIFT_TESTING
/* core/store_test.c: a test segment of payload bytes of content seed (0: zeros), written in pieces and aligned as the header records */
extern void sift_test_write_segment(SiftWriteTarget *wt, uint64 gen, uint64 payload, uint64 seed, uint32 piece, uint32 align,
									SiftSegEntry *entry);
#endif

/*
 * SHARED targets: the bytes the streams written through wt will take about (upper bound of a merge,
 * a promotion's write buffer pages). Streams then reuse free space in runs as short as a 256th of it
 * rather than extending the file for long ones.
 */
extern void sift_wt_expect(SiftWriteTarget *wt, uint64 bytes);

/*
 * SHARED targets: makes the I/O charged to wt (a page written through it, and what sift_wt_charge adds for pages the caller reads)
 * sleep delay_ms for every 16 pages, like vacuum_cost_delay; 0 (the default) never sleeps.
 */
extern void sift_wt_throttle(SiftWriteTarget *wt, int delay_ms);
extern void sift_wt_charge(SiftWriteTarget *wt, uint64 pages);

/*
 * SHARED targets: the page that would take the WAL this backend wrote since this call past kb_per_sec kilobytes per second waits (interruptibly)
 * before it is written, so a long merge cannot take the WAL of the foreground; 0 (the default) never waits.
 */
extern void sift_wt_wal_rate(SiftWriteTarget *wt, int kb_per_sec);

/*
 * SHARED targets: after an ERROR rolled the writing transaction back, gives every block the target
 * took (written or not, published or not: the caller found nothing published) back to the free list.
 */
extern void sift_wt_abort(SiftWriteTarget *wt);

/*
 * Extents of a stream, the same bookkeeping in every store: the writer collects the blocks of its pages into extents
 * (runs of contiguous blocks), the reader maps a logical page to its block through the first logical page of each extent.
 */
typedef struct SiftExtentList
{
	SiftExtent *ext;
	int			n;
	int			cap;
	int			max;			/* extents a stream may use: what a header page lists, less for tests */
} SiftExtentList;

static inline void
sift_extlist_init(SiftExtentList *l)
{
	l->n = 0;
	l->cap = 8;
	l->max = SIFT_MAX_EXTENTS;
	l->ext = palloc(l->cap * sizeof(SiftExtent));
}

/* The next page of the stream is block blk: it extends the last extent when it follows it */
static inline void
sift_extlist_add(SiftExtentList *l, BlockNumber blk)
{
	SiftExtent *last = l->n > 0 ? &l->ext[l->n - 1] : NULL;

	if (last && last->start + last->nblocks == blk)
	{
		last->nblocks++;
		return;
	}
	if (l->n == l->max)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift segment stream needs more than %d extents", l->max),
				 errhint("Run sift.merge() on the index so that the segment is rewritten contiguously, or REINDEX it if this repeats.")));
	if (l->n == l->cap)
	{
		l->cap *= 2;
		l->ext = repalloc(l->ext, l->cap * sizeof(SiftExtent));
	}
	l->ext[l->n++] = (SiftExtent) {blk, 1};
}

/* pagestart[i] = first logical page of extent i, pagestart[n] = the pages of the stream (returned) */
static inline uint64
sift_extents_pagestart(const SiftExtent *ext, int n, uint32 *pagestart)
{
	uint64		total = 0;

	for (int i = 0; i < n; i++)
	{
		pagestart[i] = total;
		total += ext[i].nblocks;
	}
	pagestart[n] = total;
	return total;
}

/* The block of logical page lpage; *hint is the extent of the last lookup, where the search starts */
static inline BlockNumber
sift_extents_block(const SiftExtent *ext, const uint32 *pagestart, int n, uint32 lpage, int *hint)
{
	int			lo = *hint;

	if (lpage < pagestart[lo] || lpage >= pagestart[lo + 1])
	{
		int			hi = n - 1;

		lo = 0;
		while (lo < hi)
		{
			int			mid = (lo + hi + 1) / 2;

			if (pagestart[mid] <= lpage)
				lo = mid;
			else
				hi = mid - 1;
		}
		*hint = lo;
	}
	return ext[lo].start + (lpage - pagestart[lo]);
}

/*
 * Sequential byte stream over extents allocated from a write target, as
 * SIFT_PT_DATA pages. Extents grow from 16 to 1024 blocks.
 */
typedef struct SiftStreamWriter SiftStreamWriter;

extern SiftStreamWriter *sift_sw_begin(SiftWriteTarget *wt, uint64 gen);
extern uint64 sift_sw_tell(const SiftStreamWriter *sw);
extern void sift_sw_write(SiftStreamWriter *sw, const void *data, size_t len);
/* The first len bytes of a temporary file (the spool of a section written out of order), from its start */
extern void sift_sw_write_file(SiftStreamWriter *sw, BufFile *file, uint64 len);

/*
 * Zero-pads (align: power of two <= SIFT_SIMD_ALIGN) so the next byte's
 * address inside its page buffer is a multiple of align. Buffer pages are
 * PG_IO_ALIGN_SIZE-aligned, so direct pointers from sift_sr_get to data that
 * fits one page are then SIMD-aligned. Logical offsets are not multiples of
 * align (pages hold SIFT_PAGE_PAYLOAD bytes from SIFT_PAGE_DATA_OFFSET).
 */
extern void sift_sw_align(SiftStreamWriter *sw, size_t align);

/* Returns the extent list in stream order (palloc'd), *nextents set */
extern SiftExtent *sift_sw_finish(SiftStreamWriter *sw, int *nextents);

/*
 * Single non-stream pages (segment header, LIVE pages). The page must be
 * built with sift_page_init and pd_lower must cover its content.
 * sift_wt_reserve returns the first of nblocks contiguous blocks; each of
 * them must then be written with sift_wt_put_page.
 */
extern BlockNumber sift_wt_write_page(SiftWriteTarget *wt, Page page);
extern BlockNumber sift_wt_reserve(SiftWriteTarget *wt, uint32 nblocks);
extern void sift_wt_put_page(SiftWriteTarget *wt, BlockNumber blkno, Page page);

/*
 * Stream reader over a segment's extents. sift_sr_get returns a pointer to
 * len bytes at logical offset off: into the buffer page when the range fits
 * one page (pinned + share-locked until the next call or release), else a
 * copy in scratch (caller-provided, >= len bytes). Page type/gen are checked
 * on every page change, and reads past a page's written bytes are corrupt.
 * sift_sr_init allocates a small extent index in CurrentMemoryContext;
 * the remaining fields are private.
 */
typedef struct SiftStreamReader
{
	Relation	index;
	const SiftExtent *extents;
	int			nextents;
	uint64		gen;
	Buffer		buf;			/* current page or InvalidBuffer */
	BlockNumber blkno;
	bool		locked;			/* buf share-locked */
	int			cur;			/* extent of the last lookup */
	uint32		npages;
	uint32	   *pagestart;		/* first logical page of each extent, [nextents + 1] */
	struct ReadStream *stream;	/* optional prefetch stream */
	int			pf_cur;
	uint32		pf_issue;		/* next page the stream callback yields */
	uint32		pf_next;		/* next page the stream returns */
	uint32		pf_end;
} SiftStreamReader;

extern void sift_sr_init(SiftStreamReader *sr, Relation index, const SiftExtent *extents,
						 int nextents, uint64 gen);
extern const void *sift_sr_get(SiftStreamReader *sr, uint64 off, size_t len, void *scratch);

/* Reads [off, off + len) ahead through a ReadStream for the following gets */
extern void sift_sr_prefetch(SiftStreamReader *sr, uint64 off, uint64 len);
extern void sift_sr_release(SiftStreamReader *sr);

/* Drops the share lock the last read left on its page (a pointer it returned is invalid): the caller may then serve interrupts */
extern void sift_sr_unlock(SiftStreamReader *sr);

/*
 * Blocks referenced by any directory entry, the free list or the metapage
 * and directory themselves (allocates in CurrentMemoryContext). ERROR if a
 * block is referenced twice or lies past the high-water mark.
 */
extern struct Bitmapset *sift_store_referenced_blocks(Relation index, const SiftDirSnapshot *snap);

/* Every block of an immutable segment (header, streams, liveness run) as extents; palloc'd */
extern SiftExtent *sift_store_immutable_extents(Relation index, const SiftSegEntry *s, int *nextents);

/*
 * Orphans: blocks below the high-water mark that nothing references and no
 * writer owns. A page whose gen is in the directory (an unlinked write buffer
 * page) or >= next_gen (a segment being written) is in flight, never an
 * orphan. The caller holds the maintenance lock, so the only concurrent
 * writers are write buffer appenders of published segments.
 */
extern struct Bitmapset *sift_store_orphans(Relation index);

/*
 * Checks metapage, directory, free list (bounds, overlap, chain, reference
 * conflicts) and that the relation covers high_water. ERRCODE_INDEX_CORRUPTED.
 */
extern void sift_store_verify(Relation index);

#ifdef SIFT_TESTING
/* The index of a test function's argument, opened with mode; ERRCODE_WRONG_OBJECT_TYPE for another relation */
extern Relation sift_test_open_index(Oid relid, LOCKMODE mode);
#endif

#endif							/* SIFT_STORE_H */
