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

#define SIFT_L0_SHARD_ABS_MAX		SIFT_L0_GC_MAX_SHARDS
#define SIFT_L0_SHARD_DIR_MAGIC		0x4C305344U		/* "L0SD" */

#define SIFT_L0_REC_PAGES			4		/* MAX_GENERIC_XLOG_PAGES: pages one WAL record may carry */
#define SIFT_L0_FRESH_MAX			(SIFT_L0_SHARD_ABS_MAX + 4)	/* the pages a directory and its shard slabs take together, and the successor pages of a record */

#define SIFT_L0_FLUSH_NONE			0U
#define SIFT_L0_FLUSH_ASYNC			1U		/* queue SIFT_MK_SLAB_FLUSH */
#define SIFT_L0_FLUSH_QUEUED		2U		/* a writer has queued it */
#define SIFT_L0_FLUSH_NOW			3U		/* the hard cap: the writer flushes inline */

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

#define SIFT_L0_SL(pg)		((SiftL0SlabPageData *) ((char *) (pg) + SIFT_L0_PAGE_DATA_OFFSET))
#define SIFT_L0_PD(pg)		((SiftPendDelPageData *) ((char *) (pg) + SIFT_L0_PAGE_DATA_OFFSET))
#define SIFT_L0_DIR(pg)		((SiftL0ShardDirData *) ((char *) (pg) + SIFT_L0_PAGE_DATA_OFFSET + sizeof(SiftL0SlabPageData)))

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
	char	   *(*record_page) (void *ctx, BlockNumber blk, SiftPageType type, bool full_image);	/* exclusively locked working copy (a full image is a page of type); at most SIFT_L0_REC_PAGES per record */
	void		(*record_done) (void *ctx);	/* one WAL record for every page of the record; unlocks them */
	void		(*record_abort) (void *ctx);
	const char *(*read_lock) (void *ctx, BlockNumber blk, SiftPageType type);	/* BUFFER_LOCK_SHARE */
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
	pg_atomic_uint32 claimed;		/* the generation of the key above: 0 for an entry never used, odd while the key is being set */
	pg_atomic_uint32 heapref;		/* the shard slabs hold SiftL0HeapRef[] (SIFT_L0_DIR_HEAPREF), not batch records */
	pg_atomic_uint32 active;		/* the backends that hold the entry (sift_l0_hot_find, sift_l0_hot_claim): it is given to no other partition meanwhile */
} SiftL0ShardShm;

StaticAssertDecl(sizeof(SiftL0ShardShm) == PG_CACHE_LINE_SIZE, "SiftL0ShardShm is one cache line");

#define SIFT_L0_HOT_SLOTS			2048
#define SIFT_L0_HOT_PROBES			32

/*
 * The shared table of the hot partitions: a partition has its entry at the hash of its key or in the next SIFT_L0_HOT_PROBES - 1. An entry no backend holds goes to another partition, one
 * whose partition is cold first: the partition it leaves finds its directory in its anchor again and loads it into another entry (sift_l0_settle).
 */
typedef struct SiftL0HotTable
{
	SiftShmLock claim_lock;
	SiftL0ShardShm shm[SIFT_L0_HOT_SLOTS];
} SiftL0HotTable;

extern void sift_l0_hot_init(SiftL0HotTable *t, int tranche);

/* The entry of the partition, held, NULL when it has none (no lock is taken) */
extern SiftL0ShardShm *sift_l0_hot_find(SiftL0HotTable *t, const RelFileLocator *loc, uint64 part_id);

/* The entry of the partition, held, made when it has none; NULL when every entry of the probe is held by others */
extern SiftL0ShardShm *sift_l0_hot_claim(SiftL0HotTable *t, const RelFileLocator *loc, uint64 part_id);

extern void sift_l0_hot_unpin(SiftL0ShardShm *e);

typedef struct SiftL0ShardCtx
{
	const SiftL0PageIO *io;
	SiftL0ShardShm *shm;
} SiftL0ShardCtx;

