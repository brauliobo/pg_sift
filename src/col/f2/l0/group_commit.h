/*
 * l0_group_commit.h - SiftL0GroupCommit: up to 16 concurrent L0 flushes (with their fused PEND_DEL records) of one shard of one
 * partition become one page lock and one WAL record (spec 25 1.4). A stripe of the partition lock table (core/part_lock.h) is the
 * shard's lock and the head of its wait list; the slots are one pool for the cluster. Nothing here knows a page: the caller's apply() does.
 */
#ifndef SIFT_L0_GROUP_COMMIT_H
#define SIFT_L0_GROUP_COMMIT_H

#include "core/part_lock.h"
#include "col/f2/del/pend.h"
#include "core/shm/combiner.h"

#include "storage/block.h"
#include "storage/relfilelocator.h"

#define SIFT_L0_GC_SLOTS			SIFT_COMB_POOL_SLOTS
#define SIFT_L0_GC_MAX_BYTES		3584
#define SIFT_L0_GC_MAX_BATCH		16
#define SIFT_L0_GC_MAX_DELS			32
#define SIFT_L0_GC_MAX_SHARDS		8

#define SIFT_L0_GC_SHARD_MASK		UINT16_C(0x00FF)
#define SIFT_L0_GC_SHARD_HEAPREF	UINT16_C(0x8000)	/* the shard_id of a slot whose slab payload is SiftL0HeapRef[], not a batch record */

#define SIFT_L0_GC_APPLIED			0U
#define SIFT_L0_GC_STALE			1U

typedef SiftPendDelRec SiftL0GcDel;

typedef struct SiftL0GcSlotHdr
{
	SiftCombHdr hdr;
	uint32		slab_bytes;		/* the pre-encoded batch record at payload[0] */
	uint16		ndels;			/* SiftL0GcDel[] at payload[slab_bytes], 0..SIFT_L0_GC_MAX_DELS */
	uint16		shard_id;
	RelFileLocator locator;
	uint64		part_id;
	BlockNumber expect_slab_blk;	/* the anchor (directory or single slab) the writer routed to */
	BlockNumber expect_pd_blk;
} SiftL0GcSlotHdr;

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftL0GcSlot
{
	SiftL0GcSlotHdr h;
	char		pad_hdr[PG_CACHE_LINE_SIZE - sizeof(SiftL0GcSlotHdr)];
	uint8		payload[4096 - PG_CACHE_LINE_SIZE];
} SiftL0GcSlot;

StaticAssertDecl(sizeof(SiftL0GcSlotHdr) <= PG_CACHE_LINE_SIZE, "SiftL0GcSlot header must fit one cache line");
StaticAssertDecl(sizeof(SiftL0GcSlot) == 4096, "SiftL0GcSlot must be exactly 4 KB");
StaticAssertDecl(offsetof(SiftL0GcSlot, payload) == PG_CACHE_LINE_SIZE, "the payload starts on the second cache line");
StaticAssertDecl(SIFT_L0_GC_MAX_BYTES + 0 <= 4096 - PG_CACHE_LINE_SIZE, "a request must fit the payload");
StaticAssertDecl(SIFT_L0_GC_MAX_BATCH <= SIFT_L0_GC_SLOTS, "a batch cannot exceed the pool");

typedef struct pg_attribute_aligned(PG_CACHE_LINE_SIZE) SiftL0GroupCommit
{
	SiftCombPool pool;
	SiftL0GcSlot slots[SIFT_L0_GC_SLOTS];
} SiftL0GroupCommit;

StaticAssertDecl(sizeof(SiftPartStripe) == PG_CACHE_LINE_SIZE, "SiftPartStripe must occupy exactly one 128 B cache line");
StaticAssertDecl(sizeof(SiftL0GroupCommit) == PG_CACHE_LINE_SIZE + SIFT_L0_GC_SLOTS * 4096, "SiftL0GroupCommit is one pool line and 256 KB of slots");

typedef struct SiftL0GcReq
{
	RelFileLocator locator;
	uint64		part_id;
	uint16		shard_id;
	BlockNumber expect_slab_blk;
	BlockNumber expect_pd_blk;
	const void *rec;
	uint32		rec_bytes;
	const SiftL0GcDel *dels;
	uint16		ndels;
} SiftL0GcReq;

/* Under the stripe lock: places items[0..n) (one shard of one partition) and sets items[i]->h.hdr.result to SIFT_L0_GC_*; one WAL record per page set */
typedef void (*SiftL0GcApplyFn) (void *ctx, SiftL0GcSlot *const *items, uint32 n);

#define SIFT_L0_GC_REC(slot)	((const void *) (slot)->payload)
#define SIFT_L0_GC_DELS(slot)	((const SiftL0GcDel *) ((slot)->payload + (slot)->h.slab_bytes))

extern Size sift_l0_gc_shmem_size(void);
extern void sift_l0_gc_init(SiftL0GroupCommit *gc);

/* Returns SIFT_L0_GC_APPLIED, or SIFT_L0_GC_STALE when the writer's anchor is not the partition's any more (read the manifest entry again and retry) */
extern uint32 sift_l0_gc_submit(SiftL0GroupCommit *gc, const SiftL0GcReq *req, SiftL0GcApplyFn apply, void *ctx);

/* The stripes of shards 0..nshards-1 of a partition, deduplicated and locked in ascending order (a slab flush); returns how many are held in held[] */
extern uint32 sift_l0_gc_lock_shards(SiftL0GroupCommit *gc, const RelFileLocator *loc, uint64 part_id, uint32 nshards,
									 uint32 *held);
extern void sift_l0_gc_unlock_shards(SiftL0GroupCommit *gc, const uint32 *held, uint32 nheld);

#endif							/* SIFT_L0_GROUP_COMMIT_H */
