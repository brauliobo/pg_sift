/*
 * core/part_lock.h - the locks of the partitions of a format 2 index.
 *
 * Two tiers. The fast tier is SiftPartLockShm: 256 stripes, a cache line each (the lock and the wait list of the group commit of the L0 shards that hash to it), in a table of the dynamic shared memory registry; the partition of a relation takes the stripe its
 * (relation, partition) hash picks, shared by the appends of the writers of a partition and exclusive by the flush, the split and the compaction that rewrite it. The heavy tier is the page lock
 * of the relation at SIFT_PART_LOCK_BLKNO(part_id): for a hold that spans buffer I/O or waits for a transaction, where the deadlock detector has to see the wait.
 * The block numbers of every virtual lock of a format 2 relation are at the top of the block number space, 0xFFFFE000 and up, above any block a relation has (the 32 bit block number of a
 * relation ends at 0xFFFFFFFE, the relation size limit of the server is far below 0xFFFF0000), so none of them is a block of the relation.
 *
 * The order of acquisition, ascending, never back: the relation-wide F2 locks (maintenance, split), the stripe or page lock of a partition (two partitions in ascending stripe order,
 * sift_part_lock_pair), the build lock of the manifest registry slot, block 0, block 1, leaf, slab and data buffers.
 */
#ifndef SIFT_PART_LOCK_H
#define SIFT_PART_LOCK_H

#include "postgres.h"

#include "core/shm_combiner.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/relfilelocator.h"
#include "utils/rel.h"

#define SIFT_LOCK_REL_BULK_BLKNO	0xFFFFE000U		/* a sift_table row locker's intention or coarse lock of the whole relation (table/f2/f2_lock_esc.h) */
#define SIFT_LOCK_PART_ESC_BASE		0xFFFFE001U		/* the same for one partition, one of SIFT_LOCK_PART_ESC_SLOTS by its number */
#define SIFT_LOCK_PART_ESC_SLOTS	4095U
#define SIFT_LOCK_PART_ESC_BLKNO(part_id) (SIFT_LOCK_PART_ESC_BASE + (BlockNumber) ((part_id) % SIFT_LOCK_PART_ESC_SLOTS))

#define SIFT_PART_LOCK_BASE			0xFFFFF000U
#define SIFT_PART_LOCK_SLOTS		1024U
#define SIFT_PART_LOCK_STRIPES		256U

#define SIFT_F2_MAINT_LOCK_BLKNO	0xFFFFF800U		/* relation-wide maintenance, VACUUM, the orphan sweep */
#define SIFT_F2_LOAD_LOCK_BLKNO		0xFFFFF801U		/* shared by the loads in progress, exclusive for the orphan sweep */
#define SIFT_F2_WORKER_LOCK_BLKNO	0xFFFFF802U		/* held by the maintenance worker of the relation */
#define SIFT_F2_SPLIT_LOCK_BLKNO	0xFFFFF803U		/* the splits of partitions and the rewrites of the scope directory */

#define SIFT_COMPACT_LOCK_BASE		0xFFFFFC00U		/* a partition's compaction lock, one of SIFT_COMPACT_LOCK_SLOTS by its number: it makes compactions of the partition one at a time and stops no writer or reader */
#define SIFT_COMPACT_LOCK_SLOTS		1023U

#define SIFT_PART_LOCK_BLKNO(part_id)	(SIFT_PART_LOCK_BASE + ((part_id) & (SIFT_PART_LOCK_SLOTS - 1)))
#define SIFT_COMPACT_LOCK_BLKNO(part_id) (SIFT_COMPACT_LOCK_BASE + (BlockNumber) ((part_id) % SIFT_COMPACT_LOCK_SLOTS))

StaticAssertDecl(SIFT_LOCK_REL_BULK_BLKNO >= 0xFFFF0000U, "the row lock escalation blocks are above the blocks of a relation");
StaticAssertDecl(SIFT_LOCK_PART_ESC_BASE == SIFT_LOCK_REL_BULK_BLKNO + 1U, "the partition escalation blocks follow the relation's");
StaticAssertDecl(SIFT_LOCK_PART_ESC_BASE + SIFT_LOCK_PART_ESC_SLOTS <= SIFT_PART_LOCK_BASE, "the escalation blocks end before the page locks of the partitions");
StaticAssertDecl(SIFT_PART_LOCK_BASE >= 0xFFFF0000U, "the virtual lock blocks are above the blocks of a relation");
StaticAssertDecl(SIFT_PART_LOCK_BASE + SIFT_PART_LOCK_SLOTS <= SIFT_F2_MAINT_LOCK_BLKNO, "the page locks of the partitions end before the F2 locks");
StaticAssertDecl(SIFT_F2_SPLIT_LOCK_BLKNO < SIFT_COMPACT_LOCK_BASE, "the F2 locks end before the compaction locks");
StaticAssertDecl(SIFT_COMPACT_LOCK_BASE + SIFT_COMPACT_LOCK_SLOTS <= InvalidBlockNumber, "the compaction locks are valid block numbers");
StaticAssertDecl(SIFT_F2_SPLIT_LOCK_BLKNO < InvalidBlockNumber, "the F2 locks are valid block numbers");
StaticAssertDecl((SIFT_PART_LOCK_STRIPES & (SIFT_PART_LOCK_STRIPES - 1)) == 0, "the stripes are a power of two");

typedef SiftCombQueue SiftPartStripe;

typedef struct SiftPartLockShm
{
	SiftPartStripe stripe[SIFT_PART_LOCK_STRIPES];
} SiftPartLockShm;

StaticAssertDecl(sizeof(SiftPartLockShm) == 32 * 1024, "the partition lock stripes are 32 kB");

/* The stripe of a shard of a partition of a relation: the partition lock is the stripe of its shard 0 */
extern uint32 sift_part_shard_stripe(const RelFileLocator *locator, uint64 part_id, uint32 shard);
extern uint32 sift_part_lock_stripe(const RelFileLocator *locator, uint32 part_id);
extern SiftPartStripe *sift_part_stripes(void);

extern void sift_part_lock(const RelFileLocator *locator, uint32 part_id, LWLockMode mode);
extern bool sift_part_lock_conditional(const RelFileLocator *locator, uint32 part_id, LWLockMode mode);
extern void sift_part_unlock(const RelFileLocator *locator, uint32 part_id);

/* Two partitions exclusive (a split, a merge): the lower stripe first, one lock where both are on the same stripe */
extern void sift_part_lock_pair(const RelFileLocator *locator, uint32 part_a, uint32 part_b);
extern void sift_part_unlock_pair(const RelFileLocator *locator, uint32 part_a, uint32 part_b);

/* The heavy tier: the page lock of the partition */
extern void sift_part_page_lock(Relation rel, uint32 part_id, LOCKMODE mode);
extern void sift_part_page_unlock(Relation rel, uint32 part_id, LOCKMODE mode);

#endif
