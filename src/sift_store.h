/*
 * sift_store.h - page IO, metapage/directory access, extent allocation,
 * segment byte streams and WAL helpers.
 *
 * Only this module reads or writes the metapage, the directory and the free
 * list. WAL rules: generic WAL for in-place changes (metapage first, <= 4
 * buffers, no ereport between start and finish); new pages are logged as full
 * images before anything references them.
 */
#ifndef SIFT_STORE_H
#define SIFT_STORE_H

#include "storage/bufmgr.h"
#include "utils/rel.h"

#include "sift_page.h"

/* Page helpers */
extern void sift_page_init(Page page, SiftPageType type, uint64 gen);

/* ERROR (ERRCODE_INDEX_CORRUPTED, or serialization failure on a standby) on mismatch */
extern void sift_store_check_page(Relation index, Buffer buf, SiftPageType type, uint64 gen);

/* Directory snapshot: plain copy of metapage + directory (invariant I3) */
typedef struct SiftDirSnapshot
{
	SiftMetaPageData meta;
	uint32		version;
	int			nsegs;
	SiftSegEntry segs[SIFT_MAX_SEGMENTS];
} SiftDirSnapshot;

extern void sift_dir_read(Relation index, SiftDirSnapshot *snap);

/*
 * Atomic directory update: locks metapage and directory exclusively, lets fn
 * edit working copies (fn must not ereport), records extents released by the
 * change in the free list with safexid = ReadNextFullTransactionId(), and
 * logs everything with one generic WAL record (metapage, directory, free
 * list page, at most one extra buffer already exclusively locked by the
 * caller). Bumps the directory version.
 */
typedef void (*SiftDirMutator) (SiftMetaPageData *meta, SiftDirectoryData *dir, void *arg);

typedef struct SiftDirChange
{
	SiftDirMutator fn;
	void	   *arg;
	const SiftExtent *release;
	int			nrelease;
	Buffer		extra;			/* InvalidBuffer if none */
} SiftDirChange;

extern void sift_dir_update(Relation index, const SiftDirChange *chg);

/* Initializes metapage + empty directory (ambuild, ambuildempty) via a write target */
typedef struct SiftWriteTarget SiftWriteTarget;
extern void sift_store_init_meta(SiftWriteTarget *wt, const SiftOptions *opts, uint32 analyzer_hash);

/*
 * Allocation. Extents are runs of contiguous blocks, multiples of 16 for
 * readahead; reused from the free list when safexid is past the horizon,
 * else by extending the relation. Returned buffers are pinned, exclusively
 * locked, zeroed and initialized with type/gen.
 */
extern Buffer sift_alloc_page(Relation index, SiftPageType type, uint64 gen);
extern BlockNumber sift_alloc_extent(Relation index, uint32 nblocks, SiftPageType type,
									 uint64 gen, Buffer *bufs);

/* Makes free-list extents whose safexid passed the horizon reusable; returns count */
extern int	sift_recycle(Relation index);

/*
 * Write targets: BULK (ambuild: smgr_bulk_*, single writer, WAL skipped when
 * not needed) or SHARED (promotion/merge: shared buffers + log_newpage_range
 * at finish). Pages written through a target are reachable only after the
 * caller publishes them with sift_dir_update().
 */
typedef enum SiftWriteMode
{
	SIFT_WRITE_BULK,
	SIFT_WRITE_SHARED
} SiftWriteMode;

extern SiftWriteTarget *sift_wt_begin(Relation index, SiftWriteMode mode);
extern void sift_wt_finish(SiftWriteTarget *wt);	/* flush + WAL-log all pages */

/* Sequential byte stream over extents allocated from a write target */
typedef struct SiftStreamWriter SiftStreamWriter;

extern SiftStreamWriter *sift_sw_begin(SiftWriteTarget *wt, uint64 gen);
extern uint64 sift_sw_tell(const SiftStreamWriter *sw);
extern void sift_sw_write(SiftStreamWriter *sw, const void *data, size_t len);
extern void sift_sw_align(SiftStreamWriter *sw, size_t align);
/* Returns the extent list in stream order (palloc'd), *nextents set */
extern SiftExtent *sift_sw_finish(SiftStreamWriter *sw, int *nextents);

/* Writes a single non-stream page (segment header, LIVE pages) through the target */
extern BlockNumber sift_wt_write_page(SiftWriteTarget *wt, Page page);
extern BlockNumber sift_wt_reserve(SiftWriteTarget *wt, uint32 nblocks);

/*
 * Stream reader over a segment's extents. sift_sr_get returns a pointer to
 * len bytes at logical offset off: into the buffer page when the range fits
 * one page (pinned + share-locked until the next call or release), else a
 * copy in scratch (caller-provided, >= len bytes). Page type/gen are checked
 * on every page change.
 */
typedef struct SiftStreamReader
{
	Relation	index;
	const SiftExtent *extents;
	int			nextents;
	uint64		gen;
	Buffer		buf;			/* current page or InvalidBuffer */
	BlockNumber blkno;
	struct ReadStream *stream;	/* optional prefetch stream */
} SiftStreamReader;

extern void sift_sr_init(SiftStreamReader *sr, Relation index, const SiftExtent *extents,
						 int nextents, uint64 gen);
extern const void *sift_sr_get(SiftStreamReader *sr, uint64 off, size_t len, void *scratch);
extern void sift_sr_prefetch(SiftStreamReader *sr, uint64 off, uint64 len);
extern void sift_sr_release(SiftStreamReader *sr);

/* Blocks referenced by any directory entry or the free list, for the orphan sweep */
extern struct Bitmapset *sift_store_referenced_blocks(Relation index, const SiftDirSnapshot *snap);

#endif							/* SIFT_STORE_H */