/* Called with the page share-locked: page for every page the scan reaches (the directories, the slab and pending delete pages), then what the page holds */
typedef struct SiftL0ScanVisitor
{
	void		(*batch) (void *ctx, const SiftL0BatchHeader *rec);
	void		(*dels) (void *ctx, const SiftL0GcDel *dels, uint32 n);
	void		(*refs) (void *ctx, const SiftL0HeapRef *refs, uint32 n);	/* the references of a SIFT_L0_SLAB_HEAPREF page, oldest page last */
	void		(*page) (void *ctx, BlockNumber blk, const char *pg);
	void	   *ctx;
} SiftL0ScanVisitor;

/* Page images */
extern void sift_l0_slab_format(char *pg, BlockNumber prev, uint16 chain_pages, uint32 chain_rows);
extern bool sift_l0_slab_fits(const char *pg, uint32 bytes);
extern void sift_l0_slab_add(char *pg, const SiftL0BatchHeader *rec);
extern void sift_l0_pd_format(char *pg, BlockNumber prev, uint16 chain_pages);
extern bool sift_l0_pd_fits(const char *pg, uint32 ndels);
extern void sift_l0_pd_add(char *pg, const SiftL0GcDel *dels, uint32 ndels);

/*
 * A directory of nshards fresh shard slabs, or one that adopts a cold chain of references as shard 0; returns the directory block. Its min_xid is floor, the oldest transaction that
 * may write to it: the horizon of the table (relfrozenxid) reads it from the entry's anchor, not the pages of the shards.
 */
extern BlockNumber sift_l0_dir_create(const SiftL0PageIO *io, uint32 nshards, BlockNumber draining, uint64 dir_gen, uint16 dir_flags, FullTransactionId floor);
extern BlockNumber sift_l0_dir_promote(const SiftL0PageIO *io, BlockNumber cold_tail, uint32 nshards, FullTransactionId floor);

/* Whether the page is a directory, and a copy of it */
extern bool sift_l0_page_is_dir(const char *pg);
extern SiftL0ShardDirData sift_l0_dir_read(const SiftL0PageIO *io, BlockNumber blk);

/* Points the cache at a directory page (a hot partition met for the first time, or promoted) */
extern void sift_l0_shm_load(const SiftL0PageIO *io, SiftL0ShardShm *shm, BlockNumber dir_blk);

/* The partition is cold: no writer goes to the shards any more (one that is on its way finds its anchor stale) */
extern void sift_l0_shm_unload(SiftL0ShardShm *shm);

/* The apply function of SiftL0GroupCommit for one shard of one partition */
extern void sift_l0_shard_apply(void *ctx, SiftL0GcSlot *const *items, uint32 n);

/* Reads the anchor (a directory or a cold single slab) and every chain it reaches; follow_draining adds the sealed directory a flush is still writing out */
extern void sift_l0_scan(const SiftL0PageIO *io, BlockNumber anchor, const SiftL0ScanVisitor *v, bool follow_draining);

/* Releases every page the scan of the anchor reaches */
extern void sift_l0_dir_release(const SiftL0PageIO *io, BlockNumber anchor, bool follow_draining);

typedef void (*SiftL0AnchorFn) (void *ctx, BlockNumber new_anchor);

typedef struct SiftL0Flush
{
	const SiftL0PageIO *io;
	SiftL0GroupCommit *gc;
	SiftL0ShardShm *shm;
	BlockNumber old_dir;
	BlockNumber dir_v1;
	FullTransactionId floor;
} SiftL0Flush;

/*
 * Phase 1: writes a fresh directory that drains the old one and commits it as the anchor (commit_anchor), then installs it for the writers and seals the slabs of the old one, that no
 * writer reaches any more. Writers that go on into the old directory before the install are read through the new one; an error of the commit leaves the anchor on a directory whose tails
 * still take appends. The shard stripes are held only for the install: the pages of the directories (nshards + 2 for both phases) come from pages the io holds already (sift_href_io_reserve).
 */
extern void sift_l0_flush_seal(SiftL0Flush *f, SiftL0AnchorFn commit_anchor, void *cctx);
/*
 * Phase 3: the writers leave v1 (they wait for the lock of the partition, which the caller holds), then a directory v2 with the live slabs of v1 and no draining one; publish(v2) commits
 * the part and the anchor atomically (InvalidBlockNumber when v1 holds nothing: the partition is cold again), v2 is installed and the pages of the drained slabs are released
 */
