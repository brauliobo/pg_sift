/*
 * l0_shard_dir.h - the sharded active L0 slab of a hot partition: K lock-striped slab chains behind one directory page
 * (SIFT_L0_SLAB_SHARD_DIR, spec 25 1.2, 1.3, 1.5), with O(1) reverse-linked appends, a directory scan for readers and the
 * two-phase copy-on-write flush that seals the active slabs while writers go on into a fresh directory.
 *
 * Pages are handled as BLCKSZ images through SiftL0PageIO, so the same code runs on buffers (generic WAL records) and on the
 * page store of the standalone test. No page type, GUC or reloption is added: the directory is a SIFT_PT_L0_SLAB page.
 */
#ifndef SIFT_L0_SHARD_DIR_H
#define SIFT_L0_SHARD_DIR_H

#include "col/f2/l0/group_commit.h"
#include "col/f2/l0/href.h"
#include "col/f2/l0/slab.h"
#include "col/f2/del/pend.h"

#include "access/transam.h"
#include "storage/bufpage.h"

#define SIFT_L0_SHARD_MAX			4
#define SIFT_L0_SHARD_UNKEYED		8
#define SIFT_L0_SHARD_ABS_MAX		8
#define SIFT_L0_SHARD_DIR_MAGIC		0x4C305344U		/* "L0SD" */

#define SIFT_L0_REC_PAGES			4		/* MAX_GENERIC_XLOG_PAGES: pages one WAL record may carry */

#define SIFT_L0_FLUSH_NONE			0U
#define SIFT_L0_FLUSH_ASYNC			1U		/* queue SIFT_MK_SLAB_FLUSH */
#define SIFT_L0_FLUSH_NOW			2U		/* the hard cap: the writer flushes inline */

#define SIFT_L0_PAGE_DATA_OFFSET	SIFT_PAGE_DATA_OFFSET

/* The flags of SiftL0ShardDirData.dir_flags */
#define SIFT_L0_DIR_HEAPREF			UINT16_C(1)		/* the shard slabs are SIFT_L0_SLAB_HEAPREF chains */

typedef struct SiftL0ShardDirData
{
	uint32		dir_magic;
	uint16		nshards;
	uint16		dir_flags;
	uint64		dir_gen;
	BlockNumber draining_slab_blk;
	uint32		pad;
	BlockNumber slab_tail[SIFT_L0_SHARD_ABS_MAX];
	uint16		slab_pages[SIFT_L0_SHARD_ABS_MAX];
	uint32		slab_rows[SIFT_L0_SHARD_ABS_MAX];
	BlockNumber pd_tail[SIFT_L0_SHARD_ABS_MAX];
	uint16		pd_pages[SIFT_L0_SHARD_ABS_MAX];
} SiftL0ShardDirData;

StaticAssertDecl(sizeof(SiftL0ShardDirData) == 152, "SiftL0ShardDirData is 152 bytes");
StaticAssertDecl(sizeof(SiftL0SlabPageData) + sizeof(SiftL0ShardDirData) <= SIFT_PAGE_PAYLOAD,
				 "SiftL0ShardDirData must fit on one SIFT_PT_L0_SLAB page");

/* One batch record of a slab page: this header, then the rows the writer encoded; bytes is a multiple of 8 */
typedef struct SiftL0ShardBatch
{
	uint32		bytes;
	uint32		nrows;
	FullTransactionId xid;
} SiftL0ShardBatch;

StaticAssertDecl(sizeof(SiftL0ShardBatch) == 16, "SiftL0ShardBatch is 16 bytes");

#define SIFT_L0_SL(pg)		((SiftL0SlabPageData *) ((char *) (pg) + SIFT_L0_PAGE_DATA_OFFSET))
#define SIFT_L0_PD(pg)		((SiftPendDelPageData *) ((char *) (pg) + SIFT_L0_PAGE_DATA_OFFSET))
#define SIFT_L0_DIR(pg)		((SiftL0ShardDirData *) ((char *) (pg) + SIFT_L0_PAGE_DATA_OFFSET + sizeof(SiftL0SlabPageData)))

static inline uint32
sift_l0_shard_count(bool unkeyed)
{
	return unkeyed ? SIFT_L0_SHARD_UNKEYED : SIFT_L0_SHARD_MAX;
}

static inline uint32
sift_l0_shard_of(int procno, uint32 nshards)
{
	return (uint32) procno & (nshards - 1);
}

typedef struct SiftL0PageIO
{
	void	   *ctx;
	BlockNumber (*alloc) (void *ctx);	/* a block from the local metadata batch (no Block-0 lock); its page is not formatted */
	void		(*release) (void *ctx, BlockNumber blk);	/* to the FSM quarantine: readers of an older manifest may still hold it */
	void		(*record_begin) (void *ctx);
	char	   *(*record_page) (void *ctx, BlockNumber blk, bool full_image);	/* exclusively locked working copy; at most SIFT_L0_REC_PAGES per record */
	void		(*record_done) (void *ctx);	/* one WAL record for every page of the record; unlocks them */
	void		(*record_abort) (void *ctx);
	const char *(*read_lock) (void *ctx, BlockNumber blk);	/* BUFFER_LOCK_SHARE */
	void		(*read_unlock) (void *ctx, BlockNumber blk);
} SiftL0PageIO;

/* Cache line of one hot partition: where the writers go, so the fast path reads no directory page. Written under the stripes of the shards it changes */
typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftL0ShardShm
{
	RelFileLocator locator;
	uint64		part_id;
	pg_atomic_uint64 dir_gen;
	pg_atomic_uint32 dir_blk;
	pg_atomic_uint32 nshards;
	pg_atomic_uint32 tail[SIFT_L0_SHARD_ABS_MAX];
	pg_atomic_uint32 pd_tail[SIFT_L0_SHARD_ABS_MAX];
	pg_atomic_uint32 flush_wanted;	/* SIFT_L0_FLUSH_* */
} SiftL0ShardShm;

StaticAssertDecl(sizeof(SiftL0ShardShm) == PG_CACHE_LINE_SIZE, "SiftL0ShardShm is one cache line");

typedef struct SiftL0ShardCtx
{
	const SiftL0PageIO *io;
	SiftL0ShardShm *shm;
} SiftL0ShardCtx;

typedef struct SiftL0ScanVisitor
{
	void		(*batch) (void *ctx, const SiftL0ShardBatch *rec);
	void		(*dels) (void *ctx, const SiftL0GcDel *dels, uint32 n);
	void		(*refs) (void *ctx, const SiftL0HeapRef *refs, uint32 n);	/* the references of a SIFT_L0_SLAB_HEAPREF page, oldest page last */
	void		(*page) (void *ctx, BlockNumber blk);
	void	   *ctx;
} SiftL0ScanVisitor;

/* Page images */
extern void sift_l0_slab_format(char *pg, BlockNumber prev, uint16 chain_pages, uint32 chain_rows);
extern bool sift_l0_slab_fits(const char *pg, uint32 bytes);
extern void sift_l0_slab_add(char *pg, const SiftL0ShardBatch *rec);
extern void sift_l0_pd_format(char *pg, BlockNumber prev, uint16 chain_pages);
extern bool sift_l0_pd_fits(const char *pg, uint32 ndels);
extern void sift_l0_pd_add(char *pg, const SiftL0GcDel *dels, uint32 ndels);

/* A directory of nshards fresh shard slabs, or one that adopts a cold single chain as shard 0; returns the directory block */
extern BlockNumber sift_l0_dir_create(const SiftL0PageIO *io, uint32 nshards, BlockNumber draining, uint64 dir_gen, uint16 dir_flags);
extern BlockNumber sift_l0_dir_promote(const SiftL0PageIO *io, BlockNumber cold_tail, uint32 nshards);

/* Points the cache at a directory page (a hot partition met for the first time, or promoted) */
extern void sift_l0_shm_load(const SiftL0PageIO *io, SiftL0ShardShm *shm, BlockNumber dir_blk);

/* The apply function of SiftL0GroupCommit for one shard of one partition */
extern void sift_l0_shard_apply(void *ctx, SiftL0GcSlot *const *items, uint32 n);

/* Reads the anchor (a directory or a cold single slab) and every chain it reaches; follow_draining adds the sealed directory a flush is still writing out */
extern void sift_l0_scan(const SiftL0PageIO *io, BlockNumber anchor, const SiftL0ScanVisitor *v, bool follow_draining);

typedef void (*SiftL0AnchorFn) (void *ctx, BlockNumber new_anchor);

typedef struct SiftL0Flush
{
	const SiftL0PageIO *io;
	SiftL0GroupCommit *gc;
	SiftL0ShardShm *shm;
	BlockNumber old_dir;
	BlockNumber dir_v1;
} SiftL0Flush;

/* Phase 1: seals the active slabs and installs a fresh directory that drains the old one; commit_anchor(new directory) runs with every shard stripe held */
extern void sift_l0_flush_seal(SiftL0Flush *f, SiftL0AnchorFn commit_anchor, void *cctx);
/* Phase 3: a directory v2 with the live slabs of v1 and no draining one; publish(v2) commits the part and the anchor atomically; the pages of the drained slabs are released */
extern void sift_l0_flush_publish(SiftL0Flush *f, SiftL0AnchorFn publish, void *pctx);

#endif							/* SIFT_L0_SHARD_DIR_H */