extern void sift_l0_flush_publish(SiftL0Flush *f, SiftL0AnchorFn publish, void *pctx);

#ifndef SIFT_HREF_STANDALONE

#include "col/f2/l0/href_fetch.h"

/* The most references one slot of the group commit holds: a statement with more goes to the lock of the partition (a directory is flushed to a part first) */
#define SIFT_L0_HOT_MAX_REFS		(SIFT_L0_GC_MAX_BYTES / sizeof(SiftL0HeapRef))

/* The result of a hot submit when the partition has no directory writers go to (SIFT_L0_GC_APPLIED and SIFT_L0_GC_STALE are the others) */
#define SIFT_L0_NOT_HOT				2U

/* Where a writer that holds the lock of the partition goes on: the cold chain, the directory (it leaves the lock and submits), or nowhere (the shared table is full) */
typedef enum SiftL0Route
{
	SIFT_L0_COLD,
	SIFT_L0_HOT,
	SIFT_L0_REFUSED
} SiftL0Route;

/*
 * Places the references, or the batch records, through the group commit of the partition's directory, with no lock of the partition: *applied of them are placed on return. SIFT_L0_GC_APPLIED when all
 * are, SIFT_L0_GC_STALE when a flush changed the directory (the caller goes on past *applied), SIFT_L0_NOT_HOT when the partition has none.
 */
extern uint32 sift_l0_hot_refs(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftL0HeapRef *refs, uint32 n, uint32 *applied);
extern uint32 sift_l0_hot_batches(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftL0BatchHeader *const *recs, uint32 nrecs, uint32 *applied);

/* Whether the lock of the partition is held by another backend now (a probe that takes it and lets it go when it is free) */
extern bool sift_l0_lock_busy(Relation rel, uint64 part_id);

/* The lock of the partition and its entry e, while the manifest has it with the range [key_lo, key_hi) the writer routed rows by (key_hi NULL: unbounded); false, and no lock, once a split or a merge changed it */
extern bool sift_l0_lock_range(Relation rel, uint64 part_id, const uint8 *key_lo, const uint8 *key_hi, SiftPartitionEntry *e);

/* Under the lock of the partition, e its entry: sends the writer to the directory when the partition is hot (a directory in the entry that the shared table does not know is loaded, a cold chain the writers fight for becomes one) */
extern SiftL0Route sift_l0_settle(Relation rel, uint64 part_id, const uint8 *key_lo, const SiftPartitionEntry *e, bool href, bool busy);

/* Under the lock of the partition: the copy-on-write flush of the directory the entry names; the part and the live directory are committed together */
extern void sift_l0_hot_flush(Relation rel, uint64 part_id, const uint8 *key_lo, BlockNumber anchor);

/* Whether the block is a directory */
extern bool sift_l0_slab_is_dir(Relation rel, uint64 part_id, BlockNumber blk);

/* What a directory and the one it drains hold: their rows, and the stamp of their pages */
extern uint64 sift_l0_dir_rows(Relation rel, uint64 part_id, BlockNumber anchor);
extern SiftChainStamp sift_l0_dir_stamp(Relation rel, uint64 part_id, BlockNumber anchor);

/* Calls visit for every block a directory owns, its own, its slab and pending delete pages, and those of the directory it drains; ctx and the page as the scan has it share-locked */
extern void sift_l0_dir_pages(Relation rel, uint64 part_id, BlockNumber anchor, void (*visit) (void *ctx, BlockNumber blk, const char *pg), void *ctx);
extern void sift_l0_dir_pages_frozen(Relation rel, uint64 part_id, BlockNumber anchor, void (*visit) (void *ctx, BlockNumber blk, const char *pg), void *ctx);

#ifdef SIFT_TESTING
/* Holds the entry of the partition (claimed when it has none) and exits as a FATAL error does; the backends that hold the entry of the partition */
extern void sift_l0_hot_test_hold_fatal(Relation rel, uint64 part_id);
extern uint32 sift_l0_hot_test_holders(Relation rel, uint64 part_id);
#endif

#endif							/* !SIFT_HREF_STANDALONE */

#endif							/* SIFT_L0_SHARD_DIR_H */
